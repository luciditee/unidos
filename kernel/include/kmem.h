
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MEM_REGIONS_MAX 32

typedef enum {
    MEM_USABLE = 1,
    MEM_RESERVED = 2
} mem_type_t;

typedef struct {
    uint32_t base;
    uint32_t length;
    mem_type_t type;
} mem_region_t;

void mem_init(void);

_Static_assert(MEM_REGIONS_MAX >= 16, "MEM_REGIONS_MAX too small");