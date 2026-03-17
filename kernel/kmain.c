#include "kglobal.h"
#include "kmain.h"
#include "isr.h"
#include "io.h"
#include "pit.h"

static void on_int3(trap_frame_t* tf) {
    (void)tf;
    kdbg_puts("INT3 handled in C\r\n", 0x0A);
    kdbg_dump_current();
}

void kmain(uint32_t kparam_ptr, uint32_t kparam_length) {
    (void)kparam_ptr;
    (void)kparam_length;
    kdbg_puts("Entering kmain\r\n", 0x0F);

    pic_remap();
    pit_init();
    //isr_register(3, on_int3);
    __asm__ __volatile__ ("sti");

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
