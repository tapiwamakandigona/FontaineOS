#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

/*
   Each block of memory on our heap is preceded by this hidden metadata header
   tracking structure to map its active allocation traits.
*/
struct heap_chunk_header {
    uint32_t size;                     // Size of the usable data zone block in bytes
    uint8_t  is_free;                  // Flag: 1 if available, 0 if active/allocated
    struct   heap_chunk_header* next;  // Pointer linking to the next chunk in the heap chain
} __attribute__((packed));

/*
   Exposing our primary engine allocation functions to the rest of the kernel system layers.
*/
void  init_heap(uint32_t start_vaddr, uint32_t initial_pages);
void* kmalloc(size_t size);
void  kfree(void* ptr);

#endif
