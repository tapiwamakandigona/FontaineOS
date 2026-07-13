#include "syscall.h"
#include "gdt.h"
#include "task.h"

/*
   ============================================================================
   FontaineOS RING-3 (user mode) support: the syscall dispatcher, the GPF
   reporter, and the demonstration user task.

   The demo model used here is the CONTROLLED-JUMP model: an ordinary scheduled
   kernel thread (ring3_demo_task) drops into ring 3 with enter_usermode(), the
   user program runs at CPL=3 until it retires via sys_exit (or is killed by
   the GPF handler), and control lands back in that same kernel thread as if
   enter_usermode had simply returned. The alternative — making ring-3 tasks
   first-class citizens of the round-robin ring with their own TCB flavour —
   would mean teaching create_thread/context_switch to fabricate and park
   iret-to-ring3 frames, a much more invasive change to the proven M1
   scheduler. Crucially the controlled jump loses almost nothing: because
   enter_usermode raises IF in the fabricated EFLAGS, the ring-3 code IS still
   fully preemptible — every PIT tick during a user slice performs a real
   ring3 -> ring0 privilege transition through the TSS esp0 stack, rotates the
   other tasks, and later irets back to CPL=3. The scheduler is exercised by
   user mode; it just isn't restructured for it.

   ONE-USER-CONTEXT INVARIANT: there is a single TSS esp0 kernel stack below.
   That is safe precisely because at most one user context ever exists at a
   time (the frames a preemption or syscall leaves on the esp0 stack are always
   fully unwound by iret before the same — only — user context can re-enter).
   Running two ring-3 tasks concurrently would require a per-task esp0 swap in
   the scheduler; documented here so nobody adds a second user task casually.
   ============================================================================
*/

// The assembly trampolines living in boot.s.
extern "C" void enter_usermode(uint32_t user_eip, uint32_t user_stack_top);
extern "C" void kernel_reentry(); // never returns: resurrects the enter_usermode caller

/*
   The dedicated kernel stack the CPU switches onto (via TSS ss0:esp0) whenever
   an interrupt or int 0x80 arrives while the processor is at CPL=3. 8KB —
   comfortably enough for a syscall dispatch plus a nested timer IRQ, without
   nibbling at the boot stack or a task's private 4KB stack.
*/
static uint8_t ring0_syscall_stack[8192] __attribute__((aligned(16)));

/*
   The user task's stack. A static buffer (inside the identity-mapped first
   4MB) rather than a kmalloc allocation, so the demo cannot be derailed by
   heap state; ring 3 can touch it once the U/S page bits are flipped below.
*/
static uint8_t user_demo_stack[4096] __attribute__((aligned(16)));

/*
   ----------------------------------------------------------------------------
   Paging: why user mode needs a permission flip.

   init_vmm() (src/vmm.cpp — deliberately NOT modified, it is proven M0 code)
   identity-maps the first 4MB with flags 0x03 = Present | Read-Write |
   SUPERVISOR-ONLY. The x86 U/S check consults BOTH levels: the page directory
   entry AND the page table entry must carry bit 2 (0x04) for a CPL=3 access
   to succeed. With the current tables, the very first instruction fetch after
   iret-to-ring3 would page-fault. So before the first drop to ring 3 we OR the
   User bit into PDE 0 and all 1024 PTEs, then reload cr3 to flush the TLB.

   Honest caveat: this makes the ENTIRE first 4MB (kernel code, VGA, heap)
   readable/writable from ring 3. Memory isolation is a paging milestone, not
   this one — here the protection being demonstrated is the PRIVILEGE-LEVEL
   kind: ring 3 cannot cli/hlt/in/out or skip the syscall gate, and the GPF
   demo below proves it.
   ----------------------------------------------------------------------------
*/
extern uint32_t page_directory[1024];  // defined in src/vmm.cpp
extern uint32_t first_page_table[1024];

static void make_low_memory_user_accessible() {
    page_directory[0] = page_directory[0] | 0x04;      // User bit on the PDE
    for (uint32_t i = 0; i < 1024; i++) {
        first_page_table[i] = first_page_table[i] | 0x04; // User bit on every PTE
    }
    // Reload cr3 with the same directory: the cheap full-TLB flush, so no
    // stale supervisor-only translation lingers in the CPU's cache.
    asm volatile("mov %0, %%cr3" : : "r"(page_directory));
}

