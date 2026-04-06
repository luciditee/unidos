
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct mm {
    uintptr_t cr3_phys; // physical address of page directory
    uint32_t refcount;
    uintptr_t user_base; // base of user-space mapping (for sanity checks, not necessarily used for anything else)
    uintptr_t user_limit; // top of user-space mapping (must be < KERNEL_VIRTUAL_BASE)
    uint32_t page_count; 
    uint32_t slot_id; // for debugging, index in global mm array
} mm_t;

typedef enum mm_status {
    MM_SUCCESS = 0,
    MM_PD_PMM_INIT_FAILED = 1,
    MM_MM_PMM_INIT_FAILED = 2,
    MM_PD_PG_INIT_FAILED = 3,
    MM_MM_PG_INIT_FAILED = 4,
    MM_NO_MORE_REGION_SLOTS = 5
} mm_status_t;

mm_t* mm_create(void);
mm_t* mm_clone_user_eager(mm_t* parent);
void mm_destroy(mm_t* mm);
void mm_switch(mm_t* mm);
