
#include "kmain.h"
#include "isr.h"

void panic(trap_frame_t* tf) {
    kdbg_puts("panic(): unrecoverable error occurred\r\n", 0x0C);
    kdbg_dump_current();

    kdbg_puts("\r\nCR2=", 0x0C); kdbg_hex32(tf->cr2, 0x0C);
    kdbg_puts("\r\nEIP=", 0x0C); kdbg_hex32(tf->eip, 0x0C);
    kdbg_puts("\r\n CS=", 0x0C); kdbg_hex32(tf->cs, 0x0C);

    kdbg_puts("Halting.\r\n", 0x0C);
    HALT_FOREVER;
}