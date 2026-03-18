#include "kglobal.h"
#include "kmain.h"
#include "isr.h"
#include "io.h"
#include "pit.h"
#include "kbm.h"
#include "include/sched.h"

static void on_int3(trap_frame_t* tf) {
    (void)tf;
    kdbg_puts("INT3 handled in C\r\n", 0x0A);
    kdbg_dump_current();
}

void test_task1() {
    while (1) {
        kdbg_puts("1", 0x0D);
        for (volatile int i = 0; i < 1000; i++);
    }
}

void test_task2() {
    while (1) {
        kdbg_puts("2", 0x0D);
        for (volatile int i = 0; i < 1000; i++);
    }
}

void kmain(uint32_t kparam_ptr, uint32_t kparam_length) {
    (void)kparam_ptr;
    (void)kparam_length;
    kdbg_puts("Entering kmain\r\n", 0x0F);

    pic_remap();
    pit_init();
    kb_init();
    isr_register(3, on_int3);
    __asm__ __volatile__ ("sti");

    sched_add_task(test_task1);
    sched_add_task(test_task2);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
