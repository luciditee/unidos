#pragma once
#include <stdint.h>
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

void isr_register(uint8_t vector, isr_handler_t fn);
void isr_dispatch(trap_frame_t* tf);