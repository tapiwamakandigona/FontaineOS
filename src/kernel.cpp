#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "pmm.h"

extern "C" void kernel_main() {
    /* Step 1: Initialize the Global Descriptor Table (GDT) */
    init_gdt();

    /* Step 2: Initialize the Interrupt Descriptor Table (IDT) */
    init_idt();

    /* Step 3: Initialize the Programmable Interval Timer (PIT) at 100Hz */
    init_timer(100);

    /*
       Step 4: Initialize the Physical Memory Manager (PMM).
       We pass 64 Megabytes (64 * 1024 * 1024 bytes) to map our
       lightweight initial tracking bitmap matrix workspace boundaries.
    */
    init_pmm(64 * 1024 * 1024);

    /*
       Step 5: Run a Live Memory Allocation Verification Check.
       We request one blank 4KB physical page. The engine should look past our
       reserved kernel space boundaries and hand out the first unreserved 4KB pointer.
    */
    void* allocated_page_ptr = pmm_alloc_page();

    /* Step 6: Enable Hardware Interrupts globally */
    asm volatile("sti");

    volatile char* video_memory = (volatile char*)0xB8000;
    const char* message = "FontaineOS Matrix Active! PMM Allocation Verification Success.";

    /*
       Verify our pointer. Since the first 2MB are reserved for the kernel,
       the first page dynamically handed to us should sit right at 2MB (0x200000).
       If our pointer is valid, print our confirmation message in Light Purple style!
    */
    if (allocated_page_ptr == (void*)0x200000) {
        int i = 0;
        while (message[i] != '\0') {
            video_memory[i * 2] = message[i];

            /*
               0x0D represents a brilliant Light Purple / Magenta color on a Black background.
               We switch to purple to show that the memory allocation test passed cleanly!
            */
            video_memory[i * 2 + 1] = 0x0D;
            i++;
        }
    } else {
        // Fallback trace message if pointer location math fails validation
        const char* fail_msg = "PMM Verification Failed: Out of Memory Pointer Offset Match.";
        int i = 0;
        while (fail_msg[i] != '\0') {
            video_memory[i * 2] = fail_msg[i];
            video_memory[i * 2 + 1] = 0x04; // Plain red font trace
            i++;
        }
    }

    while (true) {
        // CPU spins safely here, handling system timers and keyboard inputs in real-time
    }
}
