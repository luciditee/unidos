#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kmain.h"

typedef struct trap_frame {
    // pushad block (top of stack after pushad)
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy;   // original ESP captured by pushad
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;

    // pushed by isr_common_entry (in this order)
    uint32_t ds;
    uint32_t es;
    uint32_t fs;
    uint32_t gs;
    uint32_t cr2;

    // normalized interrupt tail from stubs/CPU
    uint32_t vector;
    uint32_t error;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
} __attribute__((packed)) trap_frame_t;

typedef void (*isr_handler_t)(trap_frame_t* tf);

static inline bool tf_has_user_tail(const trap_frame_t* tf) {
    // if CS has usermode RPL, then trap frame originates from a usermode entry
    return (tf->cs & 0x3) == CPL_USER;
}

static inline uint32_t* tf_user_esp_slot(trap_frame_t* tf) {
    // only valid if tf_has_user_tail(tf) is true; otherwise, no user ESP was saved
    uintptr_t base = (uintptr_t)tf;
    uintptr_t off = offsetof(trap_frame_t, eflags) + sizeof(uint32_t);
    return (uint32_t*)(base + off); // [eflags][user_esp][user_ss]
}

static inline uint32_t* tf_user_ss_slot(trap_frame_t* tf) {
    // only valid if tf_has_user_tail(tf) is true; otherwise, no user SS was saved
    return tf_user_esp_slot(tf) + 1;
}

void isr_register(uint8_t vector, isr_handler_t fn);
void isr_dispatch(trap_frame_t* tf);