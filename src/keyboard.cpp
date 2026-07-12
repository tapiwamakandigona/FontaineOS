#include "keyboard.h"
#include "timer.h" // Gives us low-level outb access

/*
   Low-level hardware bus communications.
   Reads a single 8-bit byte directly from a motherboard hardware port address bus line.
*/
inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Global text cursor offset tracking location inside our 80x25 screen grid array
uint32_t cursor_position = 160; // Start printing on the second line row space

/*
   The primary layout lookup array mapping standard US Keyboard Scancodes
   safely into universal ASCII text bytes.
*/
const char kbd_us[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', /* Tab */
  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', /* Enter */
    0, /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0, /* Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0, /* Right shift */
  '*',
    0, /* Alt */
  ' ', /* Space bar */
    0, /* Caps lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* F1 - F10 keys */
    0, /* Num lock */
    0, /* Scroll lock */
    0, /* Home key */
    0, /* Up Arrow */
    0, /* Page Up */
  '-',
    0, /* Left Arrow */
    0,
    0, /* Right Arrow */
  '+',
    0, /* End key */
    0, /* Down Arrow */
    0, /* Page Down */
    0, /* Insert Key */
    0, /* Delete Key */
    0, 0, 0,
    0, /* F11 Key */
    0, /* F12 Key */
    0, /* All others undefined */
};

/*
   The target C++ routine executed on every single hardware keyboard interrupt press event.
*/
extern "C" void keyboard_handler() {
    // 1. Read the scancode byte from the physical keyboard hardware data port
    uint8_t scancode = inb(0x60);

    /*
       If the top bit of the byte is set (scancode & 0x80), it means the key
       was RELEASED rather than pressed. We only want to handle key PRESSES.
    */
    if (!(scancode & 0x80)) {
        // Look up the matching readable letter inside our ASCII map array
        char character = kbd_us[scancode];

        // If the key has a valid ASCII mapping, print it onto the VGA text buffer grid
        if (character != 0) {
            volatile char* video_memory = (volatile char*)0xB8000;

            // Render the letter right where our tracking cursor currently sits
            video_memory[cursor_position] = character;
            video_memory[cursor_position + 1] = 0x0E; // Brilliant Yellow style font

            // Advance the cursor index forward by 2 bytes for the next character block
            cursor_position = cursor_position + 2;
        }
    }

    /*
       End of Interrupt (EOI). We MUST send a 0x20 confirmation token byte to the
       Master PIC port, otherwise the keyboard interface bus will lock up completely.
    */
    outb(0x20, 0x20);
}
