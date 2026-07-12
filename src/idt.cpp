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

    // 4. Register Interrupt Line 32 (Hardware Timer IRQ 0)
    idt_set_gate(32, (uint32_t)irq0_handler_stub, 0x08, 0x8E);

    /*
       5. Register Interrupt Line 33 (Hardware Keyboard IRQ 1).
       0x08 is our target GDT Kernel Code Segment selector offset.
       0x8E marks this as a Present Kernel Privilege Interception Gate.
    */
    idt_set_gate(33, (uint32_t)irq1_handler_stub, 0x08, 0x8E);

    // 6. Inform the CPU register hardware where our table sits in RAM space
    asm volatile("lidt (%0)" : : "r" (&idp));
}
