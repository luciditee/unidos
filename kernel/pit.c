#include "kmain.h"
#include "io.h"
#include "pit.h"
#include "isr.h"
#include "include/sched.h"

#define PIT_FREQ            1193182
#define PIT_CHANNEL0        0x40
#define PIT_CMD             0x43
#define PIT_DESIRED_TICK_HZ 100

static uint64_t ticks = 0;
static uint64_t next_report_tick = 10;

uint64_t get_ticks() {
    return ticks;
}

static void increment_ticks(trap_frame_t* tf) {
    (void)tf;
    ticks++;
    /*if (ticks == next_report_tick) {
        kdbg_puts("Tick: ", 0x0B);
        kdbg_hex32((uint32_t)(ticks >> 32), 0x0B);
        kdbg_hex32((uint32_t)(ticks & 0xFFFFFFFF), 0x0B);
        kdbg_puts("\r\n", 0x0B);
        next_report_tick += 10;
    }*/
    sched_pending = 1;
    pic_send_eoi(0);
}

void pit_init() {
    uint16_t divisor = (uint16_t)(PIT_FREQ / PIT_DESIRED_TICK_HZ);
    outb(PIT_CMD, 0x34); // channel 0, lobyte/hibyte, mode 2 (rate generator), binary
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));       // low byte
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF)); // high byte

    // install handler, unmask IRQ
    isr_register(32, increment_ticks);
    pic_unmask_irq(0);
}
