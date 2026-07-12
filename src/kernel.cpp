#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "task.h"
#include "keyboard.h"

uint32_t count_alpha = 0;
uint32_t count_beta = 0;

/*
   A lightweight, bare-metal string comparison utility.
   Enforces strict AND logic bounds so matching stops exactly at the null terminator!
*/
bool mystrcmp(const char* str1, const char* str2) {
    int i = 0;
    while (str1[i] != '\0' && str2[i] != '\0') {
        if (str1[i] != str2[i]) {
            return false;
        }
        i++;
    }
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

        video_memory[430] = state_char;
        video_memory[431] = 0x0A; // Light Green style ticker

        for (uint32_t delay = 0; delay < 1000000; delay++) { asm volatile(""); }

        // Cooperatively hand the CPU over to the next task in the ring
        switch_task();
    }
}

/*
   Thread Task Beta.
   Runs concurrently alongside Task Alpha and updates its own separate status ticker on row 4!
*/
void task_beta_routine() {
    volatile char* video_memory = (volatile char*)0xB8000;
    while (true) {
        count_beta = count_beta + 1;
        char state_char = '0' + (count_beta % 10);

        video_memory[590] = state_char;
        video_memory[591] = 0x0D; // Light Purple style ticker

        for (uint32_t delay = 0; delay < 1000000; delay++) { asm volatile(""); }

        // Cooperatively hand the CPU over to the next task in the ring
        switch_task();
    }
}

/*
   Wipe every character cell so leftover firmware text (SeaBIOS banners,
   'Booting from ROM...' residue) cannot linger behind our own console output.
*/
static void clear_screen() {
    volatile char* video_memory = (volatile char*)0xB8000;
    for (int i = 0; i < 4000; i = i + 2) {
        video_memory[i] = ' ';
        video_memory[i + 1] = 0x07; // Standard gray-on-black attribute
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

    /* Initialize our hardware keyboard subsystem */
    init_keyboard();

    /* Step 2: Initialize the Multitasking Scheduler Layer */
    init_multitasking();

    /* Step 3: Spawn our parallel runtime threads */
    create_thread(task_alpha_routine);
    create_thread(task_beta_routine);

    /* Start from a pristine screen so firmware boot text cannot bleed into our console */
    clear_screen();

    /* Render Baseline Screen Texts ONCE before loops execute to eliminate memory drift */
    volatile char* video_memory = (volatile char*)0xB8000;

    const char* msg_master = "FontaineOS Architecture Complete! Task Scheduler Loops Active.";
    const char* msg_alpha  = "[Task Alpha Running Concurrently] Cycle State Ticking: ";
    const char* msg_beta   = "[Task Beta Running Concurrently] Cycle State Ticking:  ";
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
    while (msg_beta[i] != '\0') {
        video_memory[480 + (i * 2)] = msg_beta[i];
        video_memory[480 + (i * 2) + 1] = 0x0D; // Light Purple style on Row 4
        i++;
    }

    i = 0;
    while (msg_shell[i] != '\0') {
        video_memory[1440 + (i * 2)] = msg_shell[i];
        video_memory[1440 + (i * 2) + 1] = 0x0B; // Light Cyan style on Row 9
        i++;
    }

    /* Step 4: Enable Hardware Interrupts globally */
    asm volatile("sti");

    /*
       Step 5: Drive the cooperative round-robin scheduler.
       Every pass hands the CPU to the next task in the ring (Alpha, then Beta),
       and once the ring rotates back here we sleep until the next hardware
       interrupt instead of burning cycles. Without this call chain the spawned
       threads would sit parked on their private stacks forever.
    */
    while (true) {
        switch_task();
        asm volatile("hlt");
    }
}
