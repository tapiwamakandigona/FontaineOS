#include "timer.h"
#include "idt.h"
#include "task.h"   // schedule() — the preemptive scheduler we drive from this IRQ

// Track the total number of hardware clock pulses since the kernel booted
volatile uint32_t timer_ticks = 0;

/*
   Preemption quantum, in PIT ticks. We only reschedule every SCHEDULE_QUANTUM
   ticks rather than on literally every tick. At 100 Hz a quantum of 1 already
   means a 10 ms time-slice, which is plenty fine-grained for a demo kernel and
   keeps the on-screen tickers visibly advancing instead of thrashing. Bump this
   constant to hand each task a longer slice.
*/
#define SCHEDULE_QUANTUM 1

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

       Crucially we send the EOI *before* the context switch below. If we switched
       away first, the outgoing task's saved frame would resume with the PIC still
       waiting for its acknowledgement and no further timer ticks would ever arrive
       for any task. Acknowledge now, switch after.
    */
    outb(0x20, 0x20);

    /*
       PREEMPTION: drive the scheduler straight from the timer IRQ. This is what
       turns cooperative multitasking into preemptive multitasking — no task has to
       call switch_task()/yield; the PIT tick forcibly rotates the CPU.

       We are running inside IRQ0 with IF=0. schedule() deliberately performs its
       stack swap WITHOUT re-enabling interrupts: doing an sti here (before the
       iret in irq0_handler_stub) would let a nested timer IRQ re-enter and corrupt
       the stacks even though the EOI is already out. The eventual 'iret' restores
       the resumed task's saved EFLAGS (and thus its IF) for us.

       When schedule() switches away, this timer_handler invocation is frozen on the
       outgoing task's stack; it thaws and returns normally the next time that task
       is scheduled back in, unwinding through popa -> iret.
    */
    if (timer_ticks % SCHEDULE_QUANTUM == 0) {
        schedule();
    }
}
