
#include "kmain.h"
#include "kmem.h"
#include "paging.h"
#include "bootinfo.h"

#define MEM_REGIONS_MAX     256

// Helper macros since we have no cstdlib to work with here
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

// Physical memory management bitmap and region tracking structure
static uint8_t* pagebitmap;

// Physical address of memory bitmap
static uint32_t pagebitmap_phys = 0;

// Number of page frames (4KiB blocks / 8) in the bitmap, derived from memory map
static uint32_t pagebitmap_size_bytes = 0;

// Absolute count of frames tracked by the bitmap, derived from memory map
static uint32_t frame_count = 0;

// Current frame allocation hint, used to optimize search time for free frames
static uint32_t frame_hint = 0;

// Number of page frames available (intended to be read outside this file, but not written)
static uint64_t available_frames = 0;

// Flip-flop for whether we are using physical memory window alias
// (used partly for page manager staging; especially before high-half remap)
static bool use_phys_window_alias = false;

// Get a pointer to an address based on whether we are in phys window alias mode
// (e.g. virtual memory remap) or not (identity mapping before remap)
static inline uint8_t* frame_ptr(uint32_t phys_addr) {
    return use_phys_window_alias ? (uint8_t*)PHYS_TO_VIRT(phys_addr) : (uint8_t*)phys_addr;
}

// Memory region tracking, derived from E820 or other bootinfo method. Used for
// gating access to important or immutable regions so that they are not interacted
// with during nominal usage of the paging manager
volatile mem_region_t mem_regions[MEM_REGIONS_MAX] = {0};

// Total count of defined memory regions in mem_regions
volatile size_t mem_region_count = 0;

// BITMAP MANIPULATION HELPERS
// Sets a bit in the bitmap to indicate a frame is allocated.
static inline void bm_set(uint32_t f)   { 
    uint8_t val = pagebitmap[f >> 3];
    pagebitmap[f >> 3] |=  (1u << (f & 7));
    if (val != pagebitmap[f >> 3]) available_frames--; // decrement available frames count
}

// Clears a bit in the bitmap to indicate a frame is free.
static inline void bm_clear(uint32_t f) {
    uint8_t val = pagebitmap[f >> 3];
    pagebitmap[f >> 3] &= ~(1u << (f & 7));
    if (val != pagebitmap[f >> 3]) available_frames++; // only increment if we actually freed a frame
}

// STAGE2 BOOT CONSTANTS
#ifndef STAGE2_RESERVED_SECTORS
#warning "STAGE2_RESERVED_SECTORS not defined, defaulting to 6 (3KB) - this may cause boot issues if your stage2 is larger than 3KB, please adjust as needed and recompile stage2 and kernel"
// Note: stage2 is currently 2.5KB, so 6 sectors (3KB) is sufficient, but this is something to keep in mind if stage2 grows in the future
#define STAGE2_RESERVED_SECTORS 6
#endif

// Forward declaration for paging function defined in paging.c. We just want kmain to call
// mem_init to perform all-up memory management init, but we don't expose paging_init_identity_window
// in headers because it needs to be called exactly once.
extern uint32_t paging_init_identity_window(uint64_t identity_bytes, paging_status_t* out_status);

// Forward declaration for pmm functions defined in this file
// Initializes the physical memory manager, given a set of memory regions to track
// and a quantity
void pmm_init(mem_region_t* regions, size_t region_count);

// Reserves a specific range of frames into the global mem_regions_t array
pmm_reserve_result_t pmm_reserve_range(uint32_t base, uint32_t length, bool init);

