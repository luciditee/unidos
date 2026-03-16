#include "isr.h"
#include "kmain.h"

static isr_handler_t g_isr_handlers[256];

static void halt_forever(void) {
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

void isr_register(uint8_t vector, isr_handler_t fn) {
    g_isr_handlers[vector] = fn;
}

void isr_dispatch(trap_frame_t* tf) {
    isr_handler_t fn = g_isr_handlers[tf->vector];
    if (fn) {
        fn(tf);   // handler may return
        return;
    }

    // Default: dump normalized tail and halt
    kdbg_puts("\r\n[unhandled interrupt]\r\n", 0x0C);
    kdbg_dump_frame((const trap_tail_t*)&tf->vector);
    halt_forever();
}