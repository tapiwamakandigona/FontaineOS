#ifndef TASK_H
#define TASK_H

#include <stdint.h>

/*
   This structure maps the exact snapshot of the CPU registers
   pushed onto the private thread stack during a context switch.
*/
struct cpu_registers {
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t eip; // The instruction pointer tracking exactly what line of code to execute next
} __attribute__((packed));

/*
   Execution states a task can be in. The scheduler uses these to decide who
   is eligible to run next:

     THREAD_READY    - runnable, waiting for its turn on the CPU.
     THREAD_RUNNING  - the task currently mounted on the CPU (exactly one).
     THREAD_SLEEPING - parked until timer_ticks reaches its wake_tick; the
                       scheduler skips it entirely until then.
*/
enum thread_state {
    THREAD_READY    = 0,
    THREAD_RUNNING  = 1,
    THREAD_SLEEPING = 2,
};

/*
   The Thread Control Block (TCB) manages our execution threads.
*/
struct thread_control_block {
    uint32_t             id;             // Unique identification number for the thread task
    uint32_t             esp;            // Track the current top address location of this thread's private stack
    void                 (*entry)();     // The C++ routine this thread executes once it first wakes up
    struct cpu_registers regs;           // Saved snapshot of the processor execution registers
    uint32_t             state;          // One of thread_state: READY / RUNNING / SLEEPING
    uint32_t             wake_tick;      // If SLEEPING, the timer_ticks value at which to become READY again
    struct thread_control_block* next;   // Link mapping to the next task in our round-robin execution loop
};

/*
   Exposing our primary multi-tasking engines to the core kernel loops.
*/
void init_multitasking();
void create_thread(void (*thread_entry_function)());
void switch_task();
uint32_t get_thread_count();

/*
   The preemptive scheduler entry point, driven from inside the timer IRQ
   (see timer_handler in src/timer.cpp). Unlike switch_task() it must NEVER
   touch the interrupt flag: it already runs with interrupts masked inside the
   IRQ, and the final 'iret' is what restores IF for the task being resumed.
*/
extern "C" void schedule();

/*
   Cooperative sleep primitive: park the calling task for 'ticks' PIT ticks
   (100 ticks = 1 second at 100 Hz), then reschedule so a runnable task takes
   over. The caller resumes only once the timer has advanced far enough.
*/
void sleep(uint32_t ticks);

#endif