/*
   ----------------------------------------------------------------------------
   The syscall console: a tiny VGA writer owned by the syscall layer.

   shell_print_line() in keyboard.cpp is static (private to the shell) and its
   cursor lives in the scrolling shell area; sys_write output must not fight
   with it. So the syscall layer owns rows 5-7 of the screen (byte offsets
   800/960/1120), between the Task Gamma ticker (row 4) and the FontFS status
   line (row 8). Three lines is exactly the demo's budget: the ring-3 hello,
   the sys_exit confirmation, and the caught-GPF report. If the budget is
   exhausted we clamp instead of overrunning the FontFS line.

   No large stack arrays anywhere in here — this runs in interrupt context on
   the fixed-size esp0 stack.
   ----------------------------------------------------------------------------
*/
static uint32_t syscall_console_cursor = 800; // row 5 of the 80x25 VGA text matrix

/*
   Blank rows 5-7 and rewind the syscall console cursor. Called by the program
   launcher before every 'run' so each user program starts with the full
   three-row budget instead of inheriting whatever the boot demo (or the
   previous program) left behind — this is the "state cleaned up between runs"
   half of the loader contract.
*/
static void syscall_console_reset() {
    volatile char* video_memory = (volatile char*)0xB8000;
    for (uint32_t i = 800; i < 1280; i += 2) {
        video_memory[i] = ' ';
        video_memory[i + 1] = 0x07;
    }
    syscall_console_cursor = 800;
}

static void syscall_console_write(const char* text, uint8_t color) {
    if (syscall_console_cursor >= 1280) return; // never collide with the FontFS row
    volatile char* video_memory = (volatile char*)0xB8000;
    uint32_t pos = syscall_console_cursor;
    int i = 0;
    while (text[i] != '\0' && pos < syscall_console_cursor + 160) {
        video_memory[pos] = text[i];
        video_memory[pos + 1] = color;
        pos = pos + 2;
        i++;
    }
    syscall_console_cursor = syscall_console_cursor + 160; // next full row
}

/*
   ----------------------------------------------------------------------------
   The system call dispatcher — the C++ half of the int 0x80 gate.

   Entered from isr80_syscall_stub with interrupts masked (0xEE is an interrupt
   gate) and kernel data segments mounted, standing on the TSS esp0 stack.
   ----------------------------------------------------------------------------
*/
extern "C" uint32_t syscall_dispatcher(uint32_t num, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    (void)arg3; // reserved for future three-argument syscalls

    switch (num) {
        case SYS_WRITE: {
            /*
               arg1 = user pointer to a NUL-terminated string, arg2 = color.
               With the flat/shared address space of this milestone the kernel
               can dereference the user pointer directly; a real kernel with
               separate address spaces would validate + copy it first.
            */
            syscall_console_write((const char*)arg1, (uint8_t)(arg2 & 0xFF));
            return 0;
        }

        case SYS_READ: {
            /*
               STUB (documented in the milestone brief as acceptable): a real
               sys_read needs a blocking character queue fed by the keyboard
               IRQ, but keyboard.cpp's buffer is owned by the interactive shell
               and stealing bytes from it would break the very shell this
               milestone must not regress. Returns 0 = "no bytes available".
            */
            return 0;
        }

        case SYS_YIELD: {
            /*
               Cooperative reschedule on behalf of the ring-3 caller. From the
               scheduler's point of view the current task is still the launcher
               thread (ring3_demo_task) — its TCB esp simply gets parked
               pointing into the esp0 stack, and when the round-robin returns
               to it the unwind runs back through the syscall stub's iret,
               resuming the user code at CPL=3 right after 'int 0x80'.
            */
            switch_task();
            return 0;
        }

        case SYS_EXIT: {
            /*
               Terminate the user task for good. We do NOT iret back to ring 3;
               instead we abandon the whole syscall frame and jump through
               kernel_reentry, which remounts the kernel stack parked by
               enter_usermode and 'returns' from it. arg1 (the exit code) is
               ignored beyond this point — there is no wait()ing parent yet.
            */
            kernel_reentry();
            __builtin_unreachable();
        }

        default:
            return (uint32_t)-1; // unknown syscall number
    }
}

