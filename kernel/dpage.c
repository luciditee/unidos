
#include "dpage.h"

bool dpage_init(uint32_t bitmap_base, uint32_t pool_base, uint8_t* bm) {
    // Map the first page of the bitmap region so dpage_alloc can read it
    pmm_alloc_result_t res;
    uint32_t bm_phys = pmm_get_next_available_block(&res);
    if (res != PMM_ALLOC_SUCCESS)
        return false;

    if (paging_map_page(bitmap_base, bm_phys, PG_PRESENT | PG_RW, NULL) != PAGING_OK) {
        pmm_dealloc_specific_block(bm_phys);
        return false;
    }

    // Zero the bitmap -- all slots start free
    bm = (uint8_t*)bitmap_base;
    for (uint32_t i = 0; i < 4096; i++) bm[i] = 0;

    // Pre-map the first page of the pool itself so that the PDE covering
    // pool_base exists in the kernel master PD before any per-process PDs
    // are cloned (they copy kernel PDEs 768-1023 at snapshot time).  If
    // the bitmap and pool share the same PDE this is harmless -- the PDE
    // already exists and paging_map_page simply creates a second PTE.
    uint32_t pool_phys = pmm_get_next_available_block(&res);
    if (res != PMM_ALLOC_SUCCESS)
        return false;

    if (paging_map_page(pool_base, pool_phys, PG_PRESENT | PG_RW, NULL) != PAGING_OK) {
        pmm_dealloc_specific_block(pool_phys);
        return false;
    }

    // Zero the pool page so dpage_alloc's presence check sees it as
    // already mapped and doesn't try to double-map it.
    uint8_t* p = (uint8_t*)pool_base;
    for (uint32_t i = 0; i < 4096; i++) p[i] = 0;

    return true;
}

void* dpage_alloc(kbitmap_t vaddr_bitmap, uint32_t vaddr_base, uint32_t vaddr_size_bytes, size_t element_size, uint32_t bitmap_size_bytes, dpage_alloc_status_t* out_status) {
    for (uint32_t slot = 0; slot < (bitmap_size_bytes * 8); slot++) {
        if ((vaddr_bitmap[slot >> 3] & (1u << (slot & 7))) == 0) {
            // Found a free slot, mark it as taken and return the corresponding vaddr
            dpage_bm_set(slot, vaddr_bitmap, bitmap_size_bytes);
            if (out_status) *out_status = DPAGE_SUCCESS;
            
            // Lazy mapping: This vaddr might not yet be mapped. Get the page
            // this vaddr would be on, and if it's not mapped, map it.
            uint32_t vaddr = dpage_bm_slot_to_vaddr(slot, vaddr_base, element_size);

            // It's possible that the allocated block spans two pages, so we need to
            // check both the page of the starting address and the page of the ending address.
            uint32_t pages_to_check[2];
            uint32_t num_pages = 1;
            bool mapped_already[2] = {false, false}; // track whether we mapped each page so we can unmap if something goes wrong
            pages_to_check[0] = vaddr & ~0xFFFu;
            pages_to_check[1] = (vaddr + element_size - 1) & ~0xFFFu;
            if (pages_to_check[1] != pages_to_check[0]) {
                num_pages = 2;
            }

            for (uint32_t p = 0; p < num_pages; p++) {
                uint32_t page_start = pages_to_check[p];
                volatile uint32_t* kpd = (uint32_t*)PHYS_TO_VIRT(paging_get_kernel_pd_phys());
                uint32_t pdi = page_start >> 22;
                uint32_t pti = (page_start >> 12) & 0x3FFu;
                
                // Find if it's present in the kernel PD, and use this to determine
                // whether we've already mapped this before
                bool need_map = false;
                if ((kpd[pdi] & PG_PRESENT) == 0) {
                    need_map = true;
                } else {
                    uint32_t* pt = (uint32_t*)PHYS_TO_VIRT(kpd[pdi] & ~0xFFFu);
                    if ((pt[pti] & PG_PRESENT) == 0) {
                        need_map = true;
                    }
                }
                
                // We found a need to map this page, so we should demand-map it in the kernel PD
                if (need_map) {
                    pmm_alloc_result_t res;
                    uint32_t phys = pmm_get_next_available_block(&res);
                    if (res != PMM_ALLOC_SUCCESS) {

                        if (p > 0) {
                            // If we already mapped the first page but failed on the second, we should unmap the first page and free its frame before returning
                            uint32_t unmap_phys;
                            paging_unmap_page(pages_to_check[0], &unmap_phys);
                            pmm_dealloc_specific_block(unmap_phys);
                        }

                        dpage_bm_clear(slot, vaddr_bitmap, bitmap_size_bytes);

                        if (out_status) *out_status = DPAGE_PMM_INIT_FAILED;
                        return NULL;
                    }
                    
                    paging_status_t map_res = paging_map_page(page_start, phys, PG_PRESENT | PG_RW, NULL);
                    if (map_res != PAGING_OK) {
                        
                        pmm_dealloc_specific_block(phys);

                        if (p > 0 && mapped_already[0]) {
                            // If we already mapped the first page but failed on the second, we should unmap the first page and free its frame before returning
                            uint32_t unmap_phys;
                            paging_unmap_page(pages_to_check[0], &unmap_phys);
                            pmm_dealloc_specific_block(unmap_phys);
                        }

                        dpage_bm_clear(slot, vaddr_bitmap, bitmap_size_bytes);
                        if (out_status) *out_status = DPAGE_PG_INIT_FAILED;
                        return NULL;
                    }

                    mapped_already[p] = true;
                }
            }

            // If here, the page(s) are mapped, so we can zero out the specific region
            // we care about
            uint32_t* ptr = (uint32_t*)vaddr;
            for (uint32_t i = 0; i < (element_size / 4); i++) ptr[i] = 0;

            return (void*)vaddr;
        }
    }

    // Failed to find a free slot. Caller should interpret as OOM.
    if (out_status) *out_status = DPAGE_NO_MORE_REGION_SLOTS;
    return NULL;
}