
#include "kmain.h"
#include "kmem.h"
#include "bootinfo.h"
    

// Physical memory management bitmap and region tracking structure
static uint8_t* pagebitmap;
static uint32_t pagebitmap_size_bytes = 0;
static uint32_t frame_count = 0;
static uint32_t frame_hint = 0;

volatile mem_region_t mem_regions[MEM_REGIONS_MAX] = {0};
volatile size_t mem_region_count = 0;

static inline void bm_set(uint32_t f)   { pagebitmap[f >> 3] |=  (1u << (f & 7)); }
static inline void bm_clear(uint32_t f) { pagebitmap[f >> 3] &= ~(1u << (f & 7)); }

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

    #define ADD_REGION(b, l, t) do { \
        if (n < MEM_REGIONS_MAX) { \
            mem_regions[n].base = (b); \
            mem_regions[n].length = (l); \
            mem_regions[n].type = (t); \
            n++; \
        } \
    } while(0)

    size_t n = 0;

    // Kernel scratch memory reservation
    // Note: overlaps with VGA memory reservation, BIOS area,
    // bootinfo region, and possibly some small usable regions,
    // but this is simpler than trying to carve out usable regions
    // REQUIRED if we want to keep programs from allocating memory
    // below 1MB
    ADD_REGION(0, 0x10000, MEM_RESERVED);

    // Stage2 boot region
    // TODO: Should change 512 to build env macro
    ADD_REGION(0x10000, STAGE2_RESERVED_SECTORS * 512, MEM_RESERVED);

    // VGA memory reservation
    ADD_REGION(0xA0000, 0x20000, MEM_RESERVED);

    // BIOS/ROM reserved area
    ADD_REGION(0xC0000, 0x40000, MEM_RESERVED);

    // BOOTINFO reservation
    ADD_REGION(BOOTINFO_LINEAR_ADDR, BOOTINFO_SIZE_EXPECTED, MEM_RESERVED);

    // Kernel image reservation
    ADD_REGION(g_kernel_phys_address, g_kernel_size_bytes, MEM_RESERVED);    

    // Kernel params reservation
    ADD_REGION(g_kparams_phys_address, g_kparams_size_bytes, MEM_RESERVED);

    // Parse E820 memory map and mark unusable regions as reserved.
    if (g_memory_method == USE_E820) {
        for (size_t i = 0; i < g_e820_desc_count; i++) {
            volatile const e820_desc_t* d = &g_e820_descs[i];
            if (d->type != 1)
                ADD_REGION((uint32_t)d->base, (uint32_t)d->length, MEM_RESERVED);
        }
    }

    // Update usable memory region count
    mem_region_count = n;

    #undef ADD_REGION

    // init physical memory manager
    pmm_init((mem_region_t*)mem_regions, mem_region_count);
}

static void bm_set_range_touched(uint32_t base, uint32_t len) {
    if (len == 0) return;

    uint64_t start = ((uint64_t)base) >> 12;
    uint64_t end   = (((uint64_t)base + (uint64_t)len + 0xFFFULL) >> 12); // ceil, exclusive

    if (end > (uint64_t)frame_count) end = frame_count;
    for (uint32_t f = (uint32_t)start; f < (uint32_t)end; ++f) bm_set(f);
}

static void bm_clear_range_full_pages(uint32_t base, uint32_t len) {
    if (len < 4096) return;

    uint64_t start = (((uint64_t)base + 0xFFFULL) & ~0xFFFULL); // align up
    uint64_t end   = (((uint64_t)base + (uint64_t)len) & ~0xFFFULL); // align down, exclusive
    if (end <= start) return;

    uint64_t fs64 = start >> 12;
    uint64_t fe64 = end   >> 12;
    if (fe64 > (uint64_t)frame_count) fe64 = frame_count;

    for (uint32_t f = (uint32_t)fs64; f < (uint32_t)fe64; ++f) bm_clear(f);
}


void pmm_init_single(mem_region_t* r) {
    // If region is null, bitmap uninitialized, or page frame count 0,
    // there's no way to reserve anything so we return early.
    if (!r || !pagebitmap || frame_count == 0) return;

    // This function is generally for reserving a region after
    // bitmap initialization, so we only support reserving
    if (r->type == MEM_RESERVED) {
        bm_set_range_touched((uint32_t)r->base, (uint32_t)r->length);
    }
}


