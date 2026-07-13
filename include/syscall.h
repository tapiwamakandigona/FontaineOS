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
   then becomes the USER PROGRAM LAUNCHER: an idle loop that waits for the
   shell's 'run <file>' command to submit a loaded program image and drops to
   ring 3 to execute it (see user_program_submit below).
*/
void ring3_demo_task();

/*
   ---------------------------------------------------------------------------
   M4: loading USER PROGRAMS from FontFS.

   ABI for FontaineOS user programs (see user/README-worthy comments in
   user/hello.asm):

     * Flat 32-bit binary, no header. Byte 0 of the file is the first
       instruction executed.
     * Loaded at — and assembled for — the fixed virtual==physical address
       USER_PROG_LOAD_ADDR ('org 0x180000' in NASM). No relocation is done.
     * Runs at CPL=3. Kernel services via int 0x80: eax=number,
       ebx/ecx/edx=args, result in eax (SYS_* numbers above).
     * Must terminate with SYS_EXIT (falling off the end executes whatever
       bytes follow the image and will usually be killed by the GPF handler —
       harmless to the kernel, but sloppy).
     * Stack: the kernel provides ESP pointing at the top of a private 4KB
       stack; programs need not (and should not) set up their own.

   WHY 0x180000 (1.5MB) is the load address — the low-4MB memory map:

     0x000000 .. 0x0FFFFF   real-mode/BIOS/VGA region (VGA text at 0xB8000)
     0x100000 .. ~0x113000  kernel image: .text/.rodata/.data/.bss — the bss
                            includes the boot stack, the FontFS/ATA sector
                            buffers, the shell's 8KB file buffer, the user
                            demo stack and the 8KB TSS esp0 stack
     0x180000 .. 0x182000   USER PROGRAM REGION (this) — 8KB, matching the
                            FontFS max file size, so any readable file fits
     0x200000 .. 0x2FFFFF   free (first RAM the PMM would ever hand out)
     0x300000 .. 0x3FFFFF   kernel heap (init_heap(0x300000, 256 pages)) —
                            source of the scheduler's per-thread 4KB stacks

   The region is safe on three counts:
     1. It sits inside the identity-mapped first 4MB whose pages the M3 code
        flips to user-accessible (make_low_memory_user_accessible), so ring-3
        fetch/load/store at 0x180000 works.
     2. It is inside the PMM's *permanently reserved* low 2MB (init_pmm marks
        pages 0..511 used forever), so even when pmm_alloc_page() gains its
        first caller (it has none today) it can never hand these pages out.
     3. It clears the kernel image end (~0x113000) by >400KB and stays clear
        of the heap (0x300000) — nothing else lives between them.
*/
#define USER_PROG_LOAD_ADDR 0x180000u
#define USER_PROG_MAX_SIZE  8192u   // == FONTFS_MAX_FILE_SIZE: one FontFS region

/*
   Submit a user program image for execution at ring 3. Called by the shell's
   'run' command (keyboard IRQ context) AFTER it has read the file from FontFS:
   copies 'size' bytes to USER_PROG_LOAD_ADDR and flags the launcher task,
   which performs the actual privilege drop OUTSIDE interrupt context (see the
   IRQ-context note in src/syscall.cpp). Returns:
      0  accepted — the launcher will run it shortly
     -1  busy — a previous program is still loaded/running
     -2  bad size — zero bytes or larger than USER_PROG_MAX_SIZE
*/
int user_program_submit(const uint8_t* image, uint32_t size);

#endif
