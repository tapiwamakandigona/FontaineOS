#include "timer.h"
#include "idt.h"

// Track the total number of hardware clock pulses since the kernel booted
volatile uint32_t timer_ticks = 0;

/*
   Remaps the Programmable Interrupt Controller (PIC).
   By default, the motherboard routes hardware timer ticks to Interrupt 8, which
   causes collisions with CPU Exceptions. We remap them to start safely at Interrupt 32.
*/
void remap_pic() {
    // ICW1: Start initialization chain in cascade mode
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    // ICW2: Remap Master PIC vector offset to IDT 32, and Slave PIC to IDT 40
    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    // ICW3: Tell Master PIC that there is a slave PIC at IRQ2 (0x04)
    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    // ICW4: Set environments to standard 8086 execution mode flags
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    // Clear operational interrupt masks to unblock data pathways
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

/*
   Configures the motherboard's oscillator chip to tick at a specific frequency (Hz).
*/
void init_timer(uint32_t frequency) {
    // 1. Remap the motherboard's hardware signal routing controller first
    remap_pic();

    // 2. Calculate the divisor rate. The raw hardware clock runs at exactly 1193182 Hz.
    uint32_t divisor = 1193182 / frequency;

    // 3. Send the command byte (0x36) to the PIT Control Port (0x43).
    // This sets the clock chip to Square Wave Generator mode.
    outb(0x43, 0x36);

    // 4. Split the 16-bit divisor into lower and higher bytes, and stream them to Data Port 0x40
    uint8_t low  = (uint8_t)(divisor & 0xFF);
    uint8_t high = (uint8_t)((divisor >> 8) & 0xFF);
    outb(0x40, low);
    outb(0x40, high);
}

/*
   The target C++ routine executed on every single hardware system clock tick.
*/
extern "C" void timer_handler() {
    // Explicit assignment matrix configuration to satisfy C++20 volatile rules
    timer_ticks = timer_ticks + 1;

    // For every 100 ticks (roughly every 1 second at 100Hz frequency), print an evolution message
    if (timer_ticks % 100 == 0) {
        volatile char* video_memory = (volatile char*)0xB8000;

        // Write a tiny character counter indicator tracking cycle updates to the top right corner
        video_memory[158] = '.';
        video_memory[159] = 0x0F; // Bright white style byte
    }

    /*
       End of Interrupt (EOI). We MUST send a 0x20 command confirmation byte to the
       PIC port, otherwise the motherboard will freeze and refuse to fire any more clock ticks.
    */
    outb(0x20, 0x20);
}
