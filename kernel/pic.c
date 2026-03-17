
#include "io.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

#define ICW1_ICW4  0x01   // ICW4 will be sent
#define ICW1_INIT  0x10   // Initialization sequence
#define ICW4_8086  0x01   // 8086/88 mode

#define PIC1_OFFSET 0x20  // IRQ0..7 -> INT 0x20..0x27
#define PIC2_OFFSET 0x28  // IRQ8..15 -> INT 0x28..0x2F

#define CASCADE_IRQ 2

static uint16_t irq2port(uint8_t irq, uint8_t* bit) {
    if (irq < 8) {
        *bit = irq;
        return PIC1_DATA;
    }
    *bit = (uint8_t)(irq - 8);
    return PIC2_DATA;
}

void pic_mask_irq(uint8_t irq) {
    if (irq >= 16) return;
    uint8_t bit;
    uint16_t port = irq2port(irq, &bit);
    uint8_t value = (uint8_t)(inb(port) | (1u << bit));
    outb(port, value);
}

void pic_unmask_irq(uint8_t irq) {
    if (irq >= 16) return;
    uint8_t bit;
    uint16_t port = irq2port(irq, &bit);
    uint8_t value = (uint8_t)(inb(port) & ~(1u << bit));
    outb(port, value);
}

void pic_remap() {
    __asm__ __volatile__ ("cli");
    
    // Preserve current IRQ masks
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    // Start PIC init (cascade, ICW4 expected)
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    // ICW2: vector offsets
    outb(PIC1_DATA, PIC1_OFFSET); io_wait();
    outb(PIC2_DATA, PIC2_OFFSET); io_wait();

    // ICW3: wiring
    outb(PIC1_DATA, 1 << 2); io_wait(); // master has slave on IRQ2
    outb(PIC2_DATA, 2);      io_wait(); // slave cascade identity is 2

    // ICW4: 8086 mode
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // Restore masks
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

// Send end-of-interrupt signal for the given IRQ. This should be called
// at the end of an IRQ handler to allow the PIC to send more interrupts.
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}