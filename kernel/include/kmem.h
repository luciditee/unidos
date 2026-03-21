
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MEM_REGIONS_MAX 256

typedef enum {
    MEM_USABLE = 1,
    MEM_RESERVED = 2
} mem_type_t;

typedef enum {
    PMM_PAGE_FREED = 0,
    PMM_PAGE_RESERVED = 1,
    PMM_PAGE_ALREADYFREE = 2,
    PMM_PAGE_OUTOFBOUNDS = 3,
    PMM_PAGE_ADDRESS_UNALIGNED = 4
} pmm_free_result_t;

typedef enum {
    PMM_ALLOC_SUCCESS = 0,
    PMM_ALLOC_OOM = 1,
    PMM_NO_PAGES_INITIALIZED = 2
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

_Static_assert(MEM_REGIONS_MAX >= 16, "MEM_REGIONS_MAX too small");