
#include "kmain.h"
#include "kmem.h"
#include "isr.h"
#include "bootinfo.h"
#include "paging.h"

#define PT_MAX 1024 // max PDE/PT slots in non-PAE 32-bit paging
#define PF_VECTOR 0x0E

static volatile uint32_t page_directory_loc_phys = 0;
static volatile uint32_t page_tables_loc_phys[PT_MAX] = {0};
static volatile uint32_t page_table_count = 0;

extern paging_status_t pmm_reserve_pageframe_seq(uint32_t requested, uint32_t* out_reserved, uint32_t* out_count, bool clear);
extern paging_status_t pmm_reserve_pageframe(uint32_t* out_phys_addr, bool clear);

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
    uint32_t needed_pts = needed_frames - 1;

    // needed_frames includes one frame for PD + needed_pts frames for PTs
    if (needed_frames > (PT_MAX + 1)) {
        if (out_status)
            *out_status = PAGING_ERR_EXCESS_PAGES_REQUESTED;
        return 0;
    }
    uint32_t reserved_frames[PT_MAX + 1] = {0};
    uint32_t remaining = needed_frames;
    kdbg_puts("reserving ", 0x0A);
    kdbg_hex32(needed_frames, 0x0A);
    kdbg_puts(" page frames for paging structures", 0x0A);
    while (remaining > 0) {
        uint32_t reserved_this_round = 0;
        int res = pmm_reserve_pageframe_seq(remaining, reserved_frames + (needed_frames - remaining), &reserved_this_round, true);
        if (res != PAGING_OK || reserved_this_round == 0) {
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

    // Normalize metadata: page_tables_loc_phys is indexed by PDE index.
    // Reset all slots first; then install only the PTs we created.
    for (uint32_t i = 0; i < PT_MAX; i++) {
        page_tables_loc_phys[i] = 0;
    }
    page_table_count = 0;

    // init page directory for identity mapping
    uint32_t* pd = (uint32_t*)page_directory_loc_phys;
    for (uint32_t i = 0; i < PT_MAX; i++) {
        pd[i] = 0;
    }

    for (uint32_t pdi = 0; pdi < needed_pts; pdi++) {
        uint32_t pt_phys = reserved_frames[pdi + 1];
        page_tables_loc_phys[pdi] = pt_phys;
        pd[pdi] = pt_phys | PG_PRESENT | PG_RW; // map PDE[pdi] to PT[pdi]
    }
    page_table_count = needed_pts;

    // init page tables for identity mapping
    uint32_t pages_mapped = (identity_bytes + 0xFFF) >> 12; // round to nearest page
    for (uint32_t i = 0; i < pages_mapped; i++) {
        uint32_t virt = i << 12; // virtual address being mapped, also the physical address since identity mapping
        uint32_t dirIndex = (virt >> 22) & 0x3FF;   // top 10 bits indicate which PT to use within the PD

        if (dirIndex >= PT_MAX || page_tables_loc_phys[dirIndex] == 0) {
            if (out_status) *out_status = PAGING_ERR_NOT_MAPPED_OR_ALREADY_MAPPED;
            return 0;
        }

        uint32_t tableIndex = (virt >> 12) & 0x3FF;            // Page entry within that PT
        uint32_t* pt = (uint32_t*)page_tables_loc_phys[dirIndex];
        pt[tableIndex] = ((i << 12) & 0xFFFFF000) | PG_PRESENT | PG_RW; 
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

    kdbg_puts("Identity paging initialized with ", 0x0A);
    kdbg_hex32(identity_bytes, 0x0A);
    kdbg_puts(" bytes mapped. PD at ", 0x0A);
    kdbg_hex32(page_directory_loc_phys, 0x0A);
    kdbg_puts("\r\n", 0x0A);

    return page_directory_loc_phys;
}

volatile uint32_t* pd_ptr() {
    return (volatile uint32_t*)page_directory_loc_phys;
}

volatile uint32_t* pt_ptr_from_pdi(uint32_t pdi) {
    
    // TODO: may be able to recover PT phys from & 0xFFFFF000 of PDE,
    // but this is simpler/fine for now

    if (pdi > 0x3FF || page_tables_loc_phys[pdi] == 0)
        return NULL; // out of bounds or not mapped
    if ((pd_ptr()[pdi] & PG_PRESENT) == 0)
        return NULL; // not present in PD
    return (volatile uint32_t*)page_tables_loc_phys[pdi];
}

volatile uint32_t* pt_ptr_from_virt(uint32_t virt) {
    uint32_t pdi = (virt >> 22) & 0x3FF;
    return pt_ptr_from_pdi(pdi);
}

void paging_flush_tlb() {
    uint32_t ptbase;
    __asm__ __volatile__ (
        "movl %%cr3, %0\n"
        : "=r" (ptbase)
    );
    __asm__ __volatile__ (
        "movl %0, %%cr3\n"
        :
        : "r" (ptbase)
    );  
}

void paging_flush_tlb_single(uint32_t virt_addr) {
    #ifdef INVLPG_AVAILABLE
    __asm__ __volatile__ (
        "invlpg (%0)"
        :
        : "r" (virt_addr)
        : "memory"
    );
    #else
    (void)virt_addr; // silence unused parameter warning

    // True 80386 doesn't have INVLPG, so we have to flush the whole TLB
    // This is not optimal but the compile-time constant check lets us
    // avoid this on newer CPUs and can be enabled as required.
    paging_flush_tlb();
    #endif
}

bool is_paging_ready() {
    return page_directory_loc_phys != 0 && pd_ptr() != NULL;
}

paging_status_t paging_query_page(uint32_t virt_addr, paging_query_result_t* out_result) {
    if (page_directory_loc_phys == 0)
        return PAGING_ERR_NOT_INITIALIZED;

    uint32_t pdi = (virt_addr >> 22) & 0x3FF;
    uint32_t pti = (virt_addr >> 12) & 0x3FF;

    paging_query_result_t result = {0};

    uint32_t pd_entry = pd_ptr()[pdi];
    if ((pd_entry & PG_PRESENT) == 0) { // not present in PD
        if (out_result) *out_result = result;
        return PAGING_ERR_NOT_MAPPED;
    }

    if (page_tables_loc_phys[pdi] == 0) { // not mapped in PMM
        if (out_result) *out_result = result;
        return PAGING_ERR_NOT_MAPPED;
    }

    volatile uint32_t* pt = pt_ptr_from_pdi(pdi);
    if (!pt) { // PT null pointer, either not present in PD or out of bounds
        if (out_result) *out_result = result;
        return PAGING_ERR_NOT_MAPPED;
    }

    uint32_t pt_entry = pt[pti];
    if ((pt_entry & PG_PRESENT) == 0) { // not present in PT
        if (out_result) *out_result = result;
        return PAGING_ERR_NOT_MAPPED;
    }

    result.mapped = true;
    result.phys_addr = pt_entry & 0xFFFFF000; // physical frame address
    result.flags = pt_entry & 0xFFF; // flags from PTE

    if (out_result) *out_result = result;

    return PAGING_OK;
}

paging_status_t paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags, uint32_t* out_addr) {
    if (page_directory_loc_phys == 0)
        return PAGING_ERR_NOT_INITIALIZED;

    if ((virt_addr & 0xFFF) != 0 || (phys_addr & 0xFFF) != 0) {
        return PAGING_ERR_ALIGN;
    }

    uint32_t pdi = (virt_addr >> 22) & 0x3FF;
    uint32_t pti = (virt_addr >> 12) & 0x3FF;

    uint32_t* pd = (uint32_t*)page_directory_loc_phys;
    if ((pd[pdi] & PG_PRESENT) == 0) {
        // Need to allocate a new page table for this PDE
        if (page_table_count >= PT_MAX)
            return PAGING_ERR_NOMEM; // no more page tables available

        uint32_t new_pt_phys = 0;
        int res = pmm_reserve_pageframe(&new_pt_phys, true);
        if (res != PAGING_OK)
            return PAGING_ERR_NOMEM; // failed to reserve frame for new PT

        // Note: caller must pass PG_USER if user-accessible page is desired
        uint32_t user = (flags & PG_USER) ? PG_USER : 0;
        pd[pdi] = new_pt_phys | user | PG_PRESENT | PG_RW; // map PDE to new PT
        page_tables_loc_phys[pdi] = new_pt_phys;
        page_table_count++;
    }

    volatile uint32_t* pt = pt_ptr_from_pdi(pdi);
    if (!pt)
        return PAGING_ERR_FAILED_RETRIEVE_PT; // PT not present or out of bounds

    if ((pt[pti] & PG_PRESENT) != 0)
        return PAGING_ERR_ALREADY_MAPPED; // virtual address already mapped

    pt[pti] = (phys_addr & 0xFFFFF000) | (flags & 0xFFF) | PG_PRESENT;

    paging_flush_tlb_single(virt_addr);

    if (out_addr)
        *out_addr = phys_addr;

    return PAGING_OK;
}

paging_status_t paging_unmap_page(uint32_t virt_addr, uint32_t* out_phys_addr) {
    if (page_directory_loc_phys == 0) {
        if (out_phys_addr) *out_phys_addr = 0;
        return PAGING_ERR_NOT_INITIALIZED;
    }

    // Reject unmapping of non-page-aligned addresses, since that doesn't make 
    // sense in this paging scheme and may indicate a bug in the caller
    if (virt_addr & 0xFFF) {
        if (out_phys_addr) *out_phys_addr = 0;
        return PAGING_ERR_ALIGN;
    }

    // Find mapping for virtual address
    uint32_t pdi = (virt_addr >> 22) & 0x3FF;
    uint32_t pti = (virt_addr >> 12) & 0x3FF;

    // Ensure directory exists
    uint32_t* pd = (uint32_t*)page_directory_loc_phys;
    if (!pd) {
        if (out_phys_addr) *out_phys_addr = 0;
        return PAGING_ERR_NOT_INITIALIZED;
    }

    // Ensure directory entry is present
    if ((pd[pdi] & PG_PRESENT) == 0) {
        if (out_phys_addr) *out_phys_addr = 0;
        return PAGING_ERR_NOT_MAPPED;
    }

    // Attempt to retrieve page table reference from PDE
    volatile uint32_t* pt = pt_ptr_from_pdi(pdi);
    if (!pt) {
        if (out_phys_addr) *out_phys_addr = 0;
        return PAGING_ERR_FAILED_RETRIEVE_PT; // PT not present or out of bounds
    }

    // Ensure the page we believe is mapped to this virtual address
    // is actually present in PT
    if ((pt[pti] & PG_PRESENT) == 0) {
        if (out_phys_addr) *out_phys_addr = 0;
        return PAGING_ERR_NOT_MAPPED; // not present in PT
    }

    // If here, the page is present and mapped, and can be unmapped
    if (out_phys_addr)
        *out_phys_addr = pt[pti] & 0xFFFFF000; // return physical address of unmapped page

    pt[pti] = 0; // unmap page by clearing PTE

    paging_flush_tlb_single(virt_addr);

    // TODO: Somewhere around here, we could potentially free the 
    // PT frame if no more pages are mapped within it. This isn't
    // explicitly needed yet for bring-up on paging, but will be

    return PAGING_OK;
}

paging_status_t paging_set_flags(uint32_t virt_addr, uint32_t flags) {
    if (page_directory_loc_phys == 0)
        return PAGING_ERR_NOT_INITIALIZED;

    if (virt_addr & 0xFFF) {
        return PAGING_ERR_ALIGN;
    }

    uint32_t pdi = (virt_addr >> 22) & 0x3FF;
    uint32_t pti = (virt_addr >> 12) & 0x3FF;

    uint32_t* pd = (uint32_t*)page_directory_loc_phys;
    if ((pd[pdi] & PG_PRESENT) == 0)
        return PAGING_ERR_NOT_MAPPED; // not present in PD

    if (flags & PG_USER) {
        if ((pd[pdi] & PG_USER) == 0)
            return PAGING_ERR_PT_USER_FLAG_MISMATCH; // can't set user flag if PDE isn't user-accessible
    }

    volatile uint32_t* pt = pt_ptr_from_pdi(pdi);
    if (!pt)
        return PAGING_ERR_FAILED_RETRIEVE_PT; // PT not present or out of bounds

    if ((pt[pti] & PG_PRESENT) == 0)
        return PAGING_ERR_NOT_MAPPED; // not present in PT

    pt[pti] = (pt[pti] & 0xFFFFF000) | (flags & 0xFFF) | PG_PRESENT;

    paging_flush_tlb_single(virt_addr);

    return PAGING_OK;
}

void test_paging() {
    // Example test: reserve one free frame, map it to a virtual address,
    // query that virtual address, validate that it maps to the input frame address.
    // Then, set flags to read-only, and assert through querying that RW flag
    // is cleared. Finally, unmap page, and use query to assert that it is no
    // longer mapped. Finally, remap the same virtual address to a different phys
    // address and assert through querying that the new mapping is correct.

    kdbg_puts("Testing paging...\r\n", 0x0A);

    uint32_t physA = 0, physB = 0;

    kdbg_puts("Reserving page frame A... ", 0x0A);
    paging_status_t res = pmm_reserve_pageframe(&physA, true);
    if (res != PAGING_OK) {
        kdbg_puts("Failed to reserve page frame for testing paging\r\n", 0x0C);
        return;
    }

    kdbg_puts("0x", 0x0A); kdbg_hex32(physA, 0x0A); kdbg_puts("\r\n", 0x0A);

    const uint32_t test_virt = 0x40000000; // arbitrary virtual address for testing
    uint32_t addrOut = 0; 

    kdbg_puts("Mapping page frame A to test virtual address ", 0x0A);
    kdbg_hex32(test_virt, 0x0A);
    kdbg_puts("...\r\n", 0x0A);
    res = paging_map_page(test_virt, physA, PG_RW, &addrOut);

    if (res != PAGING_OK) {
        kdbg_puts("Failed to map page frame for testing paging (code ", 0x0C);
        kdbg_hex32(res, 0x0C);
        kdbg_puts(")\r\n", 0x0C);
        return;
    }

    kdbg_puts("done. Returned physical address: 0x", 0x0A);
    kdbg_hex32(addrOut, 0x0A);
    kdbg_puts("\r\n", 0x0A);

    if (physA != addrOut) {
        kdbg_puts("Mapped physical address does not match reserved frame for testing paging\r\n", 0x0C);
        return;
    }

    kdbg_puts("Remapping page with different flags...\r\n", 0x0A);


    // Set flags to read-only by using paging_set_flags with PG_RW cleared. This should update the flags in-place without changing the physical address mapping.
    res = paging_set_flags(test_virt, PG_PRESENT); // set flags to PG_PRESENT only
    if (res != PAGING_OK) {
        kdbg_puts("Failed to remap page frame for testing paging (code ", 0x0C);
        kdbg_hex32(res, 0x0C);  
        kdbg_puts(")\r\n", 0x0C);
        return;
    }

    kdbg_puts("Querying page after remap...\r\n", 0x0A);

    paging_query_result_t queryResult;
    res = paging_query_page(test_virt, &queryResult);
    if (res != PAGING_OK) {
        kdbg_puts("Failed to query page for testing paging (code ", 0x0C);
        kdbg_hex32(res, 0x0C);
        kdbg_puts(")\r\n", 0x0C);
        return;
    }

    if (queryResult.phys_addr != physA) {
        kdbg_puts("Queried physical address does not match reserved frame for testing paging after remap\r\n", 0x0C);
        return;
    }

    if (queryResult.flags & PG_RW) {
        kdbg_puts("Page remap with different flags did not update flags correctly for testing paging\r\n", 0x0C);
        return;
    }

    kdbg_puts("Unmapping page...\r\n", 0x0A);
    res = paging_unmap_page(test_virt, &addrOut);
    if (res != PAGING_OK) {
        kdbg_puts("Failed to unmap page for testing paging (code ", 0x0C);
        kdbg_hex32(res, 0x0C);
        kdbg_puts(")\r\n", 0x0C);
        return;
    }

    kdbg_puts("done. Unmapped physical address: 0x", 0x0A);
    kdbg_hex32(addrOut, 0x0A);
    kdbg_puts("\r\n", 0x0A);

    kdbg_puts("Confirming page is unmapped through query...\r\n", 0x0A);
    res = paging_query_page(test_virt, &queryResult);
    if (res != PAGING_ERR_NOT_MAPPED) {
        kdbg_puts("Querying unmapped page did not return PAGING_ERR_NOT_MAPPED for testing paging (code ", 0x0C);
        kdbg_hex32(res, 0x0C);
        kdbg_puts(")\r\n", 0x0C);
        return;
    }

    kdbg_puts("Page successfully unmapped.\r\n", 0x0A);

    kdbg_puts("Remapping same virtual address to different physical address...\r\n", 0x0A);

    kdbg_puts("Reserving page frame B... ", 0x0A);
    res = pmm_reserve_pageframe(&physB, true);
    if (res != PAGING_OK) {
        kdbg_puts("Failed to reserve page frame for testing paging\r\n", 0x0C);
        return;
    }

    kdbg_puts("0x", 0x0A); kdbg_hex32(physB, 0x0A); kdbg_puts("\r\n", 0x0A);

    kdbg_puts("Remapping test virtual address to page frame B... ", 0x0A);
    res = paging_map_page(test_virt, physB, PG_RW, &addrOut);
    if (res != PAGING_OK) {
        kdbg_puts("Failed to remap page frame to different physical address for testing paging (code ", 0x0C);
        kdbg_hex32(res, 0x0C);
        kdbg_puts(")\r\n", 0x0C);
        return;
    }

    kdbg_puts("0x", 0x0A); kdbg_hex32(addrOut, 0x0A); kdbg_puts("\r\n", 0x0A);

    kdbg_puts("done. Querying page to validate new mapping...\r\n", 0x0A);
    res = paging_query_page(test_virt, &queryResult);

    if (res != PAGING_OK) {
        kdbg_puts("Failed to query page for testing paging (code ", 0x0C);
        kdbg_hex32(res, 0x0C);
        kdbg_puts(")\r\n", 0x0C);
        return;
    }

    if (queryResult.phys_addr != physB) {
        kdbg_puts("Queried physical address does not match new physical address for testing paging\r\n", 0x0C);
        return;
    }

    kdbg_puts("Paging test completed successfully\r\n", 0x0A);
    return;
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
