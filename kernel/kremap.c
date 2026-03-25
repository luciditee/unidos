
#include "kremap.h"
#include "kmain.h"
#include "bootinfo.h"
#include "paging.h"
#include "kmem.h"

// After higher-half remap, stack exists at a higher virtual address (physically,
// at the top of available memory). To be loaded into ESP (in practice, ESP-4)
const uint32_t g_stack_virt_addr = STACK_VIRTUAL_BASE + (STACK_PAGE_SIZE << 12);

// Forward-declarations for functions defined elsewhere that don't merit being in
// header files due to their narrow use

// Reserves a single page frame in PMM
extern paging_status_t pmm_reserve_pageframe(uint32_t* out_phys_addr, bool clear);

// Reinitializes key values in PMM to reflect virtual memory mappings
extern void pmm_switch_to_phys_window_alias(void);

// Forces TLB flush; for use after major PD/PT changes
extern void paging_flush_tlb(void);

// Reinitialize IDT after remap, since stubs will be at different VAs
extern void idt_init(void);

// ASM-defined constants for where GDT, IDT, and ISR stubs are located
extern const uint8_t gdt_start;
extern const uint8_t idt_table;
extern const uint8_t isr_stub_table;

// Helper (in lieu of normal panic()) for failures specific to kernel high-half remap,
// which is such a critical step that failure is to be treated as unrecoverable
static void remap_panic_status(const char* msg, uint32_t code) {
    kdbg_puts(msg, 0x0C);
    kdbg_puts(" (code ", 0x0C);
    kdbg_hex32(code, 0x0C);
    kdbg_puts(")\r\n", 0x0C);
    HALT_FOREVER;
}

