
#include "bootinfo.h"
#include "kmain.h"
#include <stdbool.h>



static bootinfo_t* bi = (bootinfo_t*)BOOTINFO_LINEAR_ADDR;

volatile uint32_t g_avail_memory_kib = 0;
volatile bootinfo_memory_method_t g_memory_method = 0;
volatile e820_desc_t g_e820_descs[E820_DESC_MAX] = {0};
volatile uint8_t g_e820_desc_count = 0;
volatile bool g_bootinfo_validated = false;
volatile size_t g_kernel_phys_address = 0;
volatile size_t g_kernel_size_bytes = 0;
volatile size_t g_kparams_phys_address = 0;
volatile size_t g_kparams_size_bytes = 0;

static bool decode_e820(const bootinfo_e820_t* e, e820_stats_t* out, volatile e820_desc_t* descOut, uint8_t descOutCapacity, uint8_t* descOutCount) {
    if (!e || !out || !descOutCount) return false;
    if (e->entrySize < 20) return false;
    if (e->entryCount == 0) return false;
    if (e->totalLength > sizeof(e->data)) return false;

    uint32_t tableNeeded = (uint32_t)e->entryCount * (uint32_t)e->entrySize;
    if (tableNeeded > e->totalLength) return false;

    uint64_t usableBytes = 0;
    uint64_t topBytes = 0;
    uint8_t descsFilled = 0;

    const uint8_t* p = e->data;
    for (uint16_t i = 0; i < e->entryCount; i++) {
        e820_desc_t d = {0};
        size_t copyBytes = (e->entrySize < sizeof(e820_desc_t)) ? e->entrySize : sizeof(e820_desc_t);
        kmemcpy(&d, p, copyBytes);

        if (d.length != 0 && d.type == 1) {
            uint64_t end = d.base + d.length;
            if (end < d.base) end = UINT64_MAX; // overflow clamp

            usableBytes += d.length;
            if (end > topBytes) topBytes = end;
        }

        if (descOut && descsFilled < descOutCapacity) {
            descOut[descsFilled].base = d.base;
            descOut[descsFilled].length = d.length;
            descOut[descsFilled].type = d.type;
            descOut[descsFilled].attrs = d.attrs;
            descsFilled++;
        }
        p += e->entrySize;
    }

    *descOutCount = descsFilled;
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

    g_bootinfo_validated = true;
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
            g_memory_method = USE_CMOS;
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
            e820_stats_t e820_stats = {0};
            uint8_t decoded_desc_count = 0;
            if (!decode_e820(&bi->e820, &e820_stats, g_e820_descs, E820_DESC_MAX, &decoded_desc_count)) {
                kdbg_puts("Invalid E820 data\r\n", 0x0C);
                HALT_FOREVER;
            }
            g_e820_desc_count = decoded_desc_count;
            g_avail_memory_kib = e820_stats.topKiB;
            g_memory_method = USE_E820;
            break;
        case USE_E801:
            kdbg_puts("Using E801 memory map\r\n", 0x0B);
            g_avail_memory_kib = bi->e801MemorySize;
            g_memory_method = USE_E801;
            break;
        case USE_AH88:
            kdbg_puts("Using AH88 memory map\r\n", 0x0B);
            g_avail_memory_kib = bi->ah88MemorySize;
            g_memory_method = USE_AH88;
            break;
        default:
            // Should never happen, but kept here for sanity
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
