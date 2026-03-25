#include "kglobal.h"
#include "kmain.h"
#include "isr.h"
#include "io.h"
#include "pit.h"
#include "kbm.h"
#include "bootinfo.h"
#include "include/sched.h"
#include "kmem.h"
#include "kremap.h"

static void on_int3(trap_frame_t* tf) {
    (void)tf;
    kdbg_puts("INT3 handled in C\r\n", 0x0A);
    kdbg_dump_current();
}

// Prints a 1 on the screen, then burns cycles.
void test_task1() {
    while (1) {
        kdbg_puts("1", 0x0D);
        for (volatile int i = 0; i < 1000; i++);
    }
}

// Identical to above, but prints 2
void test_task2() {
    while (1) {
        kdbg_puts("2", 0x0D);
        for (volatile int i = 0; i < 1000; i++);
    }
}

// Exits after printing a few times, to test task cleanup.
void test_task3() {
    for (volatile int i = 0; i < 5; i++){
        kdbg_puts("3", 0x0D);
        for (volatile int j = 0; j < 1000; j++);
    }
}

// Exits immediately after printing a 4 to test exit code handling.
void test_task4() {
    kdbg_puts("4", 0x0D);
    sched_task_exit(42);
}

// Prints a 5, sleeps for a while, then prints another 5 to test sleeping.
void test_task5() {
    for (;;) {
        kdbg_puts("5", 0x0C);
        sched_task_sleep(100); // sleep for 100 ticks
    }
}

static void trigger_intentional_page_fault(void) {
    // map up to nearest 4MiB boundary
    uint32_t mapped_end = (g_avail_memory_kib << 10);
    mapped_end = (mapped_end + 0x3FFFFF) & ~0x3FFFFF;

    // First page beyond mapped identity window.
    volatile uint32_t* p = (volatile uint32_t*)(mapped_end + 0x1000);

    kdbg_puts("Triggering intentional #PF...\r\n", 0x0E);
    *p = 0xDEADBEEF; // should fault (W=1, P=0 expected)
}

extern void test_paging();

void ktrampoline() {
    bootinfo_init();
    mem_init();
    kernel_highhalf_remap();
    return;
}

void kmain() {
    kdbg_puts("Entered kmain\r\n", 0x0F);

    pic_remap();
    pit_init();    
    test_paging();
    kb_init();
    sched_init();
    //isr_register(3, on_int3);
    __asm__ __volatile__ ("sti");

    //trigger_intentional_page_fault();

    sched_add_task(test_task1);
    sched_add_task(test_task2);
    //sched_add_task(test_task2); // test multiple instances
    /*sched_add_task(test_task3);
    sched_add_task(test_task4);
    sched_add_task(test_task5);*/

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
