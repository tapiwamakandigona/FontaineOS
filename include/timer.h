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

#endif
