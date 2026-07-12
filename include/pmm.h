#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/*
   x86 hardware memory specifications.
   Each standard physical memory page block allocation frame spans exactly 4096 bytes (4KB).
*/
#define PAGE_SIZE 4096

/*
   Exposing our primary engine functions to the rest of the kernel system layers.
*/
void init_pmm(uint32_t memory_size_bytes);
void* pmm_alloc_page();
void pmm_free_page(void* paddr);

#endif
