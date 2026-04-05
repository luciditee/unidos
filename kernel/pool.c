
#include "pool.h"
#include "kmem.h"
#include "kmain.h"
#include "bootinfo.h"
#include "paging.h"
#include "include/sched.h"
#include "panic.h"

// Max number of pages to reserve per pool (one for threads, one for procs)
#define POOL_PAGES 256

// When determining how many pages to allocate to the thread pool, divide 
// the total number of pages to allocate to threads by this number
#define THREAD_POOL_DIV 1

// When determining how many pages to allocate to the process pool, divide 
// the total number of pages to allocate to processes by this number
#define PROCESS_POOL_DIV 2

// Threads need kernel-mode stacks for syscalls, exceptions, etc. so we set
// the number of pages to allocate for a kstack. This must always be >1 as
// the last page is used as a guard page to detect stack overflow conditions.
#define KSTACK_USABLE_PAGES 2
#define KSTACK_GUARD_PAGES 1
#define KSTACK_STRIDE_PAGES (KSTACK_USABLE_PAGES + KSTACK_GUARD_PAGES)

static uint32_t thread_pool_base = 0;
static size_t g_thread_pool_slot_count = 0;
static uint32_t process_pool_base = 0;
static size_t g_process_pool_slot_count = 0;
static uint32_t kstack_pool_base = 0;
static size_t g_kstack_pool_slot_count = 0;

//static uint8_t* kstack_page_bitmap;

void pool_get_kstack(size_t slot, uint32_t* bottom, uint32_t* top) {
    // Given a slot number, get the corresponding kstack virtual address range
    if (slot >= g_kstack_pool_slot_count)
        panic("Invalid kstack slot number", NULL);
    uint32_t kstack_virt = kstack_pool_base + (slot * KSTACK_STRIDE_PAGES * 4096);
    if (bottom) *bottom = kstack_virt + (KSTACK_GUARD_PAGES * 4096);
    if (top) *top = kstack_virt + (KSTACK_STRIDE_PAGES * 4096);
}

void pool_init(uint32_t* thread_base, size_t* thread_count, uint32_t* proc_base, size_t* proc_count) {
    // we scale these regions by powers of two above 8 (2^3), up to 2^N
    // (this range can be expanded in the future, but at the moment the kernel simply
    // does not work with >= 1GiB of memory)

    // This gives us a reasonable number of threads and processes to work with
    // The smallest power of two which fits our memory size becomes a multiplier
    // for how many page we ultimately allocate, bounded by 8MiB (no scalar) 
    // and 2^N (max scalar)
    uint32_t memory_mb = g_avail_memory_kib / 1024;
    uint32_t scalar = 1;
    static const int maxScalar = 4;
    while (memory_mb >> 1 != 0 && memory_mb > 16 && scalar < maxScalar) {
        memory_mb = memory_mb >> 1;
        scalar++;
    }

    // register regions for thread and process pooling
    uint32_t processpool_size = (POOL_PAGES / PROCESS_POOL_DIV) * 4096 * scalar;
    uint32_t kstack_pages = (POOL_PAGES / THREAD_POOL_DIV) * KSTACK_STRIDE_PAGES * scalar;
    uint32_t threadpool_size = (POOL_PAGES / THREAD_POOL_DIV) * 4096 * scalar;

    // If the thread pool size will end up having substantially more room for entries than
    // the kstack pool, we should truncate the thread pool size down to hold only as many threads
    // as there are kstacks
    if (threadpool_size / sizeof(thread_t) > kstack_pages / KSTACK_STRIDE_PAGES) {
        threadpool_size = (kstack_pages / KSTACK_STRIDE_PAGES) * sizeof(thread_t);
        threadpool_size = ((threadpool_size >> 12)) << 12; // guarantee page alignment   
    }

    kva_register_region(REGION_THREADPOOL, threadpool_size, &thread_pool_base);
    kva_register_region(REGION_PROCESSPOOL, processpool_size, &process_pool_base);
    
    kva_register_region(REGION_KSTACK, kstack_pages * 4096, &kstack_pool_base);

    // we also need a region for the kstack page bitmap, so the pool can track which
    // kstack may or may not be in use.
    
    g_thread_pool_slot_count = threadpool_size / sizeof(thread_t);
    g_process_pool_slot_count = processpool_size / sizeof(process_t);

    g_kstack_pool_slot_count = kstack_pages / KSTACK_STRIDE_PAGES;

    // Map regions, panic if any of them fail since this is essential for
    // process and thread management to function.
    if (!kva_map_region(REGION_THREADPOOL) || !kva_map_region(REGION_PROCESSPOOL)
        || !kva_map_region(REGION_KSTACK))
        panic("Failed to map KVA region for proc/thread pool", NULL);

    // Initialize kstack page bitmap
    // Note: kstack mapping is 1:1 with thread slots, so the bitmap doesn't
    // serve much purpose since the scheduler knows what slot a thread occupies
    // which preempts the need for a separate bitmap. Commented out for now

    /*kstack_page_bitmap = (uint8_t*)kstack_pool_base;
    size_t i;
    for (i = 0; i < (kstack_pages * 4096) / 8; i++)
        kstack_page_bitmap[i] = 0; // all kstack pages are initially free (0 in bitmap)*/
    
    // Init guard pages for kstacks. Only one guard page is needed per kstack
    // so we can set our stride length to be exactly KSTACK_PAGES
    for (size_t i = 0; i < kstack_pages; i += KSTACK_STRIDE_PAGES) {
        // set guard page to present but not RW, a
        uint32_t guard_virt = kstack_pool_base + (i * 4096);
        paging_status_t res = paging_set_flags(guard_virt, PG_PRESENT);
        if (res != PAGING_OK)
            panic("Failed to set flags for kstack guard page during pool initialization", NULL);   
    }

    // Set output pointers
    if (thread_base) *thread_base = thread_pool_base;
    if (thread_count) *thread_count = g_thread_pool_slot_count;
    if (proc_base) *proc_base = process_pool_base;
    if (proc_count) *proc_count = g_process_pool_slot_count;
    
    // note: we manage kstacks here in the pool, this is not the scheduler's problem
    // if (kstack_base) *kstack_base = kstack_pool_base;
    // if (kstack_count) *kstack_count = g_kstack_pool_slot_count;

    kdbg_puts("0x", 0x0F);
    kdbg_hex32(g_thread_pool_slot_count, 0x0F);
    kdbg_puts(" thread, 0x", 0x0F);
    kdbg_hex32(g_process_pool_slot_count, 0x0F);
    kdbg_puts(" process, 0x", 0x0F);
    kdbg_hex32(g_kstack_pool_slot_count, 0x0F);
    kdbg_puts(" kstacks available\r\n", 0x0F);

    uint32_t frames = (uint32_t)get_estimated_available_frames();
    kdbg_puts("available memory according to bitmap: ", 0x0F);
    kdbg_hex32(((frames * 4096)) / 1024 / 1024, 0x0F);
    kdbg_puts(" MiB\r\n", 0x0F);

    return;
}

_Static_assert(THREAD_POOL_DIV != 0, "THREAD_POOL_DIV must be nonzero");
_Static_assert(PROCESS_POOL_DIV != 0, "PROCESS_POOL_DIV must be nonzero");
_Static_assert(KSTACK_USABLE_PAGES > 0, "KSTACK_USABLE_PAGES must be > 0");
_Static_assert(KSTACK_GUARD_PAGES > 0, "KSTACK_GUARD_PAGES must be > 0");
