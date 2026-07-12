#include "keyboard.h"
#include "timer.h"

// Reference our global string utility living inside kernel.cpp
extern bool mystrcmp(const char* str1, const char* str2);

inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Initial prompt line starts at Row 10 (1600 bytes offset)
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

extern "C" {
    void init_keyboard() {
        clear_shell_command();
    }

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
            // User hit ENTER. Lock the text string segment buffer.
            cmd_buffer[cmd_index] = '\0';

            volatile char* video_memory = (volatile char*)0xB8000;
            cursor_position = ((cursor_position / 160) + 1) * 160;

            /*
               Atomic Interrupt Level Evaluation:
               We check the strings right here inside the hardware interrupt handler,
               safely protected from thread context switches or memory cache drift!
            */
            if (mystrcmp(cmd_buffer, "help") == true) {
                const char* reply = ">> [FontaineOS Terminal Help: Commands are 'help' and 'clear']";
                int i = 0;
                while (reply[i] != '\0') {
                    video_memory[cursor_position] = reply[i];
                    video_memory[cursor_position + 1] = 0x0D; // Purple style font
                    cursor_position = cursor_position + 2;
                    i++;
                }
                cursor_position = ((cursor_position / 160) + 1) * 160;
            }
            else if (mystrcmp(cmd_buffer, "clear") == true) {
                // Clear text matrices from Row 10 down to the bottom
                for (int i = 1600; i < 4000; i = i + 2) {
                    video_memory[i] = ' ';
                    video_memory[i + 1] = 0x07;
                }
                cursor_position = 1600;
            }
            /* Fallback router: If it doesn't match our valid commands, guide the user */
            else if (cmd_buffer[0] != '\0') {
                const char* error_reply = ">> Command not found! Type 'help' for options.";
                int i = 0;
                while (error_reply[i] != '\0') {
                    video_memory[cursor_position] = error_reply[i];
                    video_memory[cursor_position + 1] = 0x0D; // Purple style font
                    cursor_position = cursor_position + 2;
                    i++;
                }
                cursor_position = ((cursor_position / 160) + 1) * 160;
            }

            // Clean our buffer arrays instantly for the next input prompt line
            clear_shell_command();
        }
        else if (character == '\b' && cmd_index > 0) {
            cmd_index = cmd_index - 1;
            cmd_buffer[cmd_index] = 0;
            cursor_position = cursor_position - 2;
            volatile char* video_memory = (volatile char*)0xB8000;
            video_memory[cursor_position] = ' ';
        }
        else if (character != 0 && cmd_index < 63) {
            cmd_buffer[cmd_index] = character;
            cmd_index = cmd_index + 1;

            volatile char* video_memory = (volatile char*)0xB8000;
            video_memory[cursor_position] = character;
            video_memory[cursor_position + 1] = 0x0E; // Yellow font
            cursor_position = cursor_position + 2;
        }
    }
    outb(0x20, 0x20);
}
