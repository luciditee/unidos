#include "kglobal.h"
#include "kmain.h"
#include "isr.h"

static void on_int3(trap_frame_t* tf) {
    (void)tf;
    kdbg_puts("INT3 handled in C\r\n", 0x0A);
}

void kmain(uint32_t kparam_ptr, uint32_t kparam_length) {
    (void)kparam_ptr;
    (void)kparam_length;

    kdbg_puts("Entering kmain\r\n", 0x0F);

    isr_register(3, on_int3);   // breakpoint vector
    __asm__ __volatile__("int3");

    kdbg_puts("Returned from INT3\r\n", 0x0F);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
