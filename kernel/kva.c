
#include "kmem.h"
#include "kmain.h"
#include "paging.h"
#include "panic.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

#define KVA_REGION_MAX 16
#define KVA_NAME_MAX 16

typedef struct kva_region {
    char name[KVA_NAME_MAX];
    uint32_t base;
    uint32_t size;
    bool mapped;
} kva_region_t;

static kva_region_t kva_regions[KVA_REGION_MAX + 2] = {0}; // +2 for physwin and kernel stack - must initialize to 0
static size_t kva_region_count = 0;
static uint32_t kva_next_free = KVA_REGION_BASE;
static bool kva_initialized = false;

static void _kva_strncpy(char* dest, const char* src, size_t n) {
    for (size_t i = 0; i < MIN(n, KVA_NAME_MAX); i++) {
        dest[i] = src[i];
        if (src[i] == '\0') break;
    }
}

static int _kva_strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return s1[i] - s2[i];
        if (s1[i] == '\0') return 0;
    }
    return 0;
}

uint32_t kva_get_region_base(const char* name) {
    if (!name) panic("KVA region name cannot be null", NULL);

    for (size_t i = 0; i < KVA_REGION_MAX+2; i++) {
        if (kva_regions[i].base == 0) continue; // skip uninitialized regions
        /*kdbg_puts("(get base) Checking region ", 0x0C);
        kdbg_puts(kva_regions[i].name, 0x0C);
        kdbg_puts(" against ", 0x0C);
        kdbg_puts(name, 0x0C);
        kdbg_puts("\r\n", 0x0C);*/
        if (_kva_strncmp(kva_regions[i].name, name, sizeof(kva_regions[i].name)) == 0)
            return kva_regions[i].base;
    }

    /*kdbg_puts("name: ", 0x0C);
    kdbg_puts(name, 0x0C);
    kdbg_puts("\r\n", 0x0C);*/

    panic("KVA region not found", NULL);
    return 0; // unreachable
}

uint32_t kva_get_region_size(const char* name) {
    if (!name) panic("KVA region name cannot be null", NULL);

    for (size_t i = 0; i < KVA_REGION_MAX+2; i++) {
        /*kdbg_puts("Checking region ", 0x0C);
        kdbg_puts(kva_regions[i].name, 0x0C);
        kdbg_puts(" against ", 0x0C);
        kdbg_puts(name, 0x0C);
        kdbg_puts("\r\n", 0x0C);*/
        if (kva_regions[i].base == 0) continue; // skip uninitialized regions
        if (_kva_strncmp(kva_regions[i].name, name, sizeof(kva_regions[i].name)) == 0) {
            /*kdbg_puts("Found KVA region ", 0x0C);
            kdbg_puts(name, 0x0C);
            kdbg_puts(" with size 0x", 0x0C);
            kdbg_hex32(kva_regions[i].size, 0x0C);
            kdbg_puts("\r\n", 0x0C);*/
            return kva_regions[i].size;
        }
    }

    panic("KVA region not found", NULL);
    return 0; // unreachable
}

bool kva_region_exists(const char* name) {
    if (!name) return false;

    for (size_t i = 0; i < KVA_REGION_MAX+2; i++) {
        if (kva_regions[i].base == 0) continue; // skip uninitialized regions
        if (_kva_strncmp(kva_regions[i].name, name, sizeof(kva_regions[i].name)) == 0)
            return true;
    }

    return false;
}

