#pragma once
#include <stdint.h>
#include <stddef.h>

// TODO: move these to build environment so they can be shared
// between C and assembly without hardcoding
#define GDT_SEL_KCODE 0x08
#define GDT_SEL_KDATA 0x10

typedef struct trap_tail {
    uint32_t    vector, // which vector the ISR entered on
                error,  // error code
                eip,    // fault/interrupt return pointer
                cs,     // return CS
                eflags; // return EFLAGS
} __attribute__((packed)) trap_tail_t;

static inline void kmemcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

void kdbg_dump_current(void);
void kdbg_dump_frame(const trap_tail_t* tf);
void kdbg_puts(const char* s, uint32_t attr);
void kdbg_hex32(uint32_t value, uint32_t attr);
uint32_t kdbg_get_cursor_linear(void);

void kmain(uint32_t kparam_ptr, uint32_t kparam_length);
