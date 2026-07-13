#include "task.h"
#include "heap.h"
#include "timer.h"   // for timer_ticks — sleep()/wake bookkeeping is driven off the PIT clock

/*
   The raw assembly context switching engine living inside boot.s.
   It saves the outgoing task's registers with 'pusha', parks its stack pointer,
   mounts the incoming task's stack pointer, and restores its registers with 'popa'.
*/
extern "C" void context_switch(uint32_t* old_esp_slot, uint32_t new_esp);

// Tracking pointers mapping the active round-robin task looping rings
struct thread_control_block* running_task = nullptr;
struct thread_control_block* task_list_head = nullptr;

uint32_t next_thread_id = 1;

/*
   Initializes our multitasking environment by mapping our main
   kernel timeline execution branch into a permanent primary Task 0 node.
*/
void init_multitasking() {
    struct thread_control_block* kernel_task = (struct thread_control_block*)kmalloc(sizeof(struct thread_control_block));

    kernel_task->id = 0;
    kernel_task->esp = 0; // The main kernel already uses our primary 16KB system stack pool
    kernel_task->entry = nullptr;
    /*
       Task 0 (the kernel/idle timeline) is the task currently on the CPU and it
       NEVER sleeps. Keeping it permanently runnable is our guard against the
       "everyone is asleep" deadlock: the scheduler can always fall back to it.
    */
    kernel_task->state = THREAD_RUNNING;
    kernel_task->wake_tick = 0;
    kernel_task->next = kernel_task; // Initially loop back into itself

    running_task = kernel_task;
    task_list_head = kernel_task;
}

/*
   The very first code a brand new thread executes after its maiden context switch.
   We re-enable hardware interrupts (the switch runs with them masked), jump into the
   thread's real entry routine, and keep yielding forever if that routine ever returns
   so a finished thread can never fall off the bottom of its stack.
*/
extern "C" void thread_bootstrap() {
    asm volatile("sti");
    if (running_task != nullptr && running_task->entry != nullptr) {
        running_task->entry();
    }
    while (true) {
        switch_task();
    }
}

/*
   Dynamically spawns a brand new parallel execution thread.
   Carves out a private stack workspace frame and links it into our running task chain ring.
*/
void create_thread(void (*thread_entry_function)()) {
    struct thread_control_block* new_thread = (struct thread_control_block*)kmalloc(sizeof(struct thread_control_block));

    /*
       Allocate an isolated 4KB private stack buffer area from our virtual heap pool.
       (1KB was dangerously small: every hardware interrupt also lands on the active
       task's stack, so we give each thread a full page of breathing room.)
    */
    uint32_t private_stack_buffer = (uint32_t)kmalloc(4096);
    uint32_t private_stack_top = private_stack_buffer + 4096; // Stacks grow downward in x86 memory map layouts

    new_thread->id = next_thread_id;
    next_thread_id = next_thread_id + 1;
    new_thread->entry = thread_entry_function;

    // A freshly fabricated thread is immediately eligible to run.
    new_thread->state = THREAD_READY;
    new_thread->wake_tick = 0;

    /*
       Fabricate the exact stack image 'context_switch' expects to find:

         [top - 4]              return address -> thread_bootstrap
         [top - 36 .. top - 8]  a zeroed 32-byte 'pusha' register frame (8 registers)

       When the scheduler switches here for the first time, 'popa' pops the 8 zeroed
       registers and 'ret' lands cleanly inside thread_bootstrap, which fires the entry.
       (The old code reserved only 28 bytes, but 'pusha' stores 8 registers = 32 bytes,
       so the first 'popa' consumed the entry address itself and derailed the stack.)
    */
    private_stack_top = private_stack_top - 4;
    *(uint32_t*)private_stack_top = (uint32_t)thread_bootstrap;

    private_stack_top = private_stack_top - 32; // 8 registers * 4 bytes each = 32 bytes frame
    for (int i = 0; i < 8; i++) {
        ((uint32_t*)private_stack_top)[i] = 0;
    }

    new_thread->esp = private_stack_top;

    // Link the new thread node securely into our global circular round-robin task list loop ring
    new_thread->next = task_list_head->next;
    task_list_head->next = new_thread;
}

/*
   Walk the circular task ring and report how many execution threads are registered.
*/
uint32_t get_thread_count() {
    if (task_list_head == nullptr) return 0;
    uint32_t count = 1;
    struct thread_control_block* node = task_list_head->next;
    while (node != task_list_head) {
        count = count + 1;
        node = node->next;
    }
    return count;
}

/*
   Pick the next runnable task in round-robin order, starting just after 'from'
   and wrapping once around the whole ring (so 'from' itself is considered last).

   Along the way any SLEEPING task whose alarm has fired (wake_tick <= timer_ticks)
   is promoted back to READY — this is where sleeping tasks re-enter the schedule.
   Returns the first READY/RUNNING task found, or nullptr if literally everyone is
   asleep (which cannot happen in practice because task 0 never sleeps).
*/
static struct thread_control_block* pick_next_runnable(struct thread_control_block* from) {
    struct thread_control_block* node = from->next;
    uint32_t ring_size = get_thread_count();

