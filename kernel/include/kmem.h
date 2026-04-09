
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "paging.h"
#include "kmain.h"
#include "panic.h"

//// KERNEL MEMORY MAP LAYOUT ////
// Virtual address space mimics Linux's higher-half schema on i386:
// 0x00000000 - 0xBFFFFFFF: user-space (3GB)
// 0xC0000000 - 0xFFFFFFFF: kernel-space (1GB)
// Within kernel-space, the following layout:
// Base address of the kernel image once remapped to higher-half
#define KERNEL_VIRTUAL_BASE 0xC0000000

// Base address of KVA, which holds various dynamically allocated
// pools of structures related to process/thread/stack management
#define KVA_REGION_BASE (KERNEL_VIRTUAL_BASE + 0x01000000)

// Base address of the physical memory window (to be deprecated)
#define PHYS_WINDOW_BASE    0xD0000000

// ── Demand-paged pools for mm_t and vma_t ─────────────────────────────
//
// Each pool has a bitmap that tracks which slots are allocated.  The
// bitmap is stored in its own pages *immediately below* the pool's
// virtual base so the two regions never overlap.  The number of bitmap
// pages is configurable per pool -- 1 page (4 KiB = 32 768 bits) is
// enough for ~32 k slots of the larger struct and ~130 k of the smaller.
//
// To increase bitmap capacity later, raise the _BITMAP_PAGES constant
// for the relevant pool.  The bitmap base will automatically shift
// downward to accommodate the extra pages.

// How many pages to devote to each pool's allocation bitmap.
#define VMA_BITMAP_PAGES    1
#define MMS_BITMAP_PAGES    1

// Base address for process VMAs (demand-paged pool)
#define VMA_VIRTUAL_BASE    0xF0000000
#define VMA_BITMAP_BASE     (VMA_VIRTUAL_BASE - (VMA_BITMAP_PAGES * 0x1000))

// Base address for process memory maps (demand-paged pool)
#define MMS_VIRTUAL_BASE    0xF4000000
#define MMS_BITMAP_BASE     (MMS_VIRTUAL_BASE - (MMS_BITMAP_PAGES * 0x1000))

// The lowest possible address on the kernel stack
// Ranges from STACK_VIRTUAL_BASE to STACK_TOP, growing downward
// from STACK_TOP
#define STACK_VIRTUAL_BASE  0xF7000000

// The size of the kernel stack.
#define STACK_TOP (STACK_VIRTUAL_BASE + (STACK_PAGE_SIZE << 12))

//// MEMORY MAP HELPERS ////
#define STACK_PAGE_SIZE     16

#define REGION_KERNEL_STACK "unistack"
#define REGION_PHYSWIN "physwin"
#define REGION_THREADPOOL "threadpool"
#define REGION_PROCESSPOOL "processpool"
#define REGION_KSTACK "kstacks"

_Static_assert((STACK_VIRTUAL_BASE & 0x003FFFFFu) == 0,
    "STACK_VIRTUAL_BASE must be 4MiB PDE-aligned");

#if (STACK_VIRTUAL_BASE < KERNEL_VIRTUAL_BASE) && ((STACK_VIRTUAL_BASE + (STACK_PAGE_SIZE << 12)) >= KERNEL_VIRTUAL_BASE)
#warning "Kernel stack mapping range reaches or exceeds KERNEL_VIRTUAL_BASE; adjust STACK_VIRTUAL_BASE or STACK_PAGE_SIZE"
#endif

static inline uintptr_t phys_to_virt(uint32_t phys_addr) {
    if (phys_addr + PHYS_WINDOW_BASE < phys_addr)
        panic("Virtual address overflow at physwin translation", NULL);
    return (uintptr_t)(PHYS_WINDOW_BASE + phys_addr);
}

static inline uintptr_t virt_to_phys(uint32_t virt_addr) {
    if ((virt_addr - PHYS_WINDOW_BASE) > PHYS_WINDOW_BASE)
        panic("Virtual address underflow at physwin translation", NULL);
    return (uintptr_t)(virt_addr - PHYS_WINDOW_BASE);
}

#define PHYS_TO_VIRT(pa) ((void*)phys_to_virt((uint32_t)(pa)))
#define VIRT_TO_PHYS(va) ((uint32_t)virt_to_phys((uint32_t)(va)))

static inline bool is_user_vaddr(uint32_t virt_addr) {
    return virt_addr < KERNEL_VIRTUAL_BASE;
}

static inline bool is_kernel_vaddr(uint32_t virt_addr) {
    return virt_addr >= KERNEL_VIRTUAL_BASE;
}

static inline cpl_t get_current_cpl() {
    uint16_t cpl;
    __asm__ __volatile__ ("mov %%cs, %0" : "=r" (cpl));
    cpl &= 3;
    return (cpl_t)cpl;
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

// Initialize all memory subsystems
void mem_init(void);

// PMM FUNCTIONS
void pmm_switch_to_phys_window_alias(void);
uint32_t pmm_get_next_available_block(pmm_alloc_result_t* out_status);
pmm_alloc_result_t pmm_alloc_specific_block(uint32_t page_aligned_phys_addr);
pmm_alloc_result_t pmm_dealloc_specific_block(uint32_t page_aligned_phys_addr);
bool pmm_query_bitmap(uint32_t address);

// KERNEL VIRTUAL ADDRESS MANAGER FUNCTIONS
void kva_register_region(const char* name, uint32_t size, uint32_t* out_base);
bool kva_map_region(const char* name);

// Returns the estimated number of available frames
uint64_t get_estimated_available_frames(void);