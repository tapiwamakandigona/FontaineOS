#include "keyboard.h"
#include "timer.h"

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

// Fixed Buffer Allocation: Explicit global allocation container with 4-byte tracking alignment bounds
uint8_t disk_test_pad[512] __attribute__((aligned(4)));

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

/*
   The Universal Terminal Scrolling Engine.
   If our text cursor slips past the 25 visible rows (4000 bytes matrix space),
   we shift all character data blocks upward by exactly 1 row (160 bytes)
   and clear out the newly allocated bottom row line container.
*/
static void scroll_screen() {
    volatile char* video_memory = (volatile char*)0xB8000;

    if (cursor_position >= 4000) {
        // Shift rows 1 through 24 upward by copying their data to the row right above them
        for (int i = 0; i < 3840; i++) {
            video_memory[i] = video_memory[i + 160];
        }

        // Blank out the absolute bottom line completely with empty spacer values
        for (int i = 3840; i < 4000; i = i + 2) {
            video_memory[i] = ' ';
            video_memory[i + 1] = 0x07; // Reset to standard style gray color formatting
        }

        // Reposition our hardware prompt cursor to sit safely at the start of the final row line
        cursor_position = 3840;
    }
}

extern "C" void keyboard_handler() {
    uint8_t scancode = inb(0x60);

    if (!(scancode & 0x80)) {
        char character = kbd_us[scancode];

        if (character == '\n') {
            cmd_buffer[cmd_index] = '\0';

            volatile char* video_memory = (volatile char*)0xB8000;
            cursor_position = ((cursor_position / 160) + 1) * 160;
            scroll_screen();

            // Tracking flag token to isolate command validation loops cleanly
            bool command_processed = false;

            if (mystrcmp(cmd_buffer, "help") == true) {
                const char* reply = ">> [FontaineOS Help: Commands are 'help', 'clear', and 'disktest']";
                int i = 0;
                while (reply[i] != '\0') {
                    video_memory[cursor_position] = reply[i];
                    video_memory[cursor_position + 1] = 0x0D; // Purple style font
                    cursor_position = cursor_position + 2;
                    i++;
                }
                cursor_position = ((cursor_position / 160) + 1) * 160;
                scroll_screen();
                command_processed = true;
            }
            else if (mystrcmp(cmd_buffer, "clear") == true) {
                for (int i = 1600; i < 4000; i = i + 2) {
                    video_memory[i] = ' ';
                    video_memory[i + 1] = 0x07;
                }
                cursor_position = 1600;
                command_processed = true;
            }
            else if (mystrcmp(cmd_buffer, "disktest") == true) {
                extern void ata_write_sector(uint32_t lba, const uint8_t* buffer);
                extern void ata_read_sector(uint32_t lba, uint8_t* buffer);

                for (int i = 0; i < 512; i++) disk_test_pad[i] = 0;

                const char* secret = "SUCCESS: STREAMED DATA STRAIGHT FROM HARD DRIVE SECTOR PLATES!";
                int len = 0;
                while (secret[len] != '\0') {
                    disk_test_pad[len] = (uint8_t)secret[len];
                    len++;
                }

                // Send the stable global binary data segment out to Sector 1
                ata_write_sector(1, disk_test_pad);

                // Zero out the pad to prove the subsequent read is genuine
                for (int i = 0; i < 512; i++) disk_test_pad[i] = 0;

                // Stream Sector 1 directly back off the IDE bus into our global buffer
                ata_read_sector(1, disk_test_pad);

                volatile uint8_t* v_pad = (volatile uint8_t*)disk_test_pad;
                int i = 0;
                while (v_pad[i] != 0 && i < 512) {
                    video_memory[cursor_position] = (char)v_pad[i];
                    video_memory[cursor_position + 1] = 0x0D; // Purple text style
                    cursor_position = cursor_position + 2;
                    i++;
                }
                cursor_position = ((cursor_position / 160) + 1) * 160;
                scroll_screen();
                command_processed = true;
            }
            /* The Guarded Fallback catch-all error handler loop router */
            else if (command_processed == false && cmd_buffer[0] != '\0') {
                const char* error_reply = ">> Command not found! Type 'help' for options.";
                int i = 0;
                while (error_reply[i] != '\0') {
                    video_memory[cursor_position] = error_reply[i];
                    video_memory[cursor_position + 1] = 0x0D; // Purple style font
                    cursor_position = cursor_position + 2;
                    i++;
                }
                cursor_position = ((cursor_position / 160) + 1) * 160;
                scroll_screen();
            }

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
