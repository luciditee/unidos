
#include "mm.h"
#include "kmem.h"
#include "paging.h"
#include "kmain.h"
#include "panic.h"

// All pages whose starts reside at 0xFFFF8000..0xFFFFF000 are reserved
// for copying mm_t structs to reduce dependency on physical/IDmapped 
// window and reduce TLB thrashing. As yet unused, but reserving this now
// Intended mapping:
// ...8000 -> parent PD
// ...9000 -> child PD
// ...A000 -> parent PT
// ...B000 -> child PT
// ...C000 -> parent page
// ...D000 -> child page
// (last two for spare)
#define TEMPMAPPING_BASE 0xFFFF8000

// The last page above is also used by mm_create for spooling up a 
// temporary PD for initializing an mm_t struct
#define TEMP_PAGE_VA 0xFFFFF000

// externs unsuitable for exposure in headers, but needed for mm
extern uint32_t paging_get_kernel_pd_phys(void);

// Bitmap which tracks virtual addresses available to use for mm allocation
static uint8_t* mm_vaddr_bitmap = (uint8_t*)MMS_VIRTUAL_BASE;// placeholder, will be initialized properly in mm_init
static uint32_t mm_vaddr_size_bytes = (STACK_VIRTUAL_BASE - MMS_VIRTUAL_BASE) & ~0xFFFu;

// Sets a bit in the bitmap to indicate a slot is taken.
static inline void bm_set(uint32_t f)   { 
    mm_vaddr_bitmap[f >> 3] |=  (1u << (f & 7));
}

// Clears a bit in the bitmap to indicate a slot is free.
static inline void bm_clear(uint32_t f) {
    mm_vaddr_bitmap[f >> 3] &= ~(1u << (f & 7));
}

static inline uint32_t bm_slot_to_vaddr(uint32_t slot) {
    return MMS_VIRTUAL_BASE + (slot * sizeof(mm_t));
}

// Assumes the current PD is the kernel PD. Returns the physical address
uint32_t _mm_alloc_pd(mm_status_t* out_status) {
    // We can't rely on kernel-mode PD for this because we use this
    // to allocate new PDs for user-space processes, so we need to engage
    // the PMM to give us a page frame, zero it out, copy the kernel mode PD,
    // flush the TLB, and return it.
    pmm_alloc_result_t res;
    uint32_t phys = pmm_get_next_available_block(&res);
    if (res != PMM_ALLOC_SUCCESS) {
        if (out_status) *out_status = MM_PD_PMM_INIT_FAILED;
        return 0;
    }

    // Single page-sized mapping slot
    // Caller MUST ensure interrupts are disabled e.g. io.h irq_save_disable()
    // to prevent preemption while the temp mapping is actively in use.
    const uint32_t temp_page = TEMP_PAGE_VA; // last page in virtual address space, guaranteed to be unmapped
                                              

    paging_status_t map_res = paging_map_page(temp_page, phys, PG_PRESENT | PG_RW, NULL);
    if (map_res != PAGING_OK) {
        // If we fail to map this page for some reason, we should free the allocated frame and return failure.
        pmm_dealloc_specific_block(phys);
        if (out_status) *out_status = MM_PD_PG_INIT_FAILED;
        return 0;
    }

    // Zero out the new PD page, copy kernel PD into it, and flush TLB
    uint32_t* temp_ptr = (uint32_t*)temp_page;
    for (uint32_t i = 0; i < 1024; i++) temp_ptr[i] = 0;
    
    // Copy the kernel-mapping portion of the kernel PD
    // Arithmetic for determining the kernel region assumes we give the kernel
    // some address space aligned to the most-significant hextet of vaddr space
    // Nominally, it's 0xC0000000. This means we can do:
    // KERNEL_VIRTUAL_BASE >> 22 == 768, so we can copy PDEs 768-1023 from the 
    // kernel PD into the new PD to give the new PD the same kernel mapping. This
    // is important because it allows us to use the same higher-half kernel mapping
    // for all processes
    uint32_t* kernel_pd_ptr = (uint32_t*)PHYS_TO_VIRT(paging_get_kernel_pd_phys());
    for (uint32_t i = (KERNEL_VIRTUAL_BASE >> 22); i < 1024; i++) temp_ptr[i] = kernel_pd_ptr[i];

    // Now that the new PD is ready, we can unmap the temporary mapping and return the physical address of the new PD
    uint32_t unmap_phys;
    paging_status_t unmap_status = paging_unmap_page(temp_page, &unmap_phys);
    if (unmap_status != PAGING_OK)
        panic("Failed to unmap temporary page used for MM PD allocation", NULL);
    
    if (unmap_phys != phys)
        panic("Physical address mismatch when unmapping temporary page used for MM PD allocation", NULL);

    return phys;
}