/*
   ----------------------------------------------------------------------------
   The General Protection Fault reporter — the C++ half of IDT vector 13.

   Receives the CPU's error code and the CS of the faulting code. The low two
   bits of that CS are the privilege level the fault happened at — RPL=3 means
   a user task tried something privileged (cli, hlt, in/out, a ring-0 selector)
   and the CPU vetoed it. That is user mode WORKING, not a kernel bug: we
   report it on the syscall console and retire the offending task through
   kernel_reentry, exactly like a forced sys_exit. The kernel, shell, and
   scheduler keep running — no triple fault.

   A GPF at CPL=0 is a genuine kernel bug; there is nothing sane to resume, so
   we paint a panic banner and halt.
   ----------------------------------------------------------------------------
*/
extern "C" void gpf_handler(uint32_t error_code, uint32_t faulting_cs) {
    if ((faulting_cs & 3) == 3) {
        (void)error_code; // 0 for privileged-instruction faults; selector-related otherwise
        syscall_console_write("GPF caught: privileged op from ring 3 -- task killed, kernel fine", 0x0C);
        kernel_reentry();          // retire the user task; never returns
        __builtin_unreachable();
    }

    // Ring-0 fault: unrecoverable. Red banner on the top line, then halt hard.
    volatile char* video_memory = (volatile char*)0xB8000;
    const char* panic_msg = "PANIC: GENERAL PROTECTION FAULT IN KERNEL (RING 0)!";
    int i = 0;
    while (panic_msg[i] != '\0') {
        video_memory[i * 2] = panic_msg[i];
        video_memory[i * 2 + 1] = 0x4F;
        i++;
    }
    while (true) {
        asm volatile("cli; hlt");
    }
}

/*
   ----------------------------------------------------------------------------
   The user programs. These are ordinary functions inside the kernel image
   (flat shared address space — see the paging note above), but they EXECUTE at
   CPL=3: no cli, no hlt, no port I/O, no privileged register access. Their
   only doors back into the kernel are int 0x80 and faulting.
   ----------------------------------------------------------------------------
*/

// The int 0x80 invocation helper: eax=number, ebx/ecx/edx=args, result in eax.
static inline uint32_t user_syscall(uint32_t num, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t result;
    asm volatile("int $0x80"
                 : "=a"(result)
                 : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
                 : "memory");
    return result;
}

/*
   User program #1 — the happy path. Reads its own CS register and embeds the
   value in its hello line: the CPU physically cannot lie about CS, so seeing
   "CS=0x1B RPL=3" on screen IS the proof the code runs at CPL=3 (0x1B = user
   code selector 0x18 with RPL bits 3). Then exercises sys_yield and retires
   via sys_exit.
*/
static void user_demo_main() {
    char message[64];
    const char* head = "Hello from ring 3 (CPL=3)  CS=0x";
    int pos = 0;
    while (head[pos] != '\0') { message[pos] = head[pos]; pos++; }

    uint16_t cs_value;
    asm volatile("mov %%cs, %0" : "=r"(cs_value)); // reading CS is unprivileged
    const char* hex_digits = "0123456789ABCDEF";
    message[pos++] = hex_digits[(cs_value >> 4) & 0xF];
    message[pos++] = hex_digits[cs_value & 0xF];

    const char* tail = "  RPL=";
    int t = 0;
    while (tail[t] != '\0') { message[pos++] = tail[t]; t++; }
    message[pos++] = (char)('0' + (cs_value & 3)); // the live privilege bits
    message[pos] = '\0';

    user_syscall(SYS_WRITE, (uint32_t)message, 0x0B, 0); // cyan hello line, row 5
    user_syscall(SYS_YIELD, 0, 0, 0);                    // give the CPU away and come back
    user_syscall(SYS_EXIT, 0, 0, 0);                     // retire — never returns
}

/*
   User program #2 — the crash-test dummy. Executes 'cli', a privileged
   instruction (IOPL=0, CPL=3), which the CPU refuses with a General Protection
   Fault. The GPF handler above reports it and kills this task; the sys_write
   afterwards must never appear on screen.
*/
static void user_gpf_main() {
    asm volatile("cli"); // BOOM — #GP(0) delivered to gpf_handler
    user_syscall(SYS_WRITE, (uint32_t)"UNREACHABLE: cli did not trap!", 0x4F, 0);
    user_syscall(SYS_EXIT, 1, 0, 0);
}

