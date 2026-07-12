#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/*
   Low-level hardware bus communications.
   We place this here in the header file so it can be viewed by all source modules.
*/
inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
   We wrap our main entry function hooks in an explicit extern "C" matrix.
*/
extern "C" {
    void init_timer(uint32_t frequency);
    void timer_handler();
}

/*
   The global hardware clock pulse counter maintained by timer_handler().
   Exposed here so diagnostics (like the shell's 'uptime' command) can read it.
*/
extern volatile uint32_t timer_ticks;

/* The configured PIT frequency: 100 ticks equals exactly one second of wall time. */
#define TIMER_HZ 100

#endif