void kva_register_region(const char* name, uint32_t size, uint32_t* out_base) {
    if ((KERNEL_VIRTUAL_BASE+(&__kernel_end - &__kernel_start)) > KVA_REGION_BASE) {
        panic("kernel image too large; overlaps KVA region", NULL);
    }

    /*kdbg_puts("Registering KVA region ", 0x0A);
    kdbg_puts(name, 0x0A);
    kdbg_puts(" with size 0x", 0x0A);
    kdbg_hex32(size, 0x0A);
    kdbg_puts("\r\n", 0x0A);*/

    // Note: This function completely assumes that PHYS_WINDOW_BASE sits somewhere above
    // KVA_REGION_BASE, and that KVA_REGION_BASE sits above KERNEL_VIRTUAL_BASE.
    // If those assumptions break, this function will destroy the memory map in fun ways.

    // Lazy init of physwin and kernel stack regions
    if (!kva_initialized) {
        //kdbg_puts("Initializing KVA manager\r\n", 0x0A);
        kva_initialized = true;

        // Last two regions are reserved for the physical window and kernel stack
        // The pages for these are mapped by the higher-half remap procedure, but we
        // likewise record the regions here for convenience.
        kva_region_t* physwin_region = &kva_regions[KVA_REGION_MAX-2];
        _kva_strncpy(physwin_region->name, REGION_PHYSWIN, sizeof(physwin_region->name));
        physwin_region->base = PHYS_WINDOW_BASE;
        physwin_region->size = 0x10000000; // 256MiB window by default -- true for now, but should TODO: be redefined to something nonmagic
        physwin_region->mapped = true;

        kva_region_t* stack_region = &kva_regions[KVA_REGION_MAX-1];
        _kva_strncpy(stack_region->name, REGION_KERNEL_STACK, sizeof(stack_region->name));
        stack_region->base = STACK_VIRTUAL_BASE;
        stack_region->size = STACK_PAGE_SIZE * 4096;
        stack_region->mapped = true;
    }

    if (kva_region_count >= KVA_REGION_MAX) {
        panic("Exceeded max KVA region count", NULL);
    }

    if (size == 0 || size % 4096 != 0)
        panic("KVA region size must be nonzero and page-aligned", NULL);
    
    // check for collisions with the ceiling, which is set at PHYS_WINDOW_BASE
    if (kva_next_free + size > PHYS_WINDOW_BASE) // if we overflow into physical region, this will corrupt memory
        panic("Exceeded KVA space limit", NULL);

    // ensure name is unique
    if (kva_region_exists(name))
        panic("KVA region with this name already exists", NULL);

    // Ensure the new region falls on the next page-aligned base
    kva_next_free = (kva_next_free + 0xFFF) & 0xFFFFF000;

    // Time to actually initialize the region
    /*kdbg_puts("Registering region ", 0x0A);
    kdbg_puts(name, 0x0A);
    kdbg_puts(" at base 0x", 0x0A);
    kdbg_hex32(kva_next_free, 0x0A);
    kdbg_puts(" with size 0x", 0x0A);
    kdbg_hex32(size, 0x0A);
    kdbg_puts("\r\n", 0x0A);*/
    kva_region_t* region = &kva_regions[kva_region_count];
    _kva_strncpy(region->name, name, sizeof(region->name));
    region->size = size;
    region->base = kva_next_free;
    region->mapped = false;
    
    // Fail hard if a subsystem tries to register a region that goes
    // over the phys window or underflows
    uint32_t next_free = kva_next_free + size;
    if (next_free < kva_next_free || (region->size + region->base) >= PHYS_WINDOW_BASE) // check for overflow
        panic("KVA region size out of bounds", NULL);
    
    kva_next_free = next_free;
    kva_region_count++;

    /*kdbg_puts("Region ", 0x0A);
    kdbg_puts(name, 0x0A);
    kdbg_puts(" registered successfully\r\n", 0x0A);*/

    if (out_base)
        *out_base = region->base;
}