/*
   ----------------------------------------------------------------------------
   M4: the user-program run request shared between the shell and the launcher.

   WHY THE HANDOFF EXISTS — the IRQ-context problem. The shell parser runs
   INSIDE the keyboard IRQ handler (keyboard.cpp). Entering ring 3 from there
   would be unsafe on two counts:

     1. EOI ordering: keyboard_handler only acknowledges IRQ 1 to the PIC
        (outb 0x20,0x20) at the very END of the handler. enter_usermode never
        returns to its caller by falling through — control comes back via
        kernel_reentry's stack teleport — so the EOI would only run after the
        program exits, and meanwhile the PIC would hold ALL lower-priority
        IRQs (including the keyboard itself) blocked. Worse, the in-service
        IRQ1 would still be marked pending, wedging keyboard input.
     2. Stack unwinding: the CPU delivered the keyboard interrupt on whatever
        stack was live (a task's 4KB heap stack or the esp0 stack if ring 3
        was interrupted). enter_usermode parks THAT esp for kernel_reentry;
        resuming it later would 'ret' back into a half-finished IRQ frame
        whose iret chain (and pending EOI) no longer matches reality.

   So the shell only LOADS: it reads the file from FontFS (synchronous polling
   ATA from IRQ context is the established, proven pattern — 'cat' does the
   same), copies it into the load region via user_program_submit(), and
   returns from the IRQ normally. The privilege drop happens in the launcher
   task below — an ordinary scheduled kernel thread, exactly the context M3's
   enter_usermode was designed and verified for.

   'volatile' because the flag is written in IRQ context and polled by a task.
   Single flag, no queue: user_program_submit refuses (-1 busy) while a
   program is pending or running, preserving M3's one-user-context invariant.
*/
static volatile uint32_t user_prog_pending = 0; // 0 = idle, 1 = image loaded & waiting

int user_program_submit(const uint8_t* image, uint32_t size) {
    if (user_prog_pending) return -1;                      // one at a time
    if (size == 0 || size > USER_PROG_MAX_SIZE) return -2; // empty or oversized

    /*
       Copy the image into the fixed load region. The copy happens BEFORE the
       pending flag is raised, so the launcher can never observe a torn image.
       Plain byte loop — freestanding kernel, no memcpy linked.
    */
    volatile uint8_t* dst = (volatile uint8_t*)USER_PROG_LOAD_ADDR;
    for (uint32_t i = 0; i < size; i++) dst[i] = image[i];

    user_prog_pending = 1;
    return 0;
}

/*
   ----------------------------------------------------------------------------
   The launcher — an ordinary scheduled kernel thread (created in kernel_main).
   Sequences the whole ring-3 demonstration, then becomes the user-program
   launcher loop for the shell's 'run' command.
   ----------------------------------------------------------------------------
*/
void ring3_demo_task() {
    // One-time paging permission flip so CPL=3 fetches/loads/stores work at all.
    make_low_memory_user_accessible();

    /*
       Arm the TSS with our dedicated interrupt stack. From the instant the
       iret below lands at CPL=3, EVERY timer tick and int 0x80 makes the CPU
       fetch ss0:esp0 from the TSS — this line is what stands between the demo
       and a triple fault.
    */
    tss_set_kernel_stack((uint32_t)ring0_syscall_stack + sizeof(ring0_syscall_stack));

    // Drop 1: the happy path. Returns (via kernel_reentry) once the user task sys_exits.
    enter_usermode((uint32_t)user_demo_main, (uint32_t)user_demo_stack + sizeof(user_demo_stack));
    syscall_console_write("sys_exit OK: ring-3 task retired, kernel context restored", 0x0A);

    // Drop 2: the protection proof. Returns once the GPF handler kills the task.
    enter_usermode((uint32_t)user_gpf_main, (uint32_t)user_demo_stack + sizeof(user_demo_stack));

    /*
       Demo complete. From here on this thread is the USER PROGRAM LAUNCHER:
       it polls the pending flag the shell raises via user_program_submit()
       and performs the ring-3 drop in ordinary task context (see the
       IRQ-context rationale above user_prog_pending). The paging flip and
       TSS esp0 arming from the demo above are still in force; we re-arm the
       esp0 stack before every run anyway so the invariant survives even if a
       future milestone moves the demo elsewhere.

       Between runs the loop sleeps 10 ticks (100ms at 100Hz) — snappy enough
       that 'run' feels immediate, idle enough that the round-robin stays
       lean. State cleanup between runs: the syscall console is reset, the
       user stack top is recomputed (the previous program may have left any
       ESP behind — irrelevant, enter_usermode installs a fresh one), and the
       pending flag is cleared only AFTER the program retired, so a second
       'run' during execution is refused instead of clobbering the image.
    */
    while (true) {
        if (user_prog_pending) {
            syscall_console_reset(); // fresh rows 5-7 for this program's output
            tss_set_kernel_stack((uint32_t)ring0_syscall_stack + sizeof(ring0_syscall_stack));

            // Drop to CPL=3 at the load address. Returns (via kernel_reentry)
            // when the program sys_exits or is killed by the GPF handler.
            enter_usermode(USER_PROG_LOAD_ADDR,
                           (uint32_t)user_demo_stack + sizeof(user_demo_stack));

            user_prog_pending = 0; // region free — the shell may load the next program
        }
        sleep(10);
    }
}
