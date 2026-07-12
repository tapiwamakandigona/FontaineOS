#include "keyboard.h"
#include "timer.h"

/*
   A lightweight, bare-metal string comparison utility.
   Bypasses cross-module linkage optimization barriers.
*/
/*
   A lightweight, bare-metal string comparison utility.
   Fixed: Marked static to prevent global linker namespace collisions!
*/
static bool mystrcmp(const char* str1, const char* str2) {
    int i = 0;
    while (str1[i] != '\0' && str2[i] != '\0') {
        if (str1[i] != str2[i]) {
            return false;
        }
        i++;
    }
    return (str1[i] == '\0' && str2[i] == '\0');
}


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
                // Updated menu guidelines string tracking
                const char* reply = ">> [FontaineOS Help: Commands are 'help', 'clear', and 'disktest']";
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
                for (int i = 1600; i < 4000; i = i + 2) {
                    video_memory[i] = ' ';
                    video_memory[i + 1] = 0x07;
                }
                cursor_position = 1600;
            }
            /*
               The Live Hard Drive Controller Test Gate:
               Explicitly imports our new hardware functions from src/ata.cpp.
            */
                        else if (mystrcmp(cmd_buffer, "disktest") == true) {
                extern void ata_write_sector(uint32_t lba, const uint8_t* buffer);
                extern void ata_read_sector(uint32_t lba, uint8_t* buffer);

                /*
                   Fixed Buffer Allocation:
                   Explicitly attaching array brackets to reserve exactly 512 bytes
                   of stack space so our disk string doesn't smash our memory tracks!
                */
                uint8_t test_buffer[512];
                for (int i = 0; i < 512; i++) test_buffer[i] = 0;

                // Load our message string directly into the first bytes of the container
                const char* secret = "SUCCESS: STREAMED DATA STRAIGHT FROM HARD DRIVE SECTOR PLATES!";
                int len = 0;
                while (secret[len] != '\0') {
                    test_buffer[len] = (uint8_t)secret[len];
                    len++;
                }

                // Push our block straight onto sector 1 of the virtual hard drive
                // Fixed: Pass the clean array handle directly to satisfy block constraints
                ata_write_sector(1, test_buffer);

                for (int i = 0; i < 512; i++) test_buffer[i] = 0;

                // Fixed: Pass the clean array handle directly for raw byte sector extraction
                ata_read_sector(1, test_buffer);



                // Print the verification payload response below our command prompt
                int i = 0;
                while (test_buffer[i] != 0 && i < 512) {
                    video_memory[cursor_position] = (char)test_buffer[i];
                    video_memory[cursor_position + 1] = 0x0D; // Purple text style
                    cursor_position = cursor_position + 2;
                    i++;
                }
                cursor_position = ((cursor_position / 160) + 1) * 160;
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
