
#pragma once

#include <stdint.h>
#include "./io/fdpool.h"

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__ ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__ ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__ ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__ ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__ ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__ ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t irq_save_disable(void) {
    uint32_t flags;
    __asm__ __volatile__(
        "pushf\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r"(flags)
        :
        : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags) {
    __asm__ __volatile__(
        "push %0\n\t"
        "popf\n\t"
        :
        : "r"(flags)
        : "memory", "cc");
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#define PIC1_OFFSET 0x20  // IRQ0..7 -> INT 0x20..0x27
#define PIC2_OFFSET 0x28  // IRQ8..15 -> INT 0x28..0x2F

void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);
void pic_remap();
void pic_send_eoi(uint8_t irq);

void unix_io_init();