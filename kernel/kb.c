
#include "kmain.h"
#include "io.h"
#include "isr.h"

void kb_handler(trap_frame_t* tf) {
    (void)tf;
    // Scancode must be read to clear buffer, otherwise we'll only ever
    // print one "K" for test case.
    uint8_t scancode = inb(0x60);
    if (scancode > 80) {
        pic_send_eoi(1); // IRQ1
        return;
    }
    kdbg_puts("K", 0x0E);
    
    pic_send_eoi(1); // IRQ1
}

void kb_init() {
    kdbg_puts("init keyboard\r\n", 0x0E);
    isr_register(PIC1_OFFSET + 1, kb_handler);
    pic_unmask_irq(1);
}
