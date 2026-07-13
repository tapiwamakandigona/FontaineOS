#include "keyboard.h"
#include "timer.h"
#include "pmm.h"
#include "task.h"
#include "fontfs.h"
#include "syscall.h"

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

/*
   Prints a full reply line onto the console at the live cursor position,
   then drops the cursor onto a fresh row and scrolls if we ran off the screen.
   Centralizes the copy/paste print loops the command handlers used to repeat.
*/
static void shell_print_line(const char* text, uint8_t color) {
    volatile char* video_memory = (volatile char*)0xB8000;
    int i = 0;
    while (text[i] != '\0') {
        video_memory[cursor_position] = text[i];
        video_memory[cursor_position + 1] = color;
        cursor_position = cursor_position + 2;
        i++;
    }
    cursor_position = ((cursor_position / 160) + 1) * 160;
    scroll_screen();
}

/*
   Renders a 32-bit unsigned number into a caller-supplied character buffer
   (base 10). Freestanding kernels have no sprintf, so we roll our own.
*/
static int uint_to_dec(uint32_t value, char* out) {
    char scratch[10];
    int len = 0;
    do {
        scratch[len] = (char)('0' + (value % 10));
        value = value / 10;
        len++;
    } while (value > 0);
    for (int i = 0; i < len; i++) {
        out[i] = scratch[len - 1 - i];
    }
    out[len] = '\0';
    return len;
}

/*
   Tiny bounded string appender used to compose diagnostic reply lines.
*/
static int append_str(char* dest, int pos, const char* src) {
    int i = 0;
    while (src[i] != '\0' && pos < 159) {
        dest[pos] = src[i];
        pos++;
        i++;
    }
    dest[pos] = '\0';
    return pos;
}

/*
   Shell argument tokenizer.

   The existing shell matched the WHOLE command buffer with mystrcmp, which is
   fine for word-less commands (help, clear, ...) but useless for the new
   filesystem verbs that take arguments ('cat hello', 'write hello world').
   first_token() peels the first whitespace-delimited word off 'src' into
   'out' (bounded, always NUL-terminated) and returns a pointer to the rest of
   the line with the separating spaces skipped. Calling it once gives us
   verb + argument-tail; calling it again on the tail gives us
   filename + text-tail for the 'write' command.
*/
static const char* first_token(const char* src, char* out, int out_max) {
    int i = 0;
    while (src[i] == ' ') i++;              // skip any leading spaces
    int o = 0;
    while (src[i] != '\0' && src[i] != ' ' && o < out_max - 1) {
        out[o] = src[i];
        o++;
        i++;
    }
    out[o] = '\0';
    while (src[i] == ' ') i++;              // skip spaces before the remainder
    return &src[i];
}

/*
   Static scratch for reading a whole file back before printing it. Kept in the
   BSS (not on the IRQ stack) and sized one byte over the max file so we can
   always NUL-terminate before handing it to shell_print_line.
*/
static uint8_t fs_file_buf[FONTFS_MAX_FILE_SIZE + 1] __attribute__((aligned(4)));

/* Static name scratch for the argument-taking filesystem commands. */
static char fs_name_buf[FONTFS_NAME_MAX];

/* fontfs_list() callback: render one "  <name>  (<n> bytes)" line per file. */
static void fs_ls_cb(const char* name, uint32_t size, void* ctx) {
    (void)ctx;
    char reply[160];
    char num[11];
    int pos = append_str(reply, 0, "  ");
    pos = append_str(reply, pos, name);
    pos = append_str(reply, pos, "  (");
    uint_to_dec(size, num);
    pos = append_str(reply, pos, num);
    pos = append_str(reply, pos, " bytes)");
    shell_print_line(reply, 0x0F);
}