bool kva_map_region(const char* name) {
    // This function is basically a no-op since we map the entire region at registration time, but
    // we provide it for symmetry with kva_unmap_region and in case we want to add lazy mapping in the future.
    if (!name) panic("KVA region name cannot be null", NULL);

    /*kdbg_puts("Mapping KVA region ", 0x0A);
    kdbg_puts(name, 0x0A);
    kdbg_puts("...\r\n", 0x0A);*/

    // Find the region by name
    // Note: Exposing the struct as an anonymous type in the header would
    // probably be cleaner/faster, but this is fine for now
    kva_region_t* region = NULL;
    for (size_t i = 0; i < KVA_REGION_MAX+2; i++) {
        /*kdbg_puts("(map) Checking region ", 0x0C);
        kdbg_puts(kva_regions[i].name, 0x0C);
        kdbg_puts(" against ", 0x0C);
        kdbg_puts(name, 0x0C);
        kdbg_puts("\r\n", 0x0C);*/
        if (kva_regions[i].base == 0) continue; // skip uninitialized regions
        if (_kva_strncmp(kva_regions[i].name, name, sizeof(kva_regions[i].name)) == 0) {
            region = &kva_regions[i];
            break;
        }
    }

    /*kdbg_puts("Found region ", 0x0A);
    kdbg_puts(name, 0x0A);
    kdbg_puts(" with base 0x", 0x0A);
    kdbg_hex32(region ? region->base : 0, 0x0A);
    kdbg_puts(" and size 0x", 0x0A);
    kdbg_hex32(region ? region->size : 0, 0x0A);
    kdbg_puts("\r\n", 0x0A);*/

    if (!region)
        panic("KVA region not found", NULL);

    if (region->mapped)
        panic("KVA region already mapped", NULL);

    // For each page in the region, we need to map it to a physical frame
    // This is where a block sequence allocator would come in handy, but
    // we do so individually in case we're dealing with something silly
    // like a hole-filled E820 memory map that makes contiguous phys alloc
    // impossible. If we fail to allocate/map a page, we stop mapping.
    size_t size = region->size;
    size_t page_count = size / 4096;
    size_t initialized = 0;
    uint32_t phys_addresses[page_count];
    uint32_t virt_addresses[page_count];
    for (size_t i = 0; i < page_count; i++, initialized++) {
        uint32_t virt_addr = region->base + (i * 4096);
        pmm_alloc_result_t phys_res = 0;        
        uint32_t phys_addr = pmm_get_next_available_block(&phys_res);
        if (phys_res != PMM_ALLOC_SUCCESS) {
            kdbg_puts("Failed to allocate physical block for KVA region (code ", 0x0C);
            kdbg_hex32(phys_res, 0x0C);
            kdbg_puts(")\r\n", 0x0C);
            break;
        }
        phys_addresses[i] = phys_addr;

        // Note: these are supervisor pages, so we do not pass | PG_USER
        paging_status_t res = paging_map_page(virt_addr, phys_addr, PG_PRESENT | PG_RW, NULL);
        if (res == PAGING_ERR_ALREADY_MAPPED) {
            kdbg_puts("Virtual address already mapped when mapping KVA region, which should be impossible since we manage the entire region. Address: 0x", 0x0C);
            kdbg_hex32(virt_addr, 0x0C);
            kdbg_puts("\r\n", 0x0C);
        }

        if (res != PAGING_OK) {
            kdbg_puts("Failed to map page for KVA region (code ", 0x0C);
            kdbg_hex32(res, 0x0C);
            kdbg_puts("), rewinding\r\n", 0x0C);

            // Rewind mapping of any physical addresses and pages
            for (size_t j = 0; j < initialized; j++) {
                // Skip the current page since if we're here we failed to map it,
                // but all the other pages can be unmapped
                if (i != j) paging_unmap_page(virt_addresses[j], NULL);
                pmm_dealloc_specific_block(phys_addresses[j]);
            }
            
            break;
        }
        
        virt_addresses[i] = virt_addr;

        //kdbg_puts("\r\n.", 0x0C);
    }

    /*kdbg_puts("Initialized ", 0x0A);
    kdbg_hex32(initialized, 0x0A);
    kdbg_puts(" / ", 0x0A);
    kdbg_hex32(page_count, 0x0A);
    kdbg_puts(" pages for region ", 0x0A);
    kdbg_puts(name, 0x0A);
    kdbg_puts("\r\n", 0x0A); */

    if (initialized != page_count)
        return false;

    // if here, we initialized everything successfully
    region->mapped = true;
    return true;
}
    