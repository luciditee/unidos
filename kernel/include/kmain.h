#pragma once
#include <stdint.h>

typedef struct trap_tail {
    uint32_t    vector, // which vector the ISR entered on
                error,  // error code
                eip,    // fault/interrupt return pointer
                cs,     // return CS
                eflags; // return EFLAGS
} __attribute__((packed)) trap_tail_t;

void kdbg_dump_current(void);
void kdbg_dump_frame(const trap_tail_t* tf);
void kdbg_puts(const char* s, uint32_t attr);
void kdbg_hex32(uint32_t value, uint32_t attr);
uint32_t kdbg_get_cursor_linear(void);

void kmain(uint32_t kparam_ptr, uint32_t kparam_length);