// Performs a higher-half remap to migrate the kernel to KERNEL_VIRTUAL_BASE,
// migrate the stack to STACK_VIRTUAL_BASE, and ultimately drop low memory aliases.
void kernel_highhalf_remap() {
    if (!is_paging_ready()) {
        kdbg_puts("Cannot perform kernel high-half remap: paging not ready\r\n", 0x0C);
        HALT_FOREVER;
        return;
    }

    // Invariants to maintain during remap:
    // - Keep kernel linked at KERNEL_VIRTUAL_BASE and ensure that mapping exists.
    // - Allocate/map a dedicated kernel stack at STACK_VIRTUAL_BASE.
    // - Leave a non-present guard page directly below the stack.
    //
    // The kernel image is already loaded physically by stage2; we ONLY need to
    // ensure virtual mappings are correct and stable

    uint32_t kernelPhysStart = ((uint32_t)&__kernel_start) & ~0xFFFu;
    uint32_t kernelPhysEnd = (((uint32_t)&__kernel_end) + 0xFFFu) & ~0xFFFu;
    uint32_t kernelPages = (kernelPhysEnd - kernelPhysStart) >> 12;

    kdbg_puts("Verifying high-half kernel mapping...\r\n", 0x0A);
    for (uint32_t i = 0; i < kernelPages; i++) {
        uint32_t virt = KERNEL_VIRTUAL_BASE + (i << 12);
        uint32_t expectedPhys = kernelPhysStart + (i << 12);

        paging_query_result_t q = {0};
        paging_status_t qres = paging_query_page(virt, &q);
        if (qres == PAGING_OK && q.mapped) {
            if (q.phys_addr != expectedPhys) {
                kdbg_puts("Kernel mapping mismatch at virt ", 0x0C);
                kdbg_hex32(virt, 0x0C);
                kdbg_puts(" expected phys ", 0x0C);
                kdbg_hex32(expectedPhys, 0x0C);
                kdbg_puts(" got ", 0x0C);
                kdbg_hex32(q.phys_addr, 0x0C);
                kdbg_puts("\r\n", 0x0C);
                HALT_FOREVER;
                return;
            }
            continue;
        }

        uint32_t outAddr = 0;
        paging_status_t mres = paging_map_page(virt, expectedPhys, PG_PRESENT | PG_RW, &outAddr);
        if (mres != PAGING_OK || outAddr != expectedPhys) {
            remap_panic_status("Failed to map missing kernel page", (uint32_t)mres);
            return;
        }
    }

    // TODO: kparams remapping (not using kparams yet)

    kdbg_puts("Allocating and mapping kernel stack pages...\r\n", 0x0A);

    // Reserve/map each stack page explicitly so we never rely on arithmetic
    // against memory-top values or assumptions about contiguous availability.
    for (uint32_t i = 0; i < STACK_PAGE_SIZE; i++) {
        uint32_t virt = STACK_VIRTUAL_BASE + (i << 12);

        paging_query_result_t q = {0};
        paging_status_t qres = paging_query_page(virt, &q);
        if (qres == PAGING_OK && q.mapped) {
            // If already mapped, leave it in place. This keeps remap idempotent
            // and avoids clobbering the active stack during iterative bring-up.
            continue;
        }

        uint32_t phys = 0;
        paging_status_t pres = pmm_reserve_pageframe(&phys, true);
        if (pres != PAGING_OK) {
            remap_panic_status("Failed to reserve physical frame for kernel stack", (uint32_t)pres);
            return;
        }

        uint32_t outAddr = 0;
        paging_status_t mres = paging_map_page(virt, phys, PG_PRESENT | PG_RW, &outAddr);
        if (mres != PAGING_OK || outAddr != phys) {
            remap_panic_status("Failed to map kernel stack page", (uint32_t)mres);
            return;
        }
    }

    // Guard page: page below stack base must be non-present so a
    // downward overflow can fault deterministically. #PF with address
    // in the guard page is a nearly guaranteed sign of stack overflow.
    uint32_t guardVirt = STACK_VIRTUAL_BASE - 4096;
    paging_query_result_t gq = {0};
    paging_status_t gqres = paging_query_page(guardVirt, &gq);
    if (gqres == PAGING_OK && gq.mapped) {
        uint32_t oldPhys = 0;
        paging_status_t ures = paging_unmap_page(guardVirt, &oldPhys);
        if (ures != PAGING_OK) {
            remap_panic_status("Failed to unmap kernel stack guard page", (uint32_t)ures);
            return;
        }
    }

    // Sanity-check that the top-of-stack address we'll load in assembly has a
    // present page directly beneath it (first push lands at ESP-4)
    paging_query_result_t topq = {0};
    paging_status_t topres = paging_query_page(g_stack_virt_addr - 4, &topq);
    if (topres != PAGING_OK || !topq.mapped) {
        remap_panic_status("Kernel stack top is not backed by a mapped page", (uint32_t)topres);
        return;
    }

    __asm__ __volatile__ ("cli"); // Disable interrupts, we're moving the IDT/GDT

    // Descriptor-table and ISR location sanity check:
    // Runtime descriptor/ISR symbols must be in higher-half virtual space
    // before we enforce low-VA teardown
    uint32_t gdtAddr = (uint32_t)&gdt_start;
    uint32_t idtAddr = (uint32_t)&idt_table;
    uint32_t isrAddr = (uint32_t)&isr_stub_table;

    if (gdtAddr < KERNEL_VIRTUAL_BASE || idtAddr < KERNEL_VIRTUAL_BASE || isrAddr < KERNEL_VIRTUAL_BASE) {
        kdbg_puts("Descriptor/ISR symbols are not in higher-half virtual space\r\n", 0x0C);
        kdbg_puts("gdt=", 0x0C); kdbg_hex32(gdtAddr, 0x0C);
        kdbg_puts(" idt=", 0x0C); kdbg_hex32(idtAddr, 0x0C);
        kdbg_puts(" isr=", 0x0C); kdbg_hex32(isrAddr, 0x0C);
        kdbg_puts("\r\n", 0x0C);
        HALT_FOREVER;
        return;
    }

    // Re-load IDT right before alias teardown, so interrupt gates are in a
    // known state and point to higher-half ISR stubs.
    //
    // NOTE: We intentionally do not call gdt_tss_init() here. It executes LTR,
    // and re-loading an already active/busy TSS descriptor can raise #GP(0x28).
    // GDT/TSS were already initialized during early kernel entry
    idt_init();

    // PMM/paging internals historically used low identity pointers for physical
    // memory operations. Switch those internals to the high physical window
    // before we drop low aliases
    pmm_switch_to_phys_window_alias();

    // Tear down low identity aliases from 1MiB and above.
    // This enforces ABI cleanliness for future user-space layouts and removes
    // all low-VA kernel aliases.
    kdbg_puts("Unmapping low identity aliases from 1MiB and above...\r\n", 0x0A);
    uint32_t lowUnmapStart = 0x00100000;
    uint32_t lowUnmapEnd = (((uint32_t)g_avail_memory_kib << 10) + 0xFFFu) & ~0xFFFu;
    for (uint32_t virt = lowUnmapStart; virt < lowUnmapEnd; virt += 4096) {
        uint32_t oldPhys = 0;
        paging_status_t ures = paging_unmap_page(virt, &oldPhys);
        if (ures == PAGING_ERR_NOT_MAPPED) {
            continue; // already absent, which is fine
        }
        if (ures != PAGING_OK) {
            remap_panic_status("Failed while unmapping low kernel identity alias", (uint32_t)ures);
            return;
        }
    }

    // Post-condition checks: low VA aliases at/above 1MiB should now be absent.
    // We probe a few representative pages instead of re-walking the full span.
    paging_query_result_t lowq = {0};
    if (paging_query_page(0x00100000, &lowq) == PAGING_OK && lowq.mapped) {
        remap_panic_status("Low VA 0x00100000 still mapped after teardown", PAGING_ERR_ALREADY_MAPPED);
        return;
    }
    if (paging_query_page(kernelPhysStart, &lowq) == PAGING_OK && lowq.mapped) {
        remap_panic_status("Low VA kernel base still mapped after teardown", PAGING_ERR_ALREADY_MAPPED);
        return;
    }
    if (lowUnmapEnd > lowUnmapStart) {
        uint32_t tailPage = lowUnmapEnd - 4096;
        if (paging_query_page(tailPage, &lowq) == PAGING_OK && lowq.mapped) {
            remap_panic_status("Low VA tail page still mapped after teardown", PAGING_ERR_ALREADY_MAPPED);
            return;
        }
    }

    // We intentionally keep identity mappings below 1MiB for now, so BIOS data,
    // BOOTINFO/KPARAMS low buffers, and VGA legacy ranges remain straightforward.

    // Force full TLB flush after tearing down identity aliases.
    paging_flush_tlb();

    // Later in the kernel init routine, we will re-enable interrupts--certain things
    // have to happen such as PIC remapping and PIT initialization, which are handled
    // elsewhere.

    kdbg_puts("Kernel high-half remap complete\r\n", 0x0A);
}