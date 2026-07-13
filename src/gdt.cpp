#include "gdt.h"

/*
   Allocate our flat memory array matrix — grown from 3 to 6 descriptors for
   RING-3 (user mode) support:

     [0] = the mandatory null descriptor
     [1] = kernel code, selector 0x08 (ring 0)
     [2] = kernel data, selector 0x10 (ring 0)
     [3] = user code,   selector 0x18 (ring 3) — used from ring 3 as 0x18|3 = 0x1B
     [4] = user data,   selector 0x20 (ring 3) — used from ring 3 as 0x20|3 = 0x23
     [5] = the TSS descriptor, selector 0x28

   The |3 on the user selectors is the Requested Privilege Level (RPL). The CPU
   compares RPL and the descriptor's DPL on every load; a ring-3 selector loaded
   with RPL=0 is rejected with a #GP, so all user selectors we push for iret
   must carry RPL=3 baked into their low two bits.
*/
struct gdt_entry gdt[6];
struct gdt_ptr   gp;

// The single system-wide TSS. We never hardware-task-switch, so one is enough;
// its only live job is handing the CPU a kernel stack on ring3 -> ring0 entry.
struct tss_entry tss;

/*
   Internal utility to break apart 32-bit memory addresses and sizes
   and pack them into the specific bitfields the CPU hardware demands.
*/
void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);

    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access      = access;
}

/*
   Point the TSS at a fresh kernel stack. Called before every drop into ring 3
   (and harmlessly re-callable). See the tss_entry comment block in gdt.h for
   why a stale esp0 means a triple fault on the next timer tick.
*/
void tss_set_kernel_stack(uint32_t stack_top) {
    tss.esp0 = stack_top;
}

/*
   The main initialization function called by our core kernel boot loop.
*/
void init_gdt() {
    // 1. Setup our GDT descriptor tracking pointer structure block
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base  = (uint32_t)&gdt;

    // 2. Gate 0: The Mandatory Intel Null Descriptor (Must be completely clear)
    gdt_set_gate(0, 0, 0, 0, 0);

    // 3. Gate 1: Kernel Code Segment (Base 0, Limit 4GB, Granularity: 4KB pages, 32-bit protected mode)
    // Access 0x9A = Present, Ring 0 (Kernel), Executable, Readable
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // 4. Gate 2: Kernel Data Segment (Base 0, Limit 4GB, Granularity: 4KB pages, 32-bit protected mode)
    // Access 0x92 = Present, Ring 0 (Kernel), Read/Write, Writable
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /*
       5. Gate 3: USER Code Segment. Identical flat 4GB layout to the kernel
       code segment, but access 0xFA instead of 0x9A: the two extra high bits
       are DPL=3 (0x60), marking this segment as loadable by ring-3 code.
       This is what lets 'iret' legally place the CPU at CPL=3.
    */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /*
       6. Gate 4: USER Data Segment. Access 0xF2 = the kernel data byte 0x92
       plus DPL=3. Ring-3 code loads this (as selector 0x23) into ds/es/fs/gs/ss.
    */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /*
       7. Gate 5: the TSS descriptor. Unlike code/data segments this describes
       a SYSTEM structure, so the encoding differs:
         - base/limit point at our actual tss variable (limit = size-1, byte
           granularity, so the gran high nibble is 0x00 — no 4KB scaling here);
         - access 0x89 = Present (0x80) | DPL=0 (0x00) | type 0x9, which is
           "32-bit TSS, available". DPL=0 because only the kernel may ltr it.
    */
    gdt_set_gate(5, (uint32_t)&tss, sizeof(struct tss_entry) - 1, 0x89, 0x00);

    /*
       8. Arm the TSS fields the CPU actually reads on a privilege transition.
       ss0 = 0x10 (our kernel data segment) and esp0 = a valid kernel stack top
       — set for real by tss_set_kernel_stack() before any ring-3 entry, zeroed
       here so a bug that skips that step faults loudly instead of silently
       running on stale memory. iomap_base is set past the end of the segment,
       which the CPU reads as "no I/O permission bitmap": all ring-3 in/out
       instructions #GP, which is exactly the containment we want.
    */
    for (uint32_t i = 0; i < sizeof(struct tss_entry); i++) {
        ((uint8_t*)&tss)[i] = 0;
    }
    tss.ss0        = 0x10;
    tss.esp0       = 0;
    tss.iomap_base = sizeof(struct tss_entry);

    /*
       9. Inform the CPU register hardware where our table sits in RAM.
       We use inline assembly here because standard C++ cannot touch physical registers.
       - 'lgdt' loads our pointer mapping block.
       - The long jump 'ljmp' forces the CPU to instantly drop its old cache and link
         its internal Code Segment register (CS) to our new Gate 1 index (0x08 offset).
       - 'ltr' finally loads the Task Register with selector 0x28 (gate 5), so
         the CPU knows which TSS to consult for ss0/esp0 on ring-3 interrupts.
    */
    asm volatile(
        "lgdt (%0)\n\t"
        "ljmp $0x08, $.reload_segments\n\t"
        ".reload_segments:\n\t"
        "movw $0x10, %%ax\n\t"  // 0x10 is our Gate 2 Data Segment offset address index
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "movw $0x28, %%ax\n\t"  // 0x28 is our Gate 5 TSS descriptor selector index
        "ltr %%ax\n\t"
        : : "r" (&gp) : "ax"
    );
}
