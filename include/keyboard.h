#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/*
   Wrapping our device driver hooks in an explicit extern "C" matrix
   so the low-level assembly routing stubs can link cleanly.
*/
extern "C" {
    void init_keyboard();
    void keyboard_handler();
}

#endif
