
#include "kmain.h"
#include "kmem.h"
#include "isr.h"
#include "bootinfo.h"
#include "paging.h"

#define PT_MAX 1025 // 1024 for 1:1 mapping of 4GiB, plus 1 for PD
#define PF_VECTOR 0x0E

static volatile uint32_t page_directory_loc_phys = 0;
static volatile uint32_t page_tables_loc_phys[PT_MAX] = {0};
static volatile uint32_t page_table_count = 0;

extern int pmm_reserve_pageframe_seq(uint32_t requested, uint32_t* out_reserved, uint32_t* out_count, bool clear);

static void on_pagefault(trap_frame_t* tf);

uint32_t paging_init_identity_window(uint32_t identity_bytes, paging_status_t* out_status) {
    if (identity_bytes % 4096 != 0) {
        if (out_status) 
            *out_status = PAGING_ERR_ALIGN;
        return 0;
    }

    // If no specific id mapping size requested, we identity map
    // all available memory.
    // NOTE: If PAE ever supported in the future, this can overflow
    // and identity_bytes ought to be promoted to uint64_t (TODO:
    // type alias to make this easier)
    if (identity_bytes == 0) {
        identity_bytes = g_avail_memory_kib << 10; // 2^10 = * 1024
        identity_bytes = (identity_bytes + 0x3FFFFF) & ~0x3FFFFF; // Round up to nearest 4MiB
    }
    
    // We're going to allocate a PD and PT for this identity mapping, and
    // ideally the pages will be contiguous--but they aren't guaranteed to be
    // For that reason, we will request the needed amount of frames for PT,
    // and add one for the PD, then fill those pages in. These frames will be
    // reserved for paging manager use and will be permanently memory-resident
    // divide by 4MiB, round up
    uint32_t needed_frames = ((identity_bytes >> 22) + ((identity_bytes & 0x3FFFFF) ? 1 : 0)) + 1; 

    if (needed_frames > PT_MAX) {
        if (out_status)
            *out_status = PAGING_ERR_PT_COUNT_MISMATCH;
        return 0;
    }
    uint32_t reserved_frames[PT_MAX] = {0};
    uint32_t remaining = needed_frames;
    kdbg_puts("reserving ", 0x0A);
    kdbg_hex32(needed_frames, 0x0A);
    kdbg_puts(" page frames for paging structures", 0x0A);
    while (remaining > 0) {
        uint32_t reserved_this_round = 0;
        int res = pmm_reserve_pageframe_seq(remaining, reserved_frames + (needed_frames - remaining), &reserved_this_round, true);
        if (res != 0 || reserved_this_round == 0) {
            if (out_status)
                *out_status = PAGING_ERR_INIT_RESERVE_FAILED;
            return 0;
        }
        kdbg_puts(".", 0x0A);
        remaining -= reserved_this_round;
    }
    kdbg_puts("done\r\n", 0x0A);

    // reserved_frames now contains the physical addresses of the frames we needed,
    // and we need to get their addresses off this function's stack
    page_directory_loc_phys = reserved_frames[0];
    for (uint32_t i = 1; i < needed_frames; i++) {
        page_tables_loc_phys[i-1] = reserved_frames[i];
    }
    page_table_count = needed_frames - 1;

    // init page directory for identity mapping
    uint32_t* pd = (uint32_t*)page_directory_loc_phys;
    for (uint32_t i = 0; i < page_table_count; i++) {
        pd[i] = page_tables_loc_phys[i] | PG_PRESENT | PG_RW; // map PD entry to PT
    }

    // init page tables for identity mapping
    uint32_t pages_mapped = (identity_bytes + 0xFFF) >> 12; // round to nearest page
    for (uint32_t i = 0; i < pages_mapped; i++) {
        uint32_t pdi = (i >> 10) & 0x3FF;   // PT's index in PD

        if (pdi >= page_table_count) {
            if (out_status) *out_status = PAGING_ERR_PT_COUNT_MISMATCH;
            return 0;
        }

        uint32_t pti = i & 0x3FF;           // Page entry within that PT
        uint32_t* pt = (uint32_t*)page_tables_loc_phys[pdi];
        pt[pti] = ((i << 12) & 0xFFFFF000) | PG_PRESENT | PG_RW; 
    }

    // Register page fault handler
    isr_register(PF_VECTOR, on_pagefault);

    // Load PD address into CR3, set CR0.PG to enable paging
    __asm__ __volatile__ (
        "movl %0, %%cr3\n"
        "movl %%cr0, %%eax\n"
        "orl $0x80000000, %%eax\n"
        "movl %%eax, %%cr0\n"
        :
        : "r" (page_directory_loc_phys)
        : "eax"
    );

    if (out_status)
        *out_status = PAGING_OK;

    return page_directory_loc_phys;
}

static inline uint32_t read_cr2(void) {
    uint32_t v;
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(v));
    return v;
}

static void on_pagefault(trap_frame_t* tf) {
    uint32_t err = tf->error; 
    uint32_t cr2 = read_cr2();

    // 386-relevant bits
    uint32_t present = (err & 0x1);        // 0: not-present, 1: protection violation
    uint32_t write   = (err >> 1) & 0x1;   // 0: read, 1: write
    uint32_t user    = (err >> 2) & 0x1;   // 0: supervisor, 1: user

    kdbg_puts("#PF: cr2=", 0x0C); kdbg_hex32(cr2, 0x0C);
    kdbg_puts(" err=", 0x0C);      kdbg_hex32(err, 0x0C);
    kdbg_puts(" P=", 0x0C);        kdbg_hex32(present, 0x0C);
    kdbg_puts(" W=", 0x0C);        kdbg_hex32(write, 0x0C);
    kdbg_puts(" U=", 0x0C);        kdbg_hex32(user, 0x0C);
    kdbg_puts("\r\n", 0x0C);

    kdbg_dump_current();
    HALT_FOREVER;
}