void mem_init(void) {
    if (!g_bootinfo_validated) {
        // Should never happen
        HALT_FOREVER;
        return;
    }

    #define RESERVE_REGION(base, length, init) \
        do { \
            pmm_reserve_result_t r = pmm_reserve_range(base, length, init); \
            if (r != PMM_RESERVE_SUCCESS && r != PMM_UPDATED_EXISTING_REGION) { \
                kdbg_puts("fatal: failed to reserve region at ", 0x0C); \
                kdbg_hex32(base, 0x0C); \
                kdbg_puts(" of length ", 0x0C); \
                kdbg_hex32(length, 0x0C); \
                kdbg_puts("\r\nstatus: ", 0x0C); \
                kdbg_hex32((uint32_t)r, 0x0C); \
                kdbg_puts("\r\n", 0x0C); \
                kdbg_dump_current(); \
                HALT_FOREVER; \
            } \
        } while (0)

    // Kernel scratch memory reservation
    // Note: overlaps with VGA memory reservation, BIOS area,
    // bootinfo region, and possibly some small usable regions,
    // but this is simpler than trying to carve out usable regions
    // REQUIRED if we want to keep programs from allocating memory
    // below 1MB
    RESERVE_REGION(0, 0x100000, false);

    // optimization: frame hint can start at the end of this reserved region
    frame_hint = 0x100000 >> 12; // convert to frame number

    // Stage2 boot region
    // TODO: Should change 512 to build env macro
    RESERVE_REGION(0x10000, STAGE2_RESERVED_SECTORS * 512, false);

    // VGA/BIOS/ROM memory reservation
    RESERVE_REGION(0xA0000, 0x20000, false);
    RESERVE_REGION(0xC0000, 0x40000, false);

    // BOOTINFO reservation
    RESERVE_REGION(BOOTINFO_LINEAR_ADDR, BOOTINFO_SIZE_EXPECTED, false);

    // Kernel image reservation
    RESERVE_REGION(g_kernel_phys_address, (uint32_t)(&__kernel_end) - g_kernel_phys_address, false);

    // Kernel params reservation
    //RESERVE_REGION(g_kparams_phys_address, g_kparams_size_bytes, false);

    // Parse E820 memory map and mark unusable regions as reserved.
    if (g_memory_method == USE_E820) {
        for (size_t i = 0; i < g_e820_desc_count; i++) {
            volatile const e820_desc_t* d = &g_e820_descs[i];
            if (d->type != 1) {
                uint64_t base64 = d->base;
                uint64_t len64 = d->length;
                if (len64 == 0) continue;
                if (base64 >= 0x100000000ULL) continue; // outside 32-bit phys space

                uint64_t end64 = base64 + len64;
                if (end64 < base64 || end64 > 0x100000000ULL) {
                    end64 = 0x100000000ULL; // clamp overflow or >4GiB to 4GiB ceiling
                }

                uint32_t base32 = (uint32_t)base64;
                uint32_t len32 = (uint32_t)(end64 - base64);
                if (len32 == 0) continue;
                
                if (mem_region_count >= MEM_REGIONS_MAX) {
                    kdbg_puts("fatal: e820 memory map has more reserved regions than MEM_REGIONS_MAX\r\n", 0x0C);
                    HALT_FOREVER;
                    break;
                }

                RESERVE_REGION(base32, len32, false);
            }
        }
    }

    #undef RESERVE_REGION

    // init physical memory manager
    pmm_init((mem_region_t*)mem_regions, mem_region_count);

    // init paging with 1:1 mapping of all available memory
    paging_status_t pg_res;
    paging_init_identity_window(0, &pg_res);
    if (pg_res != PAGING_OK) {
        kdbg_puts("fatal: paging_init_identity_window failed with code ", 0x0C);
        kdbg_hex32((uint32_t)pg_res, 0x0C);
        kdbg_puts("\r\n", 0x0C);
        HALT_FOREVER;
    }

    kdbg_puts("g_kernel_phys_address: ", 0x0A);
    kdbg_hex32(g_kernel_phys_address, 0x0A);
    kdbg_puts("\r\ng_kernel_size_bytes: ", 0x0A);
    kdbg_hex32(g_kernel_size_bytes, 0x0A);
    kdbg_puts("\r\n__kernel_end: ", 0x0A);
    kdbg_hex32((uint32_t)&__kernel_end, 0x0A);
    kdbg_puts("\r\nbitmap start: ", 0x0A);
    kdbg_hex32((uint32_t)pagebitmap, 0x0A);
    kdbg_puts("\r\nbitmap end: ", 0x0A);
    kdbg_hex32((uint32_t)pagebitmap + pagebitmap_size_bytes, 0x0A);
    kdbg_puts("\r\n", 0x0A);
}

// Helper to mark a range of frames as used in the bitmap
static void bm_set_range_touched(uint32_t base, uint32_t len) {
    if (len == 0) return;

    uint64_t start = ((uint64_t)base) >> 12;
    uint64_t end   = (((uint64_t)base + (uint64_t)len + 0xFFFULL) >> 12); // ceil, exclusive

    if (end > (uint64_t)frame_count) end = frame_count;
    for (uint32_t f = (uint32_t)start; f < (uint32_t)end; ++f) bm_set(f);
}