extern "C" void keyboard_handler() {
    uint8_t scancode = inb(0x60);

    /*
       Fixed Scancode Gate:
       If the scancode has bit 7 set, it is a key RELEASE (Break Code).
       We return immediately and completely ignore it so it cannot double-trigger our loops!
    */
    if (scancode & 0x80) {
        outb(0x20, 0x20); // Acknowledge interrupt to hardware PIC
        return;
    }

    char character = kbd_us[scancode];

    if (character == '\n') {
        cmd_buffer[cmd_index] = '\0';

        volatile char* video_memory = (volatile char*)0xB8000;
        cursor_position = ((cursor_position / 160) + 1) * 160;
        scroll_screen();

        // Tracking flag token to isolate command validation loops cleanly
        bool command_processed = false;

        /*
           Split the raw command buffer into a verb and an argument tail once,
           up front, so every branch below can use them. Word-less commands
           (help/clear/...) still match because their verb equals the whole
           line; the filesystem verbs additionally consume the argument tail.
        */
        char verb[16];
        const char* argtail = first_token(cmd_buffer, verb, (int)sizeof(verb));

        if (mystrcmp(cmd_buffer, "help") == true) {
            shell_print_line(">> [FontaineOS Help: help, clear, uptime, meminfo, disktest]", 0x0D);
            shell_print_line(">> [FontFS: format, ls, cat <f>, write <f> <text>, touch <f>, rm <f>]", 0x0D);
            shell_print_line(">> [Programs: run <f> -- execute a flat binary from FontFS at ring 3]", 0x0D);
            command_processed = true;
        }
        else if (mystrcmp(cmd_buffer, "uptime") == true) {
            /* Convert raw PIT pulses into whole seconds (the clock ticks at TIMER_HZ) */
            uint32_t ticks = timer_ticks;
            char reply[160];
            char num[11];
            int pos = append_str(reply, 0, ">> Uptime: ");
            uint_to_dec(ticks / TIMER_HZ, num);
            pos = append_str(reply, pos, num);
            pos = append_str(reply, pos, " seconds (");
            uint_to_dec(ticks, num);
            pos = append_str(reply, pos, num);
            pos = append_str(reply, pos, " hardware ticks @ 100Hz)");
            shell_print_line(reply, 0x0B);
            command_processed = true;
        }
        else if (mystrcmp(cmd_buffer, "meminfo") == true) {
            uint32_t total = pmm_get_total_pages();
            uint32_t used  = pmm_get_used_pages();
            uint32_t free_kb = (total - used) * (PAGE_SIZE / 1024);
            char reply[160];
            char num[11];
            int pos = append_str(reply, 0, ">> Memory: ");
            uint_to_dec(used, num);
            pos = append_str(reply, pos, num);
            pos = append_str(reply, pos, "/");
            uint_to_dec(total, num);
            pos = append_str(reply, pos, num);
            pos = append_str(reply, pos, " pages used | ");
            uint_to_dec(free_kb, num);
            pos = append_str(reply, pos, num);
            pos = append_str(reply, pos, " KB free | Threads: ");
            uint_to_dec(get_thread_count(), num);
            pos = append_str(reply, pos, num);
            shell_print_line(reply, 0x0B);
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
        /* ---- FontFS shell commands -------------------------------------- */
        else if (mystrcmp(verb, "format") == true) {
            fontfs_format();
            shell_print_line(">> FontFS: formatted (fresh superblock + empty file table written)", 0x0A);
            command_processed = true;
        }
        else if (mystrcmp(verb, "ls") == true) {
            if (!fontfs_is_mounted()) {
                shell_print_line(">> FontFS: no filesystem. Run 'format' first.", 0x0C);
            } else {
                int n = fontfs_list(fs_ls_cb, nullptr);
                if (n == 0) shell_print_line(">> (no files)", 0x07);
            }
            command_processed = true;
        }
        else if (mystrcmp(verb, "cat") == true) {
            first_token(argtail, fs_name_buf, (int)sizeof(fs_name_buf));
            if (fs_name_buf[0] == '\0') {
                shell_print_line(">> usage: cat <file>", 0x0C);
            } else {
                int len = fontfs_read(fs_name_buf, fs_file_buf, FONTFS_MAX_FILE_SIZE);
                if (len == FONTFS_ERR_NOTFOUND) {
                    shell_print_line(">> cat: file not found", 0x0C);
                } else if (len == FONTFS_ERR_NOTMOUNTED) {
                    shell_print_line(">> FontFS: no filesystem. Run 'format' first.", 0x0C);
                } else if (len < 0) {
                    shell_print_line(">> cat: read error", 0x0C);
                } else {
                    fs_file_buf[len] = 0; // NUL-terminate for the printer
                    shell_print_line((const char*)fs_file_buf, 0x0F);
                }
            }
            command_processed = true;
        }
        else if (mystrcmp(verb, "write") == true) {
            /* write <file> <text...> : filename is the next token, the rest of
               the line (spaces preserved) becomes the file's contents. */
            const char* text = first_token(argtail, fs_name_buf, (int)sizeof(fs_name_buf));
            if (fs_name_buf[0] == '\0') {
                shell_print_line(">> usage: write <file> <text...>", 0x0C);
            } else {
                uint32_t tlen = 0;
                while (text[tlen] != '\0') tlen++;
                int rc = fontfs_write(fs_name_buf, (const uint8_t*)text, tlen);
                if (rc == FONTFS_OK) {
                    char reply[160];
                    char num[11];
                    int pos = append_str(reply, 0, ">> wrote ");
                    uint_to_dec(tlen, num);
                    pos = append_str(reply, pos, num);
                    pos = append_str(reply, pos, " bytes to '");
                    pos = append_str(reply, pos, fs_name_buf);
                    pos = append_str(reply, pos, "'");
                    shell_print_line(reply, 0x0A);
                } else if (rc == FONTFS_ERR_NOTMOUNTED) {
                    shell_print_line(">> FontFS: no filesystem. Run 'format' first.", 0x0C);
                } else if (rc == FONTFS_ERR_NOSPACE) {
                    shell_print_line(">> write: directory full (16 files max)", 0x0C);
                } else if (rc == FONTFS_ERR_TOOBIG) {
                    shell_print_line(">> write: text too large (8192 bytes max)", 0x0C);
                } else {
                    shell_print_line(">> write: error", 0x0C);
                }
            }
            command_processed = true;
        }
        else if (mystrcmp(verb, "touch") == true) {
            first_token(argtail, fs_name_buf, (int)sizeof(fs_name_buf));
            if (fs_name_buf[0] == '\0') {
                shell_print_line(">> usage: touch <file>", 0x0C);
            } else {
                int rc = fontfs_create(fs_name_buf);
                if (rc == FONTFS_OK) {
                    shell_print_line(">> created empty file", 0x0A);
                } else if (rc == FONTFS_ERR_EXISTS) {
                    shell_print_line(">> touch: file already exists", 0x0C);
                } else if (rc == FONTFS_ERR_NOTMOUNTED) {
                    shell_print_line(">> FontFS: no filesystem. Run 'format' first.", 0x0C);
                } else if (rc == FONTFS_ERR_NOSPACE) {
                    shell_print_line(">> touch: directory full (16 files max)", 0x0C);
                } else {
                    shell_print_line(">> touch: error", 0x0C);
                }
            }
            command_processed = true;
        }
        else if (mystrcmp(verb, "rm") == true) {
            first_token(argtail, fs_name_buf, (int)sizeof(fs_name_buf));
            if (fs_name_buf[0] == '\0') {
                shell_print_line(">> usage: rm <file>", 0x0C);
            } else {
                int rc = fontfs_delete(fs_name_buf);
                if (rc == FONTFS_OK) {
                    shell_print_line(">> removed", 0x0A);
                } else if (rc == FONTFS_ERR_NOTFOUND) {
                    shell_print_line(">> rm: file not found", 0x0C);
                } else if (rc == FONTFS_ERR_NOTMOUNTED) {
                    shell_print_line(">> FontFS: no filesystem. Run 'format' first.", 0x0C);
                } else {
                    shell_print_line(">> rm: error", 0x0C);
                }
            }
            command_processed = true;
        }
        /* ---- M4: run <file> — execute a FontFS flat binary at ring 3 ----- */
        else if (mystrcmp(verb, "run") == true) {
            /*
               The shell half of the program loader. IMPORTANT CONTEXT NOTE:
               this code runs inside the keyboard IRQ handler, so it only
               LOADS the program — reading a file with polling ATA from IRQ
               context is the same proven pattern 'cat' uses. The actual drop
               to ring 3 is deferred to the launcher task via
               user_program_submit(); entering user mode from inside an IRQ
               would break the EOI/iret chain (full rationale in
               src/syscall.cpp above user_prog_pending).
            */
            first_token(argtail, fs_name_buf, (int)sizeof(fs_name_buf));
            if (fs_name_buf[0] == '\0') {
                shell_print_line(">> usage: run <file>", 0x0C);
            } else if (!fontfs_is_mounted()) {
                shell_print_line(">> FontFS: no filesystem. Run 'format' first.", 0x0C);
            } else {
                int len = fontfs_read(fs_name_buf, fs_file_buf, FONTFS_MAX_FILE_SIZE);
                if (len == FONTFS_ERR_NOTFOUND) {
                    shell_print_line(">> run: file not found", 0x0C);
                } else if (len < 0) {
                    shell_print_line(">> run: read error", 0x0C);
                } else {
                    int rc = user_program_submit(fs_file_buf, (uint32_t)len);
                    if (rc == 0) {
                        char reply[160];
                        char num[11];
                        int pos = append_str(reply, 0, ">> run: '");
                        pos = append_str(reply, pos, fs_name_buf);
                        pos = append_str(reply, pos, "' (");
                        uint_to_dec((uint32_t)len, num);
                        pos = append_str(reply, pos, num);
                        pos = append_str(reply, pos, " bytes) loaded at 0x180000 -- entering ring 3");
                        shell_print_line(reply, 0x0A);
                    } else if (rc == -1) {
                        shell_print_line(">> run: a program is already running -- try again", 0x0C);
                    } else { /* rc == -2: empty (0 bytes) or > 8192-byte region */
                        shell_print_line(">> run: bad size (must be 1..8192 bytes)", 0x0C);
                    }
                }
            }
            command_processed = true;
        }
        /* Fixed: Explicitly verify character element index slot 0 to avoid pointer comparison leaks */
        else if (command_processed == false && cmd_buffer[0] != '\0') {
            shell_print_line(">> Command not found! Type 'help' for options.", 0x0D);
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
    outb(0x20, 0x20);
}