void pmm_init(mem_region_t* regions, size_t region_count) {
    // We need a valid location for the bitmap, and also we need the
    // size of the bitmap to reserve it. A left shift may overflow our
    // 32-bit math, so we calculate it as a 64-bit value and switch back
    // to 32-bit after the shift.
    uint64_t top_bytes64 = ((uint64_t)g_avail_memory_kib) << 10;
    if (top_bytes64 > 0x100000000ULL) top_bytes64 = 0x100000000ULL;
    frame_count = (uint32_t)(top_bytes64 >> 12);
    pagebitmap_size_bytes = (frame_count + 7u) >> 3;

    // Bitmap goes immediately after the kernel image
    uint64_t loc = (uint64_t)g_kernel_phys_address + (uint64_t)g_kernel_size_bytes;
    loc = (loc + 0xFFFULL) & ~0xFFFULL;

    // Check that the bitmap fits in memory, and halt if it doesn't. If this triggers, you may need to reduce g_avail_memory_kib in your stage2 binary.
    if (loc + (uint64_t)pagebitmap_size_bytes > top_bytes64)
        HALT_FOREVER;
    
    // Initialize bitmap to all 1s
    pagebitmap = (uint8_t*)((uint32_t)loc);
    for (uint32_t i = 0; i < pagebitmap_size_bytes; ++i) pagebitmap[i] = 0xFF;

    // Clear bits for all full pages in usable memory regions, and reserve all 
    // explicitly reserved regions. We do this in two separate passes to 
    // ensure that explicitly reserved regions that overlap with usable regions
    // are properly reserved.
    if (top_bytes64 > 0x00100000ULL) {
        uint64_t usable_len64 = top_bytes64 - 0x00100000ULL;
        if (usable_len64 > 0xFFFFFFFFULL) usable_len64 = 0xFFFFFFFFULL;
        bm_clear_range_full_pages(0x00100000u, (uint32_t)usable_len64);
    }

    // Reserve explicitly reserved regions, such as kernel image
    for (size_t i = 0; i < region_count; ++i) {
        if (regions[i].type == MEM_RESERVED) {
            bm_set_range_touched((uint32_t)regions[i].base, 
                (uint32_t)regions[i].length);
        }
    }

    // Lastly, reserve the bitmap itself
    bm_set_range_touched(loc, pagebitmap_size_bytes);
}

uint32_t pmm_alloc_frame(void) {
    if (frame_count == 0) return 0; // no memory available

    // Circular loop through bitmap starting at hint
    for (uint32_t f = frame_hint, counted = 0; counted < frame_count; f = ((f+1 >= frame_count) ? 0 : f+1), counted++) {
        // Short-circuit if byte is all 0s (all frames free)
        if (pagebitmap[f >> 3] == 0) {
            bm_set(f);
            frame_hint = (f + 1) % frame_count;
            return f << 12; // convert page frame number to physical frame address
        }

         // Otherwise we need to check each bit to find a free frame
         uint8_t mask = 1u << (f & 7);
         if ((pagebitmap[f >> 3] & mask) == 0) {
             bm_set(f);
             frame_hint = (f + 1) % frame_count;
             return f << 12; // convert page frame number to physical frame address
         }
    }

    return 0; // no free frame found
}

pmm_free_result_t pmm_free_frame(uint32_t address) {
    if (address % 4096 != 0) return PMM_PAGE_ADDRESS_UNALIGNED;
    // TODO: disallow freeing of reserved frames

    uint32_t f = address >> 12; // convert linear address to page frame number
    if (f >= frame_count) return PMM_PAGE_OUTOFBOUNDS;
    uint8_t mask = 1u << (f & 7);

    if (pagebitmap[f >> 3] & mask) {
        bm_clear(f);
        return PMM_PAGE_FREED;
    } else {
        return PMM_PAGE_ALREADYFREE; // frame already free, nothing to do
    }
}   

void pmm_reserve_range(uint32_t base, uint32_t length) {
    if (mem_region_count >= MEM_REGIONS_MAX) 
        return; // no more room to track regions
                // TODO: probably needs an error code of some sort

    size_t idx = mem_region_count;
    mem_regions[idx].base = base;
    mem_regions[idx].length = length;
    mem_regions[idx].type = MEM_RESERVED;
    mem_region_count = idx + 1;

    pmm_init_single((mem_region_t*)&mem_regions[idx]);
}
