
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "paging.h"

#define MEM_REGIONS_MAX 256
#define KERNEL_VIRTUAL_BASE 0xC0000000
#define STACK_VIRTUAL_BASE 0xB0000000
#define PHYS_WINDOW_BASE 0xD0000000

static inline uintptr_t phys_to_virt(uint32_t phys_addr) {
    return (uintptr_t)(PHYS_WINDOW_BASE + phys_addr);
}

static inline uintptr_t virt_to_phys(uint32_t virt_addr) {
    return (uintptr_t)(virt_addr - PHYS_WINDOW_BASE);
}

#define PHYS_TO_VIRT(pa) ((void*)phys_to_virt((uint32_t)(pa)))
#define VIRT_TO_PHYS(va) ((uint32_t)virt_to_phys((uint32_t)(va)))

inline bool is_user_vaddr(uint32_t virt_addr) {
    return virt_addr < KERNEL_VIRTUAL_BASE;
}

inline bool is_kernel_vaddr(uint32_t virt_addr) {
    return virt_addr >= KERNEL_VIRTUAL_BASE;
}

typedef enum {
    MEM_USABLE = 1,
    MEM_RESERVED = 2
} mem_type_t;

typedef enum {
    PMM_ALLOC_SUCCESS = 0,
    PMM_ALLOC_OOM = 1,
    PMM_NO_PAGES_INITIALIZED = 2,
    PMM_BITMAP_NOT_INITIALIZED = 3,
    PMM_BITMAP_INCONSISTENT_RESERVED = 4,
    PMM_ALLOC_ALREADY_ALLOCATED = 5,
    PMM_ALLOC_RESERVED = 6,
    PMM_ALLOC_OUTOFBOUNDS = 7,
    PMM_ALLOC_UNALIGNED_ADDR = 8,
    PMM_ALLOC_ALREADY_FREE = 9
} pmm_alloc_result_t;

typedef enum {
    PMM_RESERVE_SUCCESS = 0,
    PMM_NO_MORE_REGION_SLOTS = 1,
    PMM_INVALID_REGION_LENGTH = 2,
    PMM_UPDATED_EXISTING_REGION = 3
} pmm_reserve_result_t;

typedef struct {
    uint32_t base;
    uint32_t length;
    mem_type_t type;
} mem_region_t;

void mem_init(void);
void pmm_switch_to_phys_window_alias(void);
uint32_t pmm_get_next_available_block(pmm_alloc_result_t* out_status);
pmm_alloc_result_t pmm_alloc_specific_block(uint32_t page_aligned_phys_addr);
pmm_alloc_result_t pmm_dealloc_specific_block(uint32_t page_aligned_phys_addr);
bool pmm_query_bitmap(uint32_t address);


_Static_assert(MEM_REGIONS_MAX >= 16, "MEM_REGIONS_MAX too small");