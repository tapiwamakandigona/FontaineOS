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
   The Thread Control Block (TCB) manages our execution threads.
*/
struct thread_control_block {
    uint32_t             id;             // Unique identification number for the thread task
    uint32_t             esp;            // Track the current top address location of this thread's private stack
    void                 (*entry)();     // The C++ routine this thread executes once it first wakes up
    struct cpu_registers regs;           // Saved snapshot of the processor execution registers
    struct thread_control_block* next;   // Link mapping to the next task in our round-robin execution loop
};

/*
   Exposing our primary multi-tasking engines to the core kernel loops.
*/
void init_multitasking();
void create_thread(void (*thread_entry_function)());
void switch_task();
uint32_t get_thread_count();

#endif
