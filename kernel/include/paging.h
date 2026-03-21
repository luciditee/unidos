
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PG_PRESENT  (1u << 0)
#define PG_RW       (1u << 1)
#define PG_USER     (1u << 2)

typedef enum {
    PAGING_OK = 0,
    PAGING_ERR_ALIGN,
    PAGING_ERR_NOMEM,
    PAGING_ERR_NOT_MAPPED,
    PAGING_ERR_ALREADY_MAPPED,
    PAGING_ERR_PT_COUNT_MISMATCH,
    PAGING_ERR_INIT_RESERVE_FAILED
} paging_status_t;

typedef struct {
    bool mapped;
    uint32_t phys_addr;   // 4KiB-aligned
    uint32_t flags;       // decoded from PTE
} paging_query_result_t;

typedef struct {
    bool present;           // whether or not this entry is present or represents anything useful
                            // if false, the rest of the fields could be anything
    bool rw;                // if present, whether this page is mapped read/write or read-only
    bool user;              // if present, whether this page is user-accessible or kernel-only
    uint8_t kAttributes;    // 3 bits of kernel-defined attributes, ignored by 80386 but useful to us
    uint32_t pfAddress;     // 20-bit physical frame address (4KiB-aligned)
} page_table_decoded_t;

static inline page_table_decoded_t decode_pte(uint32_t pte) {
    page_table_decoded_t d = {0};
    d.present = (pte & PG_PRESENT) != 0;
    d.rw = (pte & PG_RW) != 0;
    d.user = (pte & PG_USER) != 0;
    d.kAttributes = (pte >> 9) & 0x7;
    d.pfAddress = (pte & 0xFFFFF000) >> 12;
    return d;
}

typedef uint32_t linear_addr_t;
typedef uint32_t ptentry_t;
typedef uint32_t pdentry_t;

static inline uint16_t pde_index(linear_addr_t la) {
    return (la >> 22) & 0x3FF;
}

static inline uint16_t pte_index(linear_addr_t la) {
    return (la >> 12) & 0x3FF;
}

static inline uint16_t page_offset(linear_addr_t la) {
    return la & 0xFFF;
}

// Creates PD/PTs, identity maps, loads CR3, sets CR0.PG
uint32_t paging_init_identity_window(uint32_t identity_bytes, paging_status_t* out_status);

paging_status_t paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
paging_status_t paging_unmap_page(uint32_t virt_addr);
paging_query_result_t paging_query_page(uint32_t virt_addr);

