#include "heap.h"
#include "pmm.h" // Gives us access to PAGE_SIZE definitions

// A permanent pointer tracking the absolute start entry point of our heap chain
struct heap_chunk_header* heap_start = nullptr;

/*
   Initializes our heap tracking matrix by locking down a virtual address workspace
   and mapping a series of clean physical pages onto it.
*/
void init_heap(uint32_t start_vaddr, uint32_t initial_pages) {
    // 1. Establish the start boundary of our linked-list heap structure
    heap_start = (struct heap_chunk_header*)start_vaddr;

    // 2. Configure our very first master chunk metadata header block
    heap_start->size = (initial_pages * PAGE_SIZE) - sizeof(struct heap_chunk_header);
    heap_start->is_free = 1; // Mark as completely available for distribution
    heap_start->next = nullptr;
}

/*
   Custom Kernel Memory Allocator.
   Finds the first free metadata block that fits the requested byte size.
*/
void* kmalloc(size_t size) {
    struct heap_chunk_header* current = heap_start;

    // Loop through our block list until a free, appropriately sized block is encountered
    while (current != nullptr) {
        if (current->is_free && current->size >= size) {

            /*
               Block-Splitting Logic:
               If the found block has enough excess room to accommodate a brand new
               metadata tracking header AND at least 8 bytes of usable data payload,
               split the block into two on the fly.
            */
            if (current->size >= (size + sizeof(struct heap_chunk_header) + 8)) {
                // Calculate the pointer offset index where the new split chunk header will sit
                uint32_t split_address = (uint32_t)current + sizeof(struct heap_chunk_header) + size;
                struct heap_chunk_header* split_chunk = (struct heap_chunk_header*)split_address;

                // Configure the properties of the remaining split free segment
                split_chunk->size = current->size - size - sizeof(struct heap_chunk_header);
                split_chunk->is_free = 1;
                split_chunk->next = current->next;

                // Adjust the original block properties to match the newly requested size parameters
                current->size = size;
                current->next = split_chunk;
            }

            // Mark the selected block as active / reserved
            current->is_free = 0;

            // Reconstruct and return the usable data address pointer passing right past the metadata header
            uint32_t usable_memory_ptr = (uint32_t)current + sizeof(struct heap_chunk_header);
            return (void*)usable_memory_ptr;
        }
        current = current->next;
    }

    return nullptr; // Out of memory panic fallback handle
}

/*
   Frees an allocated heap element and merges surrounding clear frames.
*/
void kfree(void* ptr) {
    if (ptr == nullptr) return;

    // Shift our address pointer back by 12 bytes to access the hidden chunk metadata header
    uint32_t header_address = (uint32_t)ptr - sizeof(struct heap_chunk_header);
    struct heap_chunk_header* header = (struct heap_chunk_header*)header_address;

    // Free the block target segment frame
    header->is_free = 1;

    /*
       Memory Coalescing Loop:
       Walks the list from the beginning to find adjacent free blocks and merge them.
       This prevents memory from getting fragmented into tiny, unusable holes.
    */
    struct heap_chunk_header* current = heap_start;
    while (current != nullptr && current->next != nullptr) {
        if (current->is_free && current->next->is_free) {
            // Absorb the size of the next block along with its header layout overhead
            current->size = current->size + sizeof(struct heap_chunk_header) + current->next->size;
            current->next = current->next->next;
            // Do not advance loop tracking index; test the newly merged block boundary again
            continue;
        }
        current = current->next;
    }
}
