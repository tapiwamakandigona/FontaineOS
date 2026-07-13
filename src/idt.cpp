#include "idt.h"

// Allocate our system array matrix of exactly 256 interrupt lines
struct idt_entry idt[256];
struct idt_ptr   idp;

/*
   Internal utility to split our 32-bit function memory addresses
   and map them into the required 8-byte Intel hardware gate structures.
*/
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

// Assembly handler hooks matching our boot layer stubs
extern "C" void isr0_handler_stub();
extern "C" void isr13_handler_stub();   // General Protection Fault trap (pushes an ERROR CODE!)
extern "C" void isr80_syscall_stub();   // The int 0x80 system call gate for ring-3 callers
extern "C" void irq0_handler_stub();
extern "C" void irq1_handler_stub(); // Our brand new hardware keyboard assembly stub link!

/*
   Our direct C++ exception target routine for a Division by Zero error.
*/
extern "C" void divide_by_zero_handler() {
    volatile char* video_memory = (volatile char*)0xB8000;
    const char* error_msg = "PANIC: KERNEL TRAPPED A DIVISION BY ZERO EXCEPTION!";

    int i = 0;
    while (error_msg[i] != '\0') {
        video_memory[i * 2] = error_msg[i];
        video_memory[i * 2 + 1] = 0x4F;
        i++;
    }
    while (true) {
        asm volatile("cli; hlt");
    }
}

/*
   The primary initialization function called by our core kernel boot loop.
*/
void init_idt() {
    // 1. Setup our IDT descriptor tracking pointer structure block
    idp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idp.base  = (uint32_t)&idt;

    // 2. Clear out all 256 gates initially by cycling through the memory lines
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // 3. Register Interrupt Line 0 (Division by Zero Exception)
    idt_set_gate(0, (uint32_t)isr0_handler_stub, 0x08, 0x8E);

    /*
       3b. Register Interrupt Line 13 (General Protection Fault). This is the
       CPU's catch-all for privilege violations: a ring-3 task executing cli,
       hlt, an I/O instruction, or loading a forbidden selector all land here.
       Flags 0x8E (DPL=0) are correct even though the FAULTING code is ring 3:
       the DPL of an exception gate constrains who may 'int 13' it on purpose,
       not who the CPU itself may deliver the fault for. Note vector 13 pushes
       an ERROR CODE — the assembly stub must pop it before iret.
    */
    idt_set_gate(13, (uint32_t)isr13_handler_stub, 0x08, 0x8E);

    // 4. Register Interrupt Line 32 (Hardware Timer IRQ 0)
    idt_set_gate(32, (uint32_t)irq0_handler_stub, 0x08, 0x8E);

    /*
       5. Register Interrupt Line 33 (Hardware Keyboard IRQ 1).
       0x08 is our target GDT Kernel Code Segment selector offset.
       0x8E marks this as a Present Kernel Privilege Interception Gate.
    */
    idt_set_gate(33, (uint32_t)irq1_handler_stub, 0x08, 0x8E);

    /*
       5b. Register Interrupt Line 0x80 (128) — the system call gate. The one
       byte that makes user mode useful: flags 0xEE instead of 0x8E. Both mean
       "present 32-bit interrupt gate", but 0xEE carries DPL=3, which tells the
       CPU that software running at CPL=3 is ALLOWED to execute 'int 0x80'.
       With the usual 0x8E a user task touching int 0x80 would #GP instantly —
       DPL=3 on exactly this one gate is the entire kernel/user front door.
       (It stays an INTERRUPT gate, so IF is cleared on entry and the dispatcher
       starts with interrupts masked — no torn syscall entry.)
    */
    idt_set_gate(0x80, (uint32_t)isr80_syscall_stub, 0x08, 0xEE);

    // 6. Inform the CPU register hardware where our table sits in RAM space
    asm volatile("lidt (%0)" : : "r" (&idp));
}
