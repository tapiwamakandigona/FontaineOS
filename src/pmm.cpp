#include "pmm.h"

/*
   Bitmap storage calculation:
   128MB / 4096 bytes per page = 32,768 total pages.
   Since each byte holds 8 bits, we need 32,768 / 8 = 4,096 tracking bytes.
*/
#define BITMAP_SIZE 4096
uint8_t pmm_bitmap[BITMAP_SIZE];

// Track the highest page count index available to our allocator loops
uint32_t total_pages = 0;

/*
   Inline helper utility to flip a single bit to '1' (Allocated/Reserved state)
*/
inline void bitmap_set(uint32_t page_index) {
    uint32_t byte_index = page_index / 8;
    uint32_t bit_offset = page_index % 8;
    pmm_bitmap[byte_index] |= (1 << bit_offset);
}

/*
   Inline helper utility to flip a single bit to '0' (Free/Available state)
*/
inline void bitmap_clear(uint32_t page_index) {
    uint32_t byte_index = page_index / 8;
    uint32_t bit_offset = page_index % 8;
    pmm_bitmap[byte_index] &= ~(1 << bit_offset);
}

/*
   Inline helper utility to inspect the status of a specific bit
*/
inline bool bitmap_test(uint32_t page_index) {
    uint32_t byte_index = page_index / 8;
    uint32_t bit_offset = page_index % 8;
    return (pmm_bitmap[byte_index] & (1 << bit_offset)) != 0;
}

/*
   The primary initialization function called by our core kernel boot loop.
*/
void init_pmm(uint32_t memory_size_bytes) {
    total_pages = memory_size_bytes / PAGE_SIZE;

    // 1. Initially mark all memory blocks as completely free (clear all bits to 0)
    for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
        pmm_bitmap[i] = 0;
    }

    /*
       2. Protect the Kernel Area.
       Our kernel binary boots into memory starting at 1MB (0x100000).
       We must mark the first 2 Megabytes of RAM as permanently reserved
       (bits set to 1) so our allocator doesn't accidentally overwrite our code!
    */
    uint32_t reserved_kernel_pages = (2 * 1024 * 1024) / PAGE_SIZE; // 512 pages
    for (uint32_t i = 0; i < reserved_kernel_pages; i++) {
        bitmap_set(i);
    }
}

/*
   Allocates a single 4KB page of physical memory.
   Scans the bitmap for the first free bit (0), sets it to 1,
   and calculates its raw physical RAM address block pointer.
*/
void* pmm_alloc_page() {
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            // Reconstruct the 32-bit physical RAM address from the page index map
            uint32_t physical_address = i * PAGE_SIZE;
            return (void*)physical_address;
        }
    }
    return nullptr; // Out of memory panic fallback handle
}

/*
   Frees an allocated page, returning it back to the available memory pool.
*/
void pmm_free_page(void* paddr) {
    uint32_t physical_address = (uint32_t)paddr;
    uint32_t page_index = physical_address / PAGE_SIZE;
    bitmap_clear(page_index);
}

/*
   Reports the total number of physical page frames tracked by the allocator.
*/
uint32_t pmm_get_total_pages() {
    return total_pages;
}

/*
   Walks the allocation bitmap and counts every page currently marked as reserved.
*/
uint32_t pmm_get_used_pages() {
    uint32_t used = 0;
    for (uint32_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) {
            used = used + 1;
        }
    }
    return used;
}
