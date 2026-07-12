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

    while (true) {
        // Query if our keyboard engine has caught a fresh instruction string command
        char* command = get_shell_command();

        if (command != nullptr) {
            /*
               Explicit character array parsing checking individual indices.
            */
            if (command[0] == 'h' && command[1] == 'e' && command[2] == 'l' && command[3] == 'p') {
                const char* reply = ">> [FontaineOS Terminal Help: Commands are 'help' and 'clear']";
                int i = 0;
                while (reply[i] != '\0') {
                    // Fixed: Offset shifted to 1760 (Row 11) to print below your inputs
                    video_memory[1760 + (i * 2)] = reply[i];
                    video_memory[1760 + (i * 2) + 1] = 0x0D; // Purple output style
                    i++;
                }
            }
            // Check command target index: Custom 'clear' command match routing
            else if (command[0] == 'c' && command[1] == 'l' && command[2] == 'e' && command[3] == 'a' && command[4] == 'r') {
                // Fixed: Loop now starts at 1600 to clear inputs without erasing the cyan label at 1440
                for (int i = 1600; i < 4000; i = i + 2) {
                    video_memory[i] = ' ';
                    video_memory[i + 1] = 0x07;
                }
            }

            // Wipe our buffer matrices clean so we can take the next prompt line instruction
            clear_shell_command();
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
