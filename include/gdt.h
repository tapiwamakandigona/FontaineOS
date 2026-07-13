#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/*
   This structure defines a single 8-byte GDT entry descriptor.
   We use __attribute__((packed)) to prevent the C++ compiler from
   adding secret padding bytes, forcing it to match the exact hardware layout required by the Intel CPU registers.
*/
struct gdt_entry {
    uint16_t limit_low;     // The lower 16 bits of the memory segment limit size
    uint16_t base_low;      // The lower 16 bits of the memory segment base address
    uint8_t  base_middle;   // The next 8 bits of the memory segment base address
    uint8_t  access;        // Access flags determining segment privileges (Code/Data/Kernel/User)
    uint8_t  granularity;   // Size multiplier limits and operational mode bit flags
    uint8_t  base_high;     // The final 8 bits of the memory segment base address
} __attribute__((packed));

/*
   This special pointer structure tells the CPU hardware where our GDT array lives
   and exactly how large it is.
*/
struct gdt_ptr {
    uint16_t limit;         // The total size of the GDT array minus 1 byte
    uint32_t base;          // The linear physical memory address where the array starts
} __attribute__((packed));

/*
   The Task State Segment (TSS). On x86 this structure was originally designed
   for full hardware task switching, but modern kernels (ours included) use it
   for exactly ONE thing: telling the CPU which stack to switch to when an
   interrupt or syscall arrives while the processor is running RING-3 code.

   Why this is non-negotiable: when an interrupt fires at CPL=3 the CPU refuses
   to push the interrupt frame onto the (untrusted, possibly corrupt) user
   stack. Instead it reads ss0:esp0 out of the TSS pointed at by the Task
   Register and lands on THAT stack before pushing SS/ESP/EFLAGS/CS/EIP. If
   ss0/esp0 are garbage the very first timer tick during a ring-3 slice pushes
   onto a junk address, that faults, the fault handler also can't push -> a
   triple fault and instant reboot. So the TSS must be loaded (ltr) and armed
   with a known-good kernel stack BEFORE we ever iret into ring 3.

   Only ss0/esp0 (and the iomap base) matter for us; the rest of the fields
   exist because the hardware layout demands them, and stay zero.
*/
struct tss_entry {
    uint32_t prev_tss;      // Link field for hardware task nesting (unused, 0)
    uint32_t esp0;          // THE kernel stack pointer loaded on a ring3 -> ring0 transition
    uint32_t ss0;           // THE kernel stack segment loaded alongside esp0 (our 0x10)
    uint32_t esp1, ss1;     // Ring-1 stack (unused — we only have rings 0 and 3)
    uint32_t esp2, ss2;     // Ring-2 stack (unused)
    uint32_t cr3;           // Page directory for hardware task switches (unused)
    uint32_t eip, eflags;   // Saved execution state for hardware task switches (unused)
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; // GP register save area (unused)
    uint32_t es, cs, ss, ds, fs, gs;                 // Segment save area (unused)
    uint32_t ldt;           // Local Descriptor Table selector (unused, 0)
    uint16_t trap;          // Debug trap flag (unused)
    uint16_t iomap_base;    // Offset to the I/O permission bitmap; sizeof(tss) = "no bitmap"
} __attribute__((packed));

/*
   Exposing our primary engine functions to the rest of the kernel system layers.
*/
void init_gdt();

/*
   Re-arm the TSS with a fresh kernel stack top. Any code that is about to drop
   the CPU into ring 3 must make sure esp0 points at a valid, unused kernel
   stack first — that is the stack every subsequent interrupt/syscall from
   ring 3 will land on.
*/
void tss_set_kernel_stack(uint32_t stack_top);

#endif
