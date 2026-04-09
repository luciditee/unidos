
#include "mm.h"
#include "dpage.h"

// externs unsuitable for exposure in headers, but needed for mm
extern uint32_t paging_get_kernel_pd_phys(void);

// Bitmap which tracks virtual addresses available to use for vma allocation.
// The bitmap lives at VMA_BITMAP_BASE (immediately below the pool) so it
// never overlaps with the vma_t structs that start at VMA_VIRTUAL_BASE.
// Pool extent is capped at MMS_BITMAP_BASE to prevent overlap with the mm pool.
static uint8_t* vma_vaddr_bitmap = (uint8_t*)VMA_BITMAP_BASE;
static uint32_t vma_vaddr_size_bytes = (MMS_BITMAP_BASE - VMA_VIRTUAL_BASE) & ~0xFFFu;

void vma_init(void) {
    if (!dpage_init(VMA_BITMAP_BASE, VMA_VIRTUAL_BASE, vma_vaddr_bitmap))
        panic("failed to initialize vma demand-paged pool", NULL);
}

vma_t* vma_alloc(void) {
    dpage_alloc_status_t out_status;
    vma_t* ret = (vma_t*)dpage_alloc(vma_vaddr_bitmap, 
        VMA_VIRTUAL_BASE, vma_vaddr_size_bytes,
        sizeof(vma_t), vma_vaddr_size_bytes / 8,
        &out_status);

    if (ret != NULL) {
        ret->start  = 0;
        ret->end    = 0;
        ret->type   = 0;
        ret->prot   = 0;
        ret->next   = NULL;
        ret->slot_id = ((uint32_t)ret - VMA_VIRTUAL_BASE) / sizeof(vma_t);
    }

    // TODO: pass status out of this function
    return ret;
}

void vma_free(vma_t* vma) {
    if (vma == NULL) return;
    dpage_bm_clear(vma->slot_id, vma_vaddr_bitmap, vma_vaddr_size_bytes / 8);
}

void vma_insert(mm_t* mm, vma_t* new_vma) {
    if (mm == NULL || new_vma == NULL) return;

    // If the list is empty or the new VMA should be the new head
    if (mm->vma_head == NULL || new_vma->start < mm->vma_head->start) {
        new_vma->next = mm->vma_head;
        mm->vma_head = new_vma;
        return;
    }

    // Otherwise, find the correct position to insert the new VMA
    vma_t* current = mm->vma_head;
    while (current->next != NULL && current->next->start < new_vma->start) {
        current = current->next;
    }

    // Insert the new VMA
    new_vma->next = current->next;
    current->next = new_vma;
}

void vma_remove(mm_t* mm, vma_t* v) {
    if (mm == NULL || v == NULL || mm->vma_head == NULL) return;

    // If the VMA to remove is the head of the list
    if (mm->vma_head == v) {
        mm->vma_head = v->next;
        return;
    }

    // Otherwise, find the VMA in the list and remove it
    vma_t* current = mm->vma_head;
    while (current->next != NULL) {
        if (current->next == v) {
            current->next = v->next;
            return;
        }
        current = current->next;
    }
}

void vma_destroy_all(mm_t* mm) {
    if (mm == NULL) return;

    vma_t* current = mm->vma_head;
    while (current != NULL) {
        vma_t* next = current->next;
        vma_free(current);
        current = next;
    }
    mm->vma_head = NULL;
}

vma_t* vma_find(mm_t* mm, uintptr_t vaddr) {
    if (mm == NULL) return NULL;

    vma_t* current = mm->vma_head;
    while (current != NULL) {
        if (current->start <= vaddr && vaddr < current->end) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

int mm_map_region(mm_t* mm, uint32_t va_start, size_t size, vma_type_t type, uint32_t prot, const void* initial_data, size_t initdata_size) {
    if (mm == NULL) return -1;
    if (size == 0) return -1;
    if ((va_start & 0xFFFu) != 0) return -1; // must be page-aligned
    if ((size & 0xFFFu) != 0) return -1; // must be multiple of page size
    if (va_start < mm->user_base || (va_start + size) > mm->user_limit) return -1; // must be within user-space range

    vma_t* new_vma = vma_alloc();
    if (new_vma == NULL) return -1;

    new_vma->start = va_start;
    new_vma->end = va_start + size;
    new_vma->type = type;
    new_vma->prot = prot;

    // Translate VMA protection flags to PTE flags
    uint32_t pte_flags = PG_PRESENT | PG_USER;
    if (prot & VMA_PROT_WRITE) pte_flags |= PG_RW;

    // Access the target mm's page directory (NOT the currently loaded one)
    volatile uint32_t* pd = (uint32_t*)PHYS_TO_VIRT(mm->cr3_phys);

    uint32_t pages = size / 4096;
    size_t data_copied = 0;

    for (uint32_t p = 0; p < pages; p++) {
        uint32_t va = va_start + (p * 4096);
        uint32_t pdi = va >> 22;
        uint32_t pti = (va >> 12) & 0x3FFu;

        // Allocate physical frame for this page
        pmm_alloc_result_t pmm_res;
        uint32_t frame = pmm_get_next_available_block(&pmm_res);
        if (pmm_res != PMM_ALLOC_SUCCESS) {
            // TODO: unwind -- free frames allocated so far, remove VMA
            // procedure: walk backwards from page p-1, to 0, read each PTE
            // from target PD, free frame, clear PTE; *then* remove/free VMA
            vma_remove(mm, new_vma);
            vma_free(new_vma);
            return -1;
        }

        // Ensure page table exists for this PDE
        if ((pd[pdi] & PG_PRESENT) == 0) {
            pmm_alloc_result_t pt_res;
            uint32_t pt_frame = pmm_get_next_available_block(&pt_res);
            if (pt_res != PMM_ALLOC_SUCCESS) {
                pmm_dealloc_specific_block(frame);
                // TODO: unwind prior pages
                vma_remove(mm, new_vma);
                vma_free(new_vma);
                return -1;
            }
            // Zero the new page table
            uint32_t* new_pt = (uint32_t*)PHYS_TO_VIRT(pt_frame);
            for (size_t z = 0; z < 1024; z++) new_pt[z] = 0;
            pd[pdi] = pt_frame | PG_PRESENT | PG_RW | PG_USER;
        }

        // Write PTE into the target mm's page table
        volatile uint32_t* pt = (uint32_t*)PHYS_TO_VIRT(pd[pdi] & ~0xFFFu);
        pt[pti] = frame | pte_flags;

        // Initialize page contents
        uint32_t* page_ptr = (uint32_t*)PHYS_TO_VIRT(frame);
        // Zero the entire page first (BSS semantics)
        for (size_t z = 0; z < 1024; z++) page_ptr[z] = 0;

        // Copy initial data if provided
        if (initial_data && data_copied < initdata_size) {
            size_t to_copy = initdata_size - data_copied;
            if (to_copy > 4096) to_copy = 4096;
            kmemcpy(page_ptr, (const uint8_t*)initial_data + data_copied, to_copy);
            data_copied += to_copy;
        }

        mm->page_count++;
    }

    vma_insert(mm, new_vma);
    return 0;
}