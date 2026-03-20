
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BOOTINFO_LINEAR_ADDR 0x00000500 // TODO: tie this to a build env macro so it aligns with assembly at all times
#define MAGIC_4CHAR "BTP1" // TODO: sync with stage2 assembly
#define BOOTINFO_SIZE_EXPECTED 289
#define SYNTHETIC_MEM_FALLBACK 8192u
#define E820_DESC_MAX 8

typedef struct __attribute__((packed)) {
    uint16_t entrySize;
    uint16_t entryCount;
    uint16_t totalLength;
    uint8_t data[24 * E820_DESC_MAX]; // TODO: union this with a struct representing what we expect to find here maybe
} bootinfo_e820_t;

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

typedef struct __attribute__((packed)) {
    // First 3 fields are used to validate presence of header
    char     signature[4];          // MAGIC_4CHAR
    uint8_t  versionMajor;
    uint8_t  versionMinor;
    uint16_t totalSize;
    uint16_t stage2Flags;
    uint32_t checksum32;
    uint8_t  bootDrive;
    uint8_t  mediaType;

    // media constants
    uint8_t  fsSectorsPerCluster;
    uint16_t fsReservedSectors;
    uint8_t  fsFatCount;
    uint16_t fsRootEntryCount;
    uint16_t fsTotalSectors16;
    uint8_t  fsMediaDescriptor;
    uint16_t fsSectorsPerFat;
    uint16_t fsSectorsPerTrack;
    uint16_t fsHeads;

    // BPB mirror block
    uint16_t bpbBytesPerSector;
    uint8_t  bpbSectorsPerCluster;
    uint8_t  bpbFatCount;
    uint16_t bpbReservedSectors;
    uint16_t bpbRootEntryCount;
    uint16_t bpbSectorsPerFat;
    uint16_t bpbSectorsPerTrack;
    uint16_t bpbHeads;
    uint32_t bpbTotalSectors;

    // derived LBAs from stage2 parsing
    uint32_t fatStartLba;
    uint32_t rootDirStartLba;
    uint32_t dataStartLba;

    // kernel load metadata
    uint16_t stage2EntryCS;
    uint32_t kernelDestAddrLinear;
    uint32_t kernelSizeBytes;
    uint32_t kparamsLinearAddr;
    uint32_t kparamsSizeBytes;

    // e801 and ah88 memory sizes (if present)
    uint32_t e801MemorySize;
    uint32_t ah88MemorySize;
    uint32_t cmosMemorySize;

    // e820 payload
    bootinfo_e820_t e820;
} bootinfo_t;

typedef enum {
    USE_CMOS = 0x00,
    USE_E820 = 0x01,
    USE_E801 = 0x02,
    USE_AH88 = 0x03
} bootinfo_memory_method_t;

#define BI_EXTRACT_MEMORY_METHOD(flags) (bootinfo_memory_method_t)((flags) & 0x0003)

_Static_assert(offsetof(bootinfo_t, e820) == 91, "bootinfo_t.e820 offset mismatch");
_Static_assert(sizeof(bootinfo_e820_t) == 198, "bootinfo_e820_t size mismatch");
_Static_assert(sizeof(bootinfo_t) == BOOTINFO_SIZE_EXPECTED, "bootinfo_t size mismatch");

extern volatile bool g_bootinfo_validated;
extern volatile uint32_t g_avail_memory_kib;
extern volatile bootinfo_memory_method_t g_memory_method;
extern volatile e820_desc_t g_e820_descs[E820_DESC_MAX];
extern volatile uint8_t g_e820_desc_count;
extern volatile size_t g_kernel_phys_address;
extern volatile size_t g_kernel_size_bytes;
extern volatile size_t g_kparams_phys_address;
extern volatile size_t g_kparams_size_bytes;

void bootinfo_init(void);
