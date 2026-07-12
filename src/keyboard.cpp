#include "keyboard.h"
#include "timer.h"

inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Fixed: Shift the initial input prompt down to Row 10 (offset index 1600 bytes)
// to keep it entirely separate from our system notifications at the top!
uint32_t cursor_position = 1600;

char cmd_buffer[64];
uint32_t cmd_index = 0;
volatile uint8_t command_ready_flag = 0;

/* US Keyboard Map Index */
const char kbd_us[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
 '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0, '*',   0, ' '
};

/*
   Fixed Linkage Matrix:
   We explicitly bind these hooks to 'extern "C"' so our shell loop thread
   can accurately resolve their memory addresses across compilation boundaries!
*/
extern "C" {
    char* get_shell_command() {
        if (command_ready_flag == 1) {
            return cmd_buffer;
        }
        return nullptr;
    }

    void clear_shell_command() {
        for (int i = 0; i < 64; i++) cmd_buffer[i] = 0;
        cmd_index = 0;
        command_ready_flag = 0;
    }
}

extern "C" void keyboard_handler() {
    uint8_t scancode = inb(0x60);

    if (!(scancode & 0x80)) {
        char character = kbd_us[scancode];

        if (character == '\n') {
            // User hit ENTER. Lock the buffer string segment.
            cmd_buffer[cmd_index] = '\0';
            command_ready_flag = 1;

            // FIX: Removed the cursor advancement math line from here!
            // The shell thread (task_beta_routine) now handles advancing the row cleanly.
        }
        else if (character == '\b' && cmd_index > 0) {
            // Handle Backspace operations safely
            cmd_index = cmd_index - 1;
            cmd_buffer[cmd_index] = 0;
            cursor_position = cursor_position - 2;
            volatile char* video_memory = (volatile char*)0xB8000;
            video_memory[cursor_position] = ' ';
        }
        else if (character != 0 && cmd_index < 63) {
            // Append the fresh letter into our active tracking array
            cmd_buffer[cmd_index] = character;
            cmd_index = cmd_index + 1;

            volatile char* video_memory = (volatile char*)0xB8000;
            video_memory[cursor_position] = character;
            video_memory[cursor_position + 1] = 0x0E; // Brilliant Yellow style font
            cursor_position = cursor_position + 2;
        }
    }
    outb(0x20, 0x20);
}
