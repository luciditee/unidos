
#include "kmain.h"
#include "kmem.h"
#include "bootinfo.h"


#define ADD_REGION(b, l, t) do { \
    if (n < MEM_REGIONS_MAX) { \
        mem_regions[n].base = (b); \
        mem_regions[n].length = (l); \
        mem_regions[n].type = (t); \
        n++; \
    } \
} while(0)
    

// Physical memory management bitmap and region tracking structure
static uint32_t* pagebitmap;
static uint32_t pagebitmap_size;

volatile mem_region_t mem_regions[MEM_REGIONS_MAX] = {0};
volatile size_t mem_region_count = 0;

#ifndef STAGE2_RESERVED_SECTORS
#warning "STAGE2_RESERVED_SECTORS not defined, defaulting to 6 (3KB) - this may cause boot issues if your stage2 is larger than 3KB, please adjust as needed and recompile stage2 and kernel"
#define STAGE2_RESERVED_SECTORS 6
#endif

void pmm_init(mem_region_t* regions, size_t region_count);

void mem_init(void) {
    if (!g_bootinfo_validated) {
        // Should never happen
        HALT_FOREVER;
        return;
    }

    size_t n = 0;

    // Kernel scratch memory reservation
    // Note: overlaps with VGA memory reservation, BIOS area,
    // bootinfo region, and possibly some small usable regions,
    // but this is simpler than trying to carve out usable regions
    ADD_REGION(0, 0xFFFFF, MEM_RESERVED);

    // Stage2 boot region
    // TODO: Should change 512 to build env macro
    ADD_REGION(0x10000, STAGE2_RESERVED_SECTORS * 512, MEM_RESERVED);

    // VGA memory reservation
    ADD_REGION(0xA0000, 0x1FFFF, MEM_RESERVED);

    // BIOS/ROM reserved area
    ADD_REGION(0xC0000, 0x3FFFF, MEM_RESERVED);

    // BOOTINFO reservation
    ADD_REGION(BOOTINFO_LINEAR_ADDR, BOOTINFO_SIZE_EXPECTED, MEM_RESERVED);

    // Kernel image reservation
    ADD_REGION(g_kernel_phys_address, g_kernel_size_bytes, MEM_RESERVED);    

    // Kernel params reservation
    ADD_REGION(g_kparams_phys_address, g_kparams_size_bytes, MEM_RESERVED);

    // Update usable memory region count
    mem_region_count = n;

        

    pmm_init((mem_region_t*)mem_regions, mem_region_count);
}

void pmm_init_single(mem_region_t* r) {
    if (r->type == MEM_RESERVED) {
        size_t start_page = r->base >> 12;
        size_t end_page = (r->base + r->length + 0xFFF) >> 12; // round up
        // We round up because we only care about preserving fully usable
        // page frames, which are 4K (0x1000) apiece
        for (size_t p = start_page; p < end_page; p++) {
            size_t idx = p / 32;
            size_t bit = p % 32;
            if (idx < 128)
                pagebitmap[idx] |= (1U << bit);
        }
    }
}

// Marks all pages in reserved regions as used in the page bitmap, leaving
// unreserved pages untouched
void pmm_init(mem_region_t* regions, size_t region_count) {
    for (size_t i = 0; i < region_count; i++) {
        mem_region_t* r = &regions[i];
        pmm_init_single(r);
    }
}

void pmm_alloc_frame(void) {
    // TODO
}

void pmm_free_frame(void) {
    // TODO
}

void pmm_reserve_range(uint32_t base, uint32_t length) {
    if (mem_region_count >= MEM_REGIONS_MAX) 
        return; // no more room to track regions
                // TODO: probably needs an error code of some sort
    size_t n = mem_region_count;
    ADD_REGION(base, length, MEM_RESERVED);
    pmm_init_single(&mem_regions[n-1]);
    mem_region_count = n;
}
