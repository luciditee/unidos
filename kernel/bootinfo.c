
#include "bootinfo.h"
#include "kmain.h"
#include <stdbool.h>

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;      // 1 = usable
    uint32_t attrs;     // present if entrySize >= 24
} e820_desc_t;

typedef struct {
    uint32_t usableKiB;
    uint32_t topKiB;
} e820_stats_t;

static bootinfo_t* bi = (bootinfo_t*)BOOTINFO_LINEAR_ADDR;

volatile uint32_t g_avail_memory_kib = 0;
volatile bool g_bootinfo_validated = false;
volatile size_t g_kernel_phys_address = 0;
volatile size_t g_kernel_size_bytes = 0;
volatile size_t g_kparams_phys_address = 0;
volatile size_t g_kparams_size_bytes = 0;

static bool decode_e820(const bootinfo_e820_t* e, e820_stats_t* out) {
    if (!e || !out) return false;
    if (e->entrySize < 20) return false;
    if (e->entryCount == 0) return false;
    if (e->totalLength > sizeof(e->data)) return false;

    uint32_t tableNeeded = (uint32_t)e->entryCount * (uint32_t)e->entrySize;
    if (tableNeeded > e->totalLength) return false;

    uint64_t usableBytes = 0;
    uint64_t topBytes = 0;

    const uint8_t* p = e->data;
    for (uint16_t i = 0; i < e->entryCount; i++) {
        const e820_desc_t* d = (const e820_desc_t*)p;

        if (d->length != 0 && d->type == 1) {
            uint64_t end = d->base + d->length;
            if (end < d->base) end = UINT64_MAX; // overflow clamp

            usableBytes += d->length;
            if (end > topBytes) topBytes = end;
        }

        p += e->entrySize;
    }

    out->usableKiB = (uint32_t)(usableBytes >> 10);
    out->topKiB    = (uint32_t)(topBytes >> 10);
    return true;
}

void bootinfo_validate_prefix(void);
void bootinfo_getmem(void);
void bootinfo_getkmetadata(void);

void bootinfo_init(void) {
    bootinfo_validate_prefix();
    bootinfo_getmem();
    bootinfo_getkmetadata();

    kdbg_hex32(g_kernel_phys_address, 0x0A);
    kdbg_puts("\r\n", 0x0A);
    kdbg_hex32(g_kernel_size_bytes, 0x0A);
    kdbg_puts("\r\n", 0x0A);
    kdbg_hex32(g_kparams_phys_address, 0x0A);
    kdbg_puts("\r\n", 0x0A);
    kdbg_hex32(g_kparams_size_bytes, 0x0A);
    kdbg_puts("\r\n", 0x0A);

    //g_bootinfo_validated = true;
}

void bootinfo_validate_prefix(void) {
    if (kmemcmp(bi->signature, MAGIC_4CHAR, 4) != 0) {
        kdbg_puts("Invalid bootinfo signature\r\n", 0x0C);
        HALT_FOREVER;
    }

    if (bi->totalSize != BOOTINFO_SIZE_EXPECTED) {
        kdbg_puts("bootinfo size mismatch\r\n", 0x0C);
        HALT_FOREVER;
    }
}

void bootinfo_getmem(void) {
    switch (BI_EXTRACT_MEMORY_METHOD(bi->stage2Flags)) {
        case USE_CMOS:
            kdbg_puts("Using CMOS memory map\r\n", 0x0B);
            g_avail_memory_kib = bi->cmosMemorySize;
            if (g_avail_memory_kib <= 0x400) {
                kdbg_puts("CMOS reports strangely low value (got ", 0x0C);
                kdbg_hex32(g_avail_memory_kib, 0x0C);
                kdbg_puts(" KiB), using fallback value of ", 0x0C);
                kdbg_hex32(SYNTHETIC_MEM_FALLBACK, 0x0C);
                kdbg_puts(" KiB\r\n", 0x0C);
                g_avail_memory_kib = SYNTHETIC_MEM_FALLBACK;
                break;
            }

            break;
        case USE_E820:
            kdbg_puts("Using E820 memory map\r\n", 0x0B);
            e820_stats_t e;
            if (!decode_e820(&bi->e820, &e)) {
                kdbg_puts("Invalid E820 data\r\n", 0x0C);
                HALT_FOREVER;
            }
            g_avail_memory_kib = e.topKiB;
            break;
        case USE_E801:
            kdbg_puts("Using E801 memory map\r\n", 0x0B);
            g_avail_memory_kib = bi->e801MemorySize;
            break;
        case USE_AH88:
            kdbg_puts("Using AH88 memory map\r\n", 0x0B);
            g_avail_memory_kib = bi->ah88MemorySize;
            break;
        default:
            kdbg_puts("Unknown memory map method\r\n", 0x0C);
            HALT_FOREVER;
    }
}

void bootinfo_getkmetadata(void) {
    g_kernel_phys_address = bi->kernelDestAddrLinear;
    g_kernel_size_bytes = bi->kernelSizeBytes;
    g_kparams_phys_address = bi->kparamsLinearAddr;
    g_kparams_size_bytes = bi->kparamsSizeBytes;
}