// Helper to clear a range of frames as free in the bitmap
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

// Given a memory region, mark the corresponding frames as reserved in the bitmap
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

// Initializes the physical memory manager, given a set of memory regions to track
// and a quantity
void pmm_init(mem_region_t* regions, size_t region_count) {
    // We need a valid location for the bitmap, and also we need the
    // size of the bitmap to reserve it. A left shift may overflow our
    // 32-bit math, so we calculate it as a 64-bit value and switch back
    // to 32-bit after the shift.
    uint64_t top_bytes64 = ((uint64_t)g_avail_memory_kib) << 10;
    if (top_bytes64 > 0x100000000ULL) top_bytes64 = 0x100000000ULL;
    frame_count = (uint32_t)(top_bytes64 >> 12);
    pagebitmap_size_bytes = (frame_count + 7u) >> 3;
    available_frames = 0;

    // Bitmap goes immediately after the kernel image
    uint64_t loc = (uint64_t)((uint32_t)(&__kernel_end));
    loc = (loc + 0xFFFULL) & ~0xFFFULL;

    // Check that the bitmap fits in memory, and halt if it doesn't.
    if (loc + (uint64_t)pagebitmap_size_bytes > top_bytes64)
        HALT_FOREVER;
    
    // Initialize bitmap to all 1s
    pagebitmap_phys = (uint32_t)loc;
    pagebitmap = (uint8_t*)pagebitmap_phys;
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

// Switches to the physical window alias e.g. virtual address after higher-half remap.
// Otherwise, pointer to bitmap resides in lower half and will become invalid after remap
void pmm_switch_to_phys_window_alias(void) {
    if (!pagebitmap_phys) return;
    pagebitmap = (uint8_t*)PHYS_TO_VIRT(pagebitmap_phys);
    use_phys_window_alias = true;
}

// Checks if a given region overlaps with any explicitly reserved regions, and if so,
// returns true. If out_overlap provided and region turns out to be reserved, out_overlap is
// set to a pointer to the overlapping reserved region.
bool pmm_is_region_reserved(uint32_t address, uint32_t length, mem_region_t** out_overlap) {
    if (length == 0) return false; // we need nonzero length

    for(size_t i = 0; i < mem_region_count; i++) {
        volatile mem_region_t* r = &mem_regions[i];
        if (r->length == 0) continue; // skip zero-length regions
        uint64_t region_end = (uint64_t)r->base + (uint64_t)r->length;
        uint64_t span_end = (uint64_t)address + (uint64_t)length;

        // Check for ANY overlap between region and span
        //bool startOverlap = ((uint64_t)address >= (uint64_t)r->base) && ((uint64_t)address < region_end);
        //bool endOverlap = ((uint64_t)span_end > (uint64_t)r->base) && ((uint64_t)span_end <= region_end);
        //bool coversRegion = length != 0 && ((uint64_t)address <= (uint64_t)r->base) && ((uint64_t)span_end >= region_end);
        bool simplifiedOverlap = ((uint64_t)address < region_end) && ((uint64_t)r->base < (uint64_t)span_end);
        if (r->type == MEM_RESERVED && simplifiedOverlap) {
            if (out_overlap) 
                *out_overlap = (mem_region_t*)r;
            return true;
        }
    }
    return false;
}

// Allocates a single frame in the bitmap and returns a its physical address.
// If out_result provided, return code is set there to indicate nature of failure
// if not success.
uint32_t pmm_alloc_frame(pmm_alloc_result_t* out_result) {
    if (frame_count == 0) {
        if (out_result) *out_result = PMM_NO_PAGES_INITIALIZED;
        return 0;
    }

    // Circular loop through bitmap starting at hint
    for (uint32_t f = frame_hint, counted = 0; counted < frame_count; f = ((f+1 >= frame_count) ? 0 : f+1), counted++) {
        // If the current frame byte is 0, we know it's completely free and can
        // immediately set the current frame bit without further masking
        if (pagebitmap[f >> 3] == 0) {
            bm_set(f);
            frame_hint = (f + 1) % frame_count;
            if (out_result) *out_result = PMM_ALLOC_SUCCESS;
            return f << 12; // convert page frame number to physical frame address
        }

         // Otherwise we need to check each bit to find a free frame
         uint8_t mask = 1u << (f & 7);
         if ((pagebitmap[f >> 3] & mask) == 0) {
             bm_set(f);
             frame_hint = (f + 1) % frame_count;
             if (out_result) *out_result = PMM_ALLOC_SUCCESS;
             return f << 12; // convert page frame number to physical frame address
         }
    }

    if (out_result) *out_result = PMM_ALLOC_OOM;
    return 0; // no free frame found
}

// Checks if a given page-aligned physical address is allocated in the bitmap.
// True on success, false on malformed, OOB, or unallocated address.
bool pmm_query_bitmap(uint32_t address) {
    if (address % 4096 != 0) return false; // only page-frame aligned addresses are valid

    uint32_t f = address >> 12; // phys address to page frame number
    if (f >= frame_count) return false; // out of bounds addresses are not reserved

    // If bit is set, frame is reserved/allocated, otherwise it's free.
    // Note that if ever an explicitly reserved frame gets marked free in the
    // bitmap, pmm_query_bitmap may return true. For this reason, callers
    // may also use pmm_is_region_reserved to check if an address is reserved
    // explicitly to be able to safely use the address.
    return (pagebitmap[f >> 3] & (1u << (f & 7))) != 0;
}

// Reserves a specific range of frames into the global mem_regions_t array. 
// This is used for explicitly reserved regions that should be tracked permanently,
// NOT for transient allocations. Transient allocations should use the bitmap and
// therefore use pmm_alloc_frame and pmm_dealloc_frame instead.
pmm_reserve_result_t pmm_reserve_range(uint32_t base, uint32_t length, bool init) {
    // ensure we treat zero-length ranges as invalid and don't add them
    if (length == 0)
        return PMM_INVALID_REGION_LENGTH;

    // We check overlap/adjacency *before* slot-capacity checks because coalescing
    // into existing regions should still succeed even when MEM_REGIONS_MAX is
    // reached.
    //
    // Policy note:
    // - mem_regions tracks permanent forbidden ranges (kernel image, BIOS holes,
    //   paging structures, etc)
    // - bitmap tracks transient alloc/free state
    //
    // To avoid exhausting MEM_REGIONS_MAX during repeated 4KiB reservations,
    // we merge not only overlaps but also directly adjacent ranges.
    uint64_t merged_base = (uint64_t)base;
    uint64_t merged_end = (uint64_t)base + (uint64_t)length; // half-open [base, end)

    bool found_merge_candidate = false;
    size_t primary_idx = (size_t)-1;
    bool changed = false;

    // Fixed-point pass: expanding a range can make it adjacent/overlapping with
    // additional regions, so we keep scanning until no changes occur.
    do {
        changed = false;
        for (size_t i = 0; i < mem_region_count; i++) {
            volatile mem_region_t* r = &mem_regions[i];
            if (r->type != MEM_RESERVED || r->length == 0)
                continue;

            uint64_t r_base = (uint64_t)r->base;
            uint64_t r_end = r_base + (uint64_t)r->length; // half-open [r_base, r_end)

            // Merge criterion: overlap OR direct adjacency.
            // For half-open intervals this is: [a,b) and [c,d) merge if
            // a <= d && c <= b.
            bool can_merge = (merged_base <= r_end) && (r_base <= merged_end);
            if (!can_merge)
                continue;

            uint64_t old_merged_base = merged_base;
            uint64_t old_merged_end = merged_end;
            merged_base = MIN(merged_base, r_base);
            merged_end = MAX(merged_end, r_end);

            if (!found_merge_candidate) {
                primary_idx = i;
                found_merge_candidate = true;
                changed = true;
            } else if (i != primary_idx) {
                // Tombstone additional merged regions; we compact later.
                r->length = 0;
                r->type = MEM_USABLE;
                changed = true;
            }

            if (merged_base != old_merged_base || merged_end != old_merged_end)
                changed = true;
        }
    } while (changed);

    if (found_merge_candidate) {
        volatile mem_region_t* primary = &mem_regions[primary_idx];
        primary->base = (uint32_t)merged_base;
        primary->length = (uint32_t)(merged_end - merged_base);
        primary->type = MEM_RESERVED;

        // init=true means "apply reservation to bitmap now" for this span.
        // Re-applying to the merged region is safe/idempotent (bits remain set).
        if (init) pmm_init_single((mem_region_t*)primary);

        // Compact out tombstoned entries so region slots are recovered.
        size_t write = 0;
        for (size_t read = 0; read < mem_region_count; read++) {
            if (mem_regions[read].length == 0)
                continue;
            if (write != read)
                mem_regions[write] = mem_regions[read];
            write++;
        }
        mem_region_count = write;

        return PMM_UPDATED_EXISTING_REGION;
    }

    // No overlap to coalesce with; now we need a free slot for a new region record.
    if (mem_region_count >= MEM_REGIONS_MAX)
        return PMM_NO_MORE_REGION_SLOTS;

    size_t idx = mem_region_count;
    mem_regions[idx].base = base;
    mem_regions[idx].length = length;
    mem_regions[idx].type = MEM_RESERVED;
    mem_region_count = idx + 1;

    if (init) pmm_init_single((mem_region_t*)&mem_regions[idx]);
    return PMM_RESERVE_SUCCESS;
}

// Reserves a single frame in the bitmap and returns its physical address.
// If out_result provided, return code is set there to indicate nature of failure
// if not success. If a frame is reserved and happens to sit contiguous to a
// pre-existing reserved region below it in memory, the region below it is updated
// to include this reserved region, avoiding a common case of unnecessary usage in
// mem_regions.
// NOTE: This function adds a single page frame to the reserved region tracking 
// array. There is limited space in this array, meaning this should only be used
// for permanent reservations of important frames e.g. page directory, page tables, 
// etc.
paging_status_t pmm_reserve_pageframe(uint32_t* out_phys_addr, bool clear) {
    if (!out_phys_addr) return PAGING_ERR_INVALID; // TODO: enum this

    // E820 memory map could mean a memory hole in weird places that is system-defined
    // and out of our control. We can fudge this by allocating a frame for our PD,
    // keeping a copy of its address, freeing it, then reserving it, guaranteeing
    // we will always pick the first unreserved, unused frame.

    uint32_t ret_frame = frame_hint; // start at hint since it's likely to be near the end of reserved regions, but we will loop around if needed
    bool found = false;
    for (uint32_t f = ret_frame, counted = 0; counted < frame_count; f = ((f+1 >= frame_count) ? 0 : f+1), counted++) {
        if (!pmm_is_region_reserved(f << 12, 4096, NULL) && !pmm_query_bitmap(f << 12)) {
            ret_frame = f;
            found = true;
            break;
        }
    }
    if (!found) return PAGING_ERR_NOMEM; // no free frame (TODO: enum this)

    // Reserve frame in region tracking
    pmm_reserve_result_t reserve_result = pmm_reserve_range(ret_frame << 12, 4096, true);
    // Reservation may either add a new region or coalesce into an existing one;
    // both are successful outcomes from PMM's perspective.
    if (reserve_result != PMM_RESERVE_SUCCESS && reserve_result != PMM_UPDATED_EXISTING_REGION)
        return PAGING_ERR_FRAME_RESERVE_FAILED; // failed to reserve frame (TODO: enum this)
    
    // Update bitmap
    bm_set(ret_frame);

    // Update hint and set return value
    frame_hint = (ret_frame+1) % frame_count; // next search can start at the next frame to optimize contiguity
    *out_phys_addr = ret_frame << 12; // convert frame number to physical address
    
    // Zero out the reserved page frame if requested
    if (clear) {
        uint8_t* ptr = frame_ptr(*out_phys_addr);
        for (size_t i = 0; i < 4096; i++) ptr[i] = 0;
    }

    return PAGING_OK;
}

// Performs the same as pmm_reserve_pageframe, but for a sequence of contiguous page frames.
// The return value is a status code. Out parameters point to the physical addressees reserved as well
// as the count. The first contiguous sequence of frames is found, and the largest span (up to `requested`)
// is reserved. If `requested` frames cannot be reserved, the largest possible contiguous span of frames is
// reserved instead, and the out_count is updated to reflect this.
paging_status_t pmm_reserve_pageframe_seq(uint32_t requested, uint32_t* out_reserved, uint32_t* out_count, bool clear) {
    // This function does effectively the same thing as pmm_reserve_pageframe, but
    // for a sequence of contiguous page frame. The return value is a status code,
    // and the out parameters are a pointer to an array of reserved physical
    // addresses of page frames, as well as the count of how many were successfully
    // reserved. No allocations are performed, caller provides buffer, and must
    // ensure that buffer has at least 'requested' elements. Unless there are no
    // remaining page frames, if no other errors occur, this function is guaranteed
    // to return at least one reserved page frame.

    // sanity-check request
    if (!requested || !out_reserved || !out_count) return PAGING_ERR_INVALID; // TODO: enum this

    uint32_t curFrame = frame_hint; // start at hint
    bool found = false;
    for (uint32_t f = curFrame, counted = 0; counted < frame_count; f = ((f+1 >= frame_count) ? 0 : f+1), counted++) {
        // Walk the bitmap starting at the hint. We initially only look
        // for one free frame to find a starting point.
        if (!pmm_is_region_reserved(f << 12, 4096, NULL) && !pmm_query_bitmap(f << 12)) {
            curFrame = f;
            found = true;
            break;
        }
    }

    if (!found) return PAGING_ERR_NOMEM; // no free frame

    // We found a free frame. Try to reserve a contiguous space as big
    // as the request.

    // Step 1: Expand our window to the maximum size of the request, starting
    // from the free frame and ending at either bitmap-end or extents of the request,
    // whichever comes first.
    // 1A: Check if any element of window is reserved. If so, shrink window by 1 page.
    // 1B: Repeat 1A until we have a fully unreserved window, or shrink to 1 frame total.
    // 1C: When an unreserved window is found, check if it's taken in the bitmap. If so,
    //     shrink window to be [firstFreeFrame, firstTakenFrame-1] inclusive.
    // Step 2: Reserve whatever we're left with in region tracking and bitmap. Update
    //         output parameters.
    // Step 3: Set hint to frame after the newly reserved window.
    // Step 4: Clear out reserved window, if requested.

    // Step 1
    *out_count = requested;
    for (size_t i = requested; i > 0; i--) {
        uint32_t checkFrame = curFrame + i - 1;
        if (checkFrame >= frame_count) {
            *out_count = i - 1; // we can only reserve up to the end of the bitmap
            break;
        }
        if (pmm_is_region_reserved(checkFrame << 12, 4096, NULL)) {
            *out_count = i - 1; // we can only reserve up to the first reserved frame
            break;
        }
        if (pmm_query_bitmap(checkFrame << 12)) {
            *out_count = i - 1; // we can only reserve up to the first taken frame
            break;
        }    
    }

    // Step 2
    // Note: We reserve region as one big chunk, but set bits in bitmap piecemeal
    // This is to reduce churn in the region tracking structure, which has limited capacity
    pmm_reserve_result_t reserve_result = pmm_reserve_range(curFrame << 12, (*out_count) << 12, true);
    if (reserve_result != PMM_RESERVE_SUCCESS && reserve_result != PMM_UPDATED_EXISTING_REGION)
        return PAGING_ERR_FRAME_RESERVE_FAILED; // failed to reserve frame (TODO: enum this)
    for (size_t i = 0; i < *out_count; i++) {
        bm_set(curFrame + i);
        out_reserved[i] = (curFrame + i) << 12; // convert frame number to physical address
    }

    // Step 3
    frame_hint = (curFrame + *out_count) % frame_count;
    
    // Step 4
    if (clear) {
        for (size_t i = 0; i < *out_count; i++) {
            uint8_t* ptr = frame_ptr(out_reserved[i]);
            for (size_t j = 0; j < 4096; j++) ptr[j] = 0;
        }
    }
    
    return PAGING_OK;
}

// Given a page-aligned physical address, mark the corresponding frame as allocated.
// Returns status code indicating success or failure of the operation.
pmm_alloc_result_t pmm_alloc_specific_block(uint32_t page_aligned_phys_addr) {
    // sanity-check input
    if (page_aligned_phys_addr % 4096 != 0) 
    return PMM_ALLOC_UNALIGNED_ADDR;
    
    uint32_t f = page_aligned_phys_addr >> 12; // phys address to page frame number
    if (f >= frame_count) return PMM_ALLOC_OUTOFBOUNDS; // out of bounds addresses are not reservable

    if (pmm_is_region_reserved(page_aligned_phys_addr, 4096, NULL))
        return PMM_ALLOC_RESERVED; // cannot allocate a page that is reserved by an explicitly reserved region

    if (pmm_query_bitmap(page_aligned_phys_addr))
        return PMM_ALLOC_ALREADY_ALLOCATED; // cannot allocate a page that is already allocated
    
    bm_set(f);
    return PMM_ALLOC_SUCCESS;
}

// Given a page-aligned physical address, mark the corresponding frame as free.
// Returns status code indicating success or failure of the operation.
pmm_alloc_result_t pmm_dealloc_specific_block(uint32_t page_aligned_phys_addr) {
    // sanity-check input
    if (page_aligned_phys_addr % 4096 != 0) 
        return PMM_ALLOC_UNALIGNED_ADDR;

    if (frame_count == 0)
        return PMM_NO_PAGES_INITIALIZED;
    
    if (!pagebitmap)
        return PMM_BITMAP_NOT_INITIALIZED;
    
    
    uint32_t f = page_aligned_phys_addr >> 12; // phys address to page frame number
    if (f >= frame_count) return PMM_ALLOC_OUTOFBOUNDS; // out of bounds addresses are not deallocatable

    if (pmm_is_region_reserved(page_aligned_phys_addr, 4096, NULL))
        return PMM_ALLOC_RESERVED; // cannot deallocate a page that is reserved by an explicitly reserved region

    if (!pmm_query_bitmap(page_aligned_phys_addr))
        return PMM_ALLOC_ALREADY_FREE; // cannot deallocate a page that is already free
    
    // update hint to this frame so that next alloc is likely to happen
    // immediately after this deallocation
    frame_hint = f; 

    // clear bit in bitmap, return success
    bm_clear(f);
    return PMM_ALLOC_SUCCESS;
}

// Returns the physical address of the next available page-aligned block.
// Provides status code in out_status if not null, indicating success or
// nature of failure.
uint32_t pmm_get_next_available_block(pmm_alloc_result_t* out_status) {
    // Helper function for iterating the bitmap and finding an available block
    // (bit marked as 0). If found, marks it as 1 (allocated) and returns the
    // physical address to it. If no available block is found, returns 0.
    // If not null, out_status is set to an appropriate status code.
    if (frame_count == 0) {
        if (out_status) *out_status = PMM_NO_PAGES_INITIALIZED;
        return 0;
    }

    if (!pagebitmap) {
        if (out_status) *out_status = PMM_BITMAP_NOT_INITIALIZED;
        return 0;
    }

    for (uint32_t f = frame_hint, counted = 0; counted < frame_count; f = ((f+1 >= frame_count) ? 0 : f+1), counted++) {
        // Short-circuit heuristic: if the byte is 0, all bits are 0, therefore we
        // can return the first bit without further masking. Common occurrence in
        // early allocations when most frames are free.
        if (pagebitmap[f >> 3] == 0) {
            // Sanity check: ensure this frame does not fall within within an explicitly
            // reserved region. If it does, we have a bitmap integrity problem.
            if (pmm_is_region_reserved(f << 12, 4096, NULL)) {
                if (out_status) 
                    *out_status = PMM_BITMAP_INCONSISTENT_RESERVED;
                return 0;
            }

            bm_set(f);
            frame_hint = (f + 1) % frame_count;
            if (out_status) 
                *out_status = PMM_ALLOC_SUCCESS;
            return f << 12; // convert page frame number to physical frame address
        }

        // If short-circuit heuristic didn't hit, check each bit in the byte.
        uint8_t mask = 1u << (f & 7);
        if ((pagebitmap[f >> 3] & mask) == 0) {
            // Perform the same sanity check as above for a candidate frame found to be 0.
            if (pmm_is_region_reserved(f << 12, 4096, NULL)) {
                if (out_status) 
                    *out_status = PMM_BITMAP_INCONSISTENT_RESERVED;
                return 0;
            }
            bm_set(f);
            frame_hint = (f + 1) % frame_count;
            if (out_status) *out_status = PMM_ALLOC_SUCCESS;
            return f << 12; // convert page frame number to physical frame address
        }
    }

    // No available block found,
    if (out_status) *out_status = PMM_ALLOC_OOM;
    return 0;
}

uint64_t get_estimated_available_frames(void) {
    return available_frames;
}

_Static_assert(MEM_REGIONS_MAX >= 16, "MEM_REGIONS_MAX too small");
