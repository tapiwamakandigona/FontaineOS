#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"

extern "C" void kernel_main() {
    /* Step 1: Initialize the Global Descriptor Table (GDT) */
    init_gdt();

    /* Step 2: Initialize the Interrupt Descriptor Table (IDT) */
    init_idt();

    /* Step 3: Initialize the Programmable Interval Timer (PIT) at 100Hz */
    init_timer(100);

    /* Step 4: Initialize the Physical Memory Manager (PMM) with 64MB space bounds */
    init_pmm(64 * 1024 * 1024);

    /* Step 5: Initialize the Virtual Memory Manager (VMM) and enable hardware paging mode */
    init_vmm();

    /* Step 6: Initialize the Kernel Heap Manager at 3MB with 1MB workspace boundaries */
    init_heap(0x300000, 256);

    /* Step 7: Run a Live Heap Allocation Block Split Check */
    void* chunk_a = kmalloc(32);  // Request 32 bytes
    void* chunk_b = kmalloc(128); // Request 128 bytes right behind it

    /* Step 8: Enable Hardware Interrupts globally */
    asm volatile("sti");

    volatile char* video_memory = (volatile char*)0xB8000;
    const char* message = "FontaineOS Kernel Heap Live! kmalloc() Allocation Split Success.";

    /*
       Dynamic Spacing Verification:
       Instead of guessing hex numbers, we mathematically ensure that Chunk B
       lives exactly past Chunk A's address, plus Chunk A's data size, plus the compiler's
       exact structure size layout padding overhead.
    */
    uint32_t expected_chunk_b = (uint32_t)chunk_a + 32 + sizeof(struct heap_chunk_header);

    if (chunk_a != nullptr && (uint32_t)chunk_b == expected_chunk_b) {
        int i = 0;
        while (message[i] != '\0') {
            video_memory[i * 2] = message[i];

            /*
               0x0E represents brilliant Light Yellow / Gold color on a Black background.
               We switch to yellow text to show the custom heap engine passed alignment tests!
            */
            video_memory[i * 2 + 1] = 0x0E;
            i++;
        }
    } else {
        const char* fail_msg = "Heap Allocation Math Verification Mismatch: Heap Chain Broken.";
        int i = 0;
        while (fail_msg[i] != '\0') {
            video_memory[i * 2] = fail_msg[i];
            video_memory[i * 2 + 1] = 0x04; // Urgent plain red text style
            i++;
        }
    }

    while (true) {
        // CPU spins safely here, driving execution tracking loops smoothly
    }
}
