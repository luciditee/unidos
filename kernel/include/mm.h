
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum vma_type {
    VMA_TEXT =      1,
    VMA_RODATA =    2,
    VMA_DATA =      3,
    VMA_BSS =       4,
    VMA_STACK =     5,
    VMA_HEAP =      6,
    VMA_ANON =      7
} vma_type_t;

#define VMA_PROT_READ   0x01
#define VMA_PROT_WRITE  0x02
#define VMA_PROT_EXEC   0x04

typedef struct vma {
    uintptr_t start;
    uintptr_t end;
    vma_type_t type;
    uint32_t prot;
    struct vma* next;
    uint32_t slot_id; // used later to index into bitmap for freeing
} vma_t;

typedef struct mm {
    uintptr_t cr3_phys; // physical address of page directory
    uint32_t refcount;
    uintptr_t user_base; // base of user-space mapping (for sanity checks, not necessarily used for anything else)
    uintptr_t user_limit; // top of user-space mapping (must be < KERNEL_VIRTUAL_BASE)
    uint32_t page_count; 
    uint32_t slot_id; // used later to index into bitmap for freeing
    vma_t* vma_head;    // VMA linked list (note: sorted)
    uint32_t brk_start; // heap start region set by exec
    uint32_t brk_current; // current break position
} mm_t;

// NOTE: These are intended to overlay dpage.h's status codes and are
// naively cast from there for convenience, but kept distinct for debugging
// and readability. Suffice to say, if the order of these values changes
// in dpage.h, they must also change here.
typedef enum mm_status {
    MM_SUCCESS = 0,
    MM_PD_PMM_INIT_FAILED = 1,
    MM_MM_PMM_INIT_FAILED = 2,
    MM_NO_MORE_REGION_SLOTS = 3,
    MM_PD_PG_INIT_FAILED = 4,
    MM_MM_PG_INIT_FAILED = 5,
} mm_status_t;

mm_t* mm_create(void);
mm_t* mm_clone_user_eager(mm_t* parent);
void mm_destroy(mm_t* mm);
void mm_switch(mm_t* mm);
int mm_map_region(mm_t* mm, uint32_t va_start,
    size_t size, vma_type_t type, uint32_t prot,
    const void* initial_data, size_t initdata_size);

vma_t* vma_alloc(void);
void vma_free(vma_t* vma);
void vma_insert(mm_t* mm, vma_t* new_vma);
void vma_remove(mm_t* mm, vma_t* v);
void vma_destroy_all(mm_t* mm);
vma_t* vma_find(mm_t* mm, uintptr_t vaddr);