mm_t* _mm_alloc_mm(mm_status_t* out_status) {
    for (uint32_t slot = 0; slot < (mm_vaddr_size_bytes / sizeof(mm_t)); slot++) {
        if ((mm_vaddr_bitmap[slot >> 3] & (1u << (slot & 7))) == 0) {
            // Found a free slot, mark it as taken and return the corresponding vaddr
            bm_set(slot);
            if (out_status) *out_status = MM_SUCCESS;
            
            // Lazy mapping: This vaddr might not yet be mapped. Get the page
            // this vaddr would be on, and if it's not mapped, map it.
            uint32_t vaddr = bm_slot_to_vaddr(slot);

            // It's possible that the mm_t struct spans two pages, so we need to
            // check both the page of the starting address and the page of the ending address.
            uint32_t pages_to_check[2];
            uint32_t num_pages = 1;
            pages_to_check[0] = vaddr & ~0xFFFu;
            pages_to_check[1] = (vaddr + sizeof(mm_t) - 1) & ~0xFFFu;
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

                        bm_clear(slot);

                        if (out_status) *out_status = MM_MM_PMM_INIT_FAILED;
                        return NULL;
                    }
                    
                    paging_status_t map_res = paging_map_page(page_start, phys, PG_PRESENT | PG_RW, NULL);
                    if (map_res != PAGING_OK) {
                        
                        pmm_dealloc_specific_block(phys);

                        if (p > 0) {
                            // If we already mapped the first page but failed on the second, we should unmap the first page and free its frame before returning
                            uint32_t unmap_phys;
                            paging_unmap_page(pages_to_check[0], &unmap_phys);
                            pmm_dealloc_specific_block(unmap_phys);
                        }

                        bm_clear(slot);
                        if (out_status) *out_status = MM_MM_PG_INIT_FAILED;
                        return NULL;
                    }
                }
            }

            // If here, the page(s) are mapped, so we can zero out the specific region
            // we care about
            uint32_t* ptr = (uint32_t*)vaddr;
            for (uint32_t i = 0; i < (sizeof(mm_t) / 4); i++) ptr[i] = 0;

            return (mm_t*)vaddr;
        }
    }

    // This should never happen because we track availability with mm_bitmap_slots_avail, but just in case:
    if (out_status) *out_status = MM_NO_MORE_REGION_SLOTS;
    return NULL;
}

mm_t* mm_create(void) {
    mm_status_t status;
    uintptr_t pd = (uintptr_t)_mm_alloc_pd(&status);
    if (!pd) return NULL; // TODO: pass status code out

    mm_t* mm = _mm_alloc_mm(&status);
    if (!mm) {
        // Free the PD we allocated since the mm failed to allocate
        pmm_dealloc_specific_block(pd);

        return NULL; // TODO: pass status code out
    }

    mm->cr3_phys = pd;
    mm->refcount = 1;
    mm->user_base = 0; // TODO: should probably set this to be the same as Linux's
    mm->user_limit = KERNEL_VIRTUAL_BASE; // user space must be below kernel space
    mm->page_count = 0;
    mm->slot_id = ((uint32_t)mm - MMS_VIRTUAL_BASE) / sizeof(mm_t);

    return mm;
}

mm_t* mm_clone_user_eager(mm_t* parent) {
    if (parent == NULL) return NULL;
    
    mm_t* child = mm_create();
    if (child == NULL) return NULL;

    volatile uint32_t* parent_pd = (uint32_t*)PHYS_TO_VIRT(parent->cr3_phys);
    volatile uint32_t* child_pd  = (uint32_t*)PHYS_TO_VIRT(child->cr3_phys);

    static const size_t max_pde = (KERNEL_VIRTUAL_BASE >> 22);
    for (size_t i = 0; i < max_pde; i++) {
        if ((parent_pd[i] & PG_PRESENT) == 0) continue;

        uint32_t parent_pt_phys = parent_pd[i] & ~0xFFFu;
        uint32_t parent_pde_flags = parent_pd[i] & 0xFFFu;

        // Allocate a new PT frame for the child
        pmm_alloc_result_t pmm_res;
        uint32_t child_pt_phys = pmm_get_next_available_block(&pmm_res);
        if (pmm_res != PMM_ALLOC_SUCCESS) {
            mm_destroy(child);
            return NULL;
        }

        // Access both PTs via physical window
        volatile uint32_t* parent_pt = (uint32_t*)PHYS_TO_VIRT(parent_pt_phys);
        volatile uint32_t* child_pt  = (uint32_t*)PHYS_TO_VIRT(child_pt_phys);

        // Zero child PT first
        for (size_t z = 0; z < 1024; z++) child_pt[z] = 0;

        // Write PDE -- doing it here so we can use mm_destroy later to unwind
        child_pd[i] = child_pt_phys | parent_pde_flags;

        for (size_t j = 0; j < 1024; j++) {
            if ((parent_pt[j] & PG_PRESENT) == 0) continue;

            uint32_t phys_src = parent_pt[j] & ~0xFFFu;
            uint32_t pte_flags = parent_pt[j] & 0xFFFu;

            // Allocate a new frame for the child's copy of this page
            uint32_t child_frame = pmm_get_next_available_block(&pmm_res);
            if (pmm_res != PMM_ALLOC_SUCCESS) {
                mm_destroy(child);
                return NULL;
            }

            // copy page contents -- using physical window for now
            uint32_t* src = (uint32_t*)PHYS_TO_VIRT(phys_src);
            uint32_t* dst = (uint32_t*)PHYS_TO_VIRT(child_frame);
            kmemcpy(dst, src, 4096);

            // Write the PTE into the child's page table
            child_pt[j] = child_frame | pte_flags;
            child->page_count++;
        }
    }

    return child;
}

void mm_destroy(mm_t* mm) {
    if (mm == NULL) return;

    // Definitely don't want to destroy the currently loaded address space,
    // so we check if the PD we're destroying is currently loaded
    uint32_t current_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(current_cr3));
    if ((current_cr3 & ~0xFFFu) == mm->cr3_phys) {
        panic("mm_destroy: attempted to destroy the active address space", NULL);
    }

    // Get reference to the PD of the mm we're destroying
    volatile uint32_t* pd = (uint32_t*)PHYS_TO_VIRT(mm->cr3_phys);

    static const size_t max_pde = (KERNEL_VIRTUAL_BASE >> 22);
    for (size_t i = 0; i < max_pde; i++) {
        if ((pd[i] & PG_PRESENT) == 0) continue;

        uint32_t pt_phys = pd[i] & ~0xFFFu;
        volatile uint32_t* pt = (uint32_t*)PHYS_TO_VIRT(pt_phys);

        for (size_t j = 0; j < 1024; j++) {
            if ((pt[j] & PG_PRESENT) == 0) continue;

            uint32_t page_phys = pt[j] & ~0xFFFu;
            pmm_dealloc_specific_block(page_phys);
        }

        pmm_dealloc_specific_block(pt_phys);
    }

    pmm_dealloc_specific_block(mm->cr3_phys);
    
    // Mark the location for this mm struct as free in the bitmap
    bm_clear(mm->slot_id);
}

// Switches current active page directory to the one specified in the
// given mm struct. Caller is responsible for ensuring the mm struct is
// valid and that interrupts are disabled.
void mm_switch(mm_t* mm) {
    if (mm == NULL) return;

    uint32_t ptbase;
    __asm__ __volatile__ (
        "movl %%cr3, %0\n"
        : "=r" (ptbase)
    );

    if ((uintptr_t)(ptbase & ~0xFFFu) == (mm->cr3_phys & ~0xFFFu)) {
        // Already using this address space, avoid switching so we don't
        // unnecessarily flush the TLB
        return;
    }

    __asm__ __volatile__ (
        "movl %0, %%cr3\n"
        :
        : "r" (mm->cr3_phys)
    );  
}
