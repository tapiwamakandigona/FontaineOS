#include "task.h"
#include "heap.h"

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
   The Cooperative Scheduler Context Switcher.
   Saves the active register traces on the current stack, swaps the stack pointer variables,
   and wakes up the next linked target thread line.
*/
void switch_task() {
    if (running_task == nullptr || running_task->next == running_task) return;

    /*
       Mask hardware interrupts while we surgically swap stacks. A timer or keyboard
       IRQ landing in the middle of a half-finished switch would corrupt both stacks.
    */
    asm volatile("cli");

    struct thread_control_block* old_task = running_task;
    struct thread_control_block* next_task = running_task->next;

    running_task = next_task;

    /*
       Hand over to the dedicated assembly routine in boot.s. Doing the pusha/popa
       dance inside a plain C++ function body is undefined behaviour territory: the
       compiler is free to wrap the inline asm with its own prologue/epilogue stack
       traffic, which silently corrupts the fabricated thread frames under -O2.
       A raw assembly function has no compiler-generated frame, so the stack layout
       is exactly what we built in create_thread().
    */
    context_switch(&old_task->esp, next_task->esp);

    // We only get back here once another task switches into us again
    asm volatile("sti");
}