    for (uint32_t scanned = 0; scanned < ring_size; scanned++) {
        // Fire any expired sleep alarm before deciding on eligibility.
        if (node->state == THREAD_SLEEPING && node->wake_tick <= timer_ticks) {
            node->state = THREAD_READY;
        }
        if (node->state != THREAD_SLEEPING) {
            return node; // first task that is READY or RUNNING wins this round
        }
        node = node->next;
    }
    return nullptr; // everyone asleep — see task 0 guard in init_multitasking()
}

/*
   The shared low-level switch core used by BOTH the cooperative path
   (switch_task/sleep) and the preemptive IRQ path (schedule).

   IMPORTANT: this routine deliberately does NOT touch the interrupt flag. Its
   callers own the IF policy, because the two paths differ:
     - cooperative callers wrap it in cli/sti;
     - the IRQ path runs with interrupts already masked and lets the eventual
       'iret' restore IF — re-enabling interrupts here would allow a nested timer
       IRQ (EOI is already sent) to re-enter and corrupt the stacks.

   We only demote the outgoing task RUNNING->READY. A task that has already marked
   itself SLEEPING (via sleep()) must keep that state, so we leave non-RUNNING
   states untouched.
*/
static void switch_into(struct thread_control_block* next_task) {
    struct thread_control_block* old_task = running_task;

    if (old_task->state == THREAD_RUNNING) {
        old_task->state = THREAD_READY;
    }
    next_task->state = THREAD_RUNNING;
    running_task = next_task;

    /*
       Hand over to the dedicated assembly routine in boot.s. Doing the pusha/popa
       dance inside a plain C++ function body is undefined behaviour territory: the
       compiler is free to wrap the inline asm with its own prologue/epilogue stack
       traffic, which silently corrupts the fabricated thread frames under -O2.
       A raw assembly function has no compiler-generated frame, so the stack layout
       is exactly what we built in create_thread(). Because every task enters and
       leaves through this SAME routine, cooperative and preemptive frames stay
       perfectly symmetric.
    */
    context_switch(&old_task->esp, next_task->esp);
}

/*
   The Cooperative Scheduler Context Switcher (still used by thread_bootstrap's
   fall-off guard and available to any code that wants to yield explicitly).
   Runs from ordinary task context, so it masks interrupts around the swap and
   re-enables them once it is switched back in.
*/
void switch_task() {
    if (running_task == nullptr || running_task->next == running_task) return;

    /*
       Mask hardware interrupts while we surgically swap stacks. A timer or keyboard
       IRQ landing in the middle of a half-finished switch would corrupt both stacks.
    */
    asm volatile("cli");

    struct thread_control_block* next_task = pick_next_runnable(running_task);
    if (next_task != nullptr && next_task != running_task) {
        switch_into(next_task);
    }

    // We only get back here once another task switches into us again
    asm volatile("sti");
}

/*
   The PREEMPTIVE scheduler, invoked from inside the timer IRQ (see timer_handler).
   This is what makes multitasking preemptive: no task has to yield — the PIT tick
   forcibly rotates the CPU to the next runnable task.

   RE-ENTRANCY CONTRACT (get this wrong and you triple-fault):
     * We are already inside IRQ0 with IF=0, so we must NOT execute sti/cli here.
       The task we switch AWAY from resumes later back through timer_handler ->
       popa -> iret, and it is that iret which restores its saved EFLAGS (IF=1).
     * The PIC EOI has ALREADY been sent by timer_handler before calling us, so
       tasks we switch to still keep receiving timer ticks. We do not re-send it.
     * Because IF stays 0 across the whole switch, no nested timer IRQ can fire
       mid-swap even though the EOI is out — the stacks cannot be re-entered.
*/
extern "C" void schedule() {
    if (running_task == nullptr || running_task->next == running_task) return;

    struct thread_control_block* next_task = pick_next_runnable(running_task);
    if (next_task != nullptr && next_task != running_task) {
        switch_into(next_task);
    }
}

/*
   The sleep(ticks) primitive. Parks the calling task for 'ticks' PIT ticks
   (100 ticks = 1 second at 100 Hz), then reschedules so a runnable task runs in
   its place. The task resumes here only once the scheduler wakes it — i.e. once
   timer_ticks has advanced past wake_tick and pick_next_runnable promotes it back
   to READY, then some later switch mounts it again.

   Runs from ordinary task context (not the IRQ), so like switch_task it fences the
   swap with cli/sti. Note switch_into() will NOT clobber our SLEEPING state because
   it only demotes tasks that are still RUNNING.
*/
void sleep(uint32_t ticks) {
    if (running_task == nullptr) return;

    asm volatile("cli");

    running_task->wake_tick = timer_ticks + ticks;
    running_task->state = THREAD_SLEEPING;

    struct thread_control_block* next_task = pick_next_runnable(running_task);
    if (next_task != nullptr && next_task != running_task) {
        switch_into(next_task);
    }

    // Control returns here after we are woken and re-scheduled onto the CPU.
    asm volatile("sti");
}
