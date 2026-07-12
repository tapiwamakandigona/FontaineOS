#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/*
   We wrap our hooks in extern "C" so the header matches
   the source implementation linkage exactly.
*/
extern "C" {
    void init_timer(uint32_t frequency);
    void timer_handler();
}

#endif
