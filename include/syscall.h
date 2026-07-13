#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/*
   The FontaineOS system call numbers. Ring-3 code places one of these in eax,
   its arguments in ebx/ecx/edx, executes 'int 0x80', and receives the result
   back in eax — the classic 32-bit Linux-flavoured convention, chosen because
   it is the one every osdev reference documents.

     SYS_WRITE : ebx = pointer to a NUL-terminated string, ecx = VGA color byte.
                 Prints the string on the kernel's syscall console rows.
     SYS_READ  : STUB in this milestone — always returns 0 ("no input"). A real
                 implementation needs a blocking keyboard queue shared with the
                 shell driver, which is out of scope here; see the dispatcher.
     SYS_YIELD : cooperatively hands the CPU to the next runnable task.
     SYS_EXIT  : terminates the calling user task permanently and returns the
                 CPU to the kernel context that launched it. Never returns.
*/
#define SYS_WRITE 1
#define SYS_READ  2
#define SYS_YIELD 3
#define SYS_EXIT  4

/*
   The C++ demultiplexer behind the int 0x80 gate. Called by isr80_syscall_stub
   in boot.s with the caller's eax/ebx/ecx/edx as plain stack arguments; the
   return value is planted back into the caller's eax by the stub.
*/
extern "C" uint32_t syscall_dispatcher(uint32_t num, uint32_t arg1, uint32_t arg2, uint32_t arg3);

/*
   The C++ General Protection Fault reporter behind IDT vector 13. Receives the
   CPU-pushed error code and the CS the fault occurred in (whose low two bits
   are the faulting privilege level). Kills the offending user task if the
   fault came from ring 3; halts the machine if the kernel itself faulted.
*/
extern "C" void gpf_handler(uint32_t error_code, uint32_t faulting_cs);

/*
   The ring-3 demonstration task. Spawned by kernel_main as an ordinary kernel
   thread; it prepares the TSS + page permissions, drops into user mode twice
   (once for the happy-path syscall demo, once to provoke and survive a GPF),
   then parks forever.
*/
void ring3_demo_task();

#endif
