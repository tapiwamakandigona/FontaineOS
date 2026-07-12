#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "task.h"
#include "keyboard.h"

uint32_t count_alpha = 0;

/*
   A lightweight, bare-metal string comparison utility.
   Fixed: Enforces strict AND logic bounds so matching stops exactly at the null terminator!
*/
bool mystrcmp(const char* str1, const char* str2) {
    int i = 0;
    while (str1[i] != '\0' && str2[i] != '\0') {
        if (str1[i] != str2[i]) {
            return false;
        }
        i++;
    }
    // Double check that both strings terminated at the exact same index length position
    return (str1[i] == '\0' && str2[i] == '\0');
}

/*
   Thread Task Alpha.
   Runs concurrently on its own private stack and prints system cycle counts on row 3.
*/
void task_alpha_routine() {
    volatile char* video_memory = (volatile char*)0xB8000;
    while (true) {
        count_alpha = count_alpha + 1;
        char state_char = '0' + (count_alpha % 10);

        // Fixed: Offset shifted to 430 so the ticking digit sits at the end of the message text!
        video_memory[430] = state_char;
        video_memory[431] = 0x0A; // Light Green style ticker

        for (uint32_t delay = 0; delay < 10000000; delay++) { asm volatile(""); }
    }
}

/*
   Thread Task Beta (Our Live Kernel Command Shell Module!).
   Monitors our global keyboard buffer. If you type 'help' or 'clear',
   the kernel executes the corresponding custom script lines in real-time!
*/
void task_beta_routine() {
    volatile char* video_memory = (volatile char*)0xB8000;
    extern uint32_t cursor_position; // Import our live keyboard screen index pointer

    while (true) {
        char* command = get_shell_command();

        if (command != nullptr) {
            /*
               Step 1: Instantly lock out interrupts and cache the string locally
               to isolate our data from asynchronous hardware modifications!
            */
            asm volatile("cli");

            char local_cmd[64];
            int c_idx = 0;
            while (command[c_idx] != '\0' && c_idx < 63) {
                local_cmd[c_idx] = command[c_idx];
                c_idx++;
            }
            local_cmd[c_idx] = '\0';

            // Step 2: Instantly free the keyboard driver buffer layout so it can take new keys
            clear_shell_command();
            asm volatile("sti");

            /* Step 3: Run our string evaluations safely on our own local stack copy */
            if (mystrcmp(local_cmd, "help") == true) {
                const char* reply = ">> [FontaineOS Terminal Help: Commands are 'help' and 'clear']";
                int i = 0;
                while (reply[i] != '\0') {
                    video_memory[1760 + (i * 2)] = reply[i]; // Print on Row 11
                    video_memory[1760 + (i * 2) + 1] = 0x0D; // Purple style
                    i++;
                }
            }
            else if (mystrcmp(local_cmd, "clear") == true) {
                for (int i = 1600; i < 4000; i = i + 2) {
                    video_memory[i] = ' ';
                    video_memory[i + 1] = 0x07; // Default style reset
                }
                cursor_position = 1600;
            }
        }

        for (uint32_t delay = 0; delay < 2000000; delay++) { asm volatile(""); }
    }
}

extern "C" void kernel_main() {
    /* Step 1: Initialize the Core System Engine Segments */
    init_gdt();
    init_idt();
    init_timer(100);
    init_pmm(64 * 1024 * 1024);
    init_vmm();
    init_heap(0x300000, 256);

    /* Step 2: Initialize the Multitasking Scheduler Layer */
    init_multitasking();

    /* Step 3: Spawn our parallel runtime threads */
    create_thread(task_alpha_routine);
    create_thread(task_beta_routine);

    /* Render Baseline Screen Texts ONCE before loops execute to eliminate memory drift */
    volatile char* video_memory = (volatile char*)0xB8000;

    const char* msg_master = "FontaineOS Architecture Complete! Task Scheduler Loops Active.";
    const char* msg_alpha  = "[Task Alpha Running Concurrently] Cycle State Ticking: ";
    const char* msg_shell  = "FontaineOS Console Interface Live. Type 'help' or 'clear':";

    int i = 0;
    while (msg_master[i] != '\0') {
        video_memory[0 + (i * 2)] = msg_master[i];
        video_memory[0 + (i * 2) + 1] = 0x0E; // Gold text style on Row 1
        i++;
    }

    i = 0;
    while (msg_alpha[i] != '\0') {
        video_memory[320 + (i * 2)] = msg_alpha[i];
        video_memory[320 + (i * 2) + 1] = 0x0A; // Light Green style on Row 3
        i++;
    }

    i = 0;
    while (msg_shell[i] != '\0') {
        // Fixed: Shifted down to offset 1440 (Row 9) so it hovers perfectly above our typing line!
        video_memory[1440 + (i * 2)] = msg_shell[i];
        video_memory[1440 + (i * 2) + 1] = 0x0B; // Light Cyan style on Row 9
        i++;
    }

    /* Step 4: Enable Hardware Interrupts globally */
    asm volatile("sti");

    while (true) {
        asm volatile("hlt");
    }
}
