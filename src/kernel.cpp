#include "gdt.h"
#include "idt.h"
#include "timer.h"

extern "C" void kernel_main() {
    /* Step 1: Initialize the Global Descriptor Table (GDT) */
    init_gdt();

    /* Step 2: Initialize the Interrupt Descriptor Table (IDT) */
    init_idt();

    /*
       Step 3: Initialize the Programmable Interval Timer (PIT).
       We pass the parameter 100 to request exactly 100 ticks per second.
    */
    init_timer(100);

    /*
       Step 4: Enable Hardware Interrupts.
       Up until now, the CPU ignored the motherboard. 'sti' instructs the
       internal execution engine to open up the IRQ signal pathways.
    */
    asm volatile("sti");

    /* Print a clean operational baseline message across the screen matrix */
    volatile char* video_memory = (volatile char*)0xB8000;
    const char* message = "FontaineOS Kernel Matrix Live! System Clock Pulses Active.";

    int i = 0;
    while (message[i] != '\0') {
        video_memory[i * 2] = message[i];

        /*
           0x0B represents a brilliant Light Cyan / Cyan color on a Black background.
           We switch to cyan to show the clock-interrupt tracking sequence is operating!
        */
        video_memory[i * 2 + 1] = 0x0B;
        i++;
    }

    /*
       Keep the kernel alive. Every single second, the timer interrupt will
       instantly interrupt this loop to cycle updates onto our screen text buffer.
    */
    while (true) {
        // CPU spins safely here, handing execution over to the timer handler on tick events
    }
}
