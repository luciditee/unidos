
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "isr.h"
#include "kmem.h"

// TODO: Introduce as build system constant to use in prekinit_idt.asm as well
#define SYSCALL_VECTOR 0x80

typedef enum syscallno {
    SYSCALL_WRITE = 0x0001,
    SYSCALL_EXIT = 0x0002,
    SYSCALL_SCHED_YIELD = 0x0018,
} syscallno_t;


static inline bool _is_user(trap_frame_t* tf) { 
    return (tf->cs & 0x3) == CPL_USER;     
}

static inline bool _ptr_safe(const void* ptr, size_t len, trap_frame_t* tf) {
    if (!ptr) return false; // null pointer check
    if (!len) return true;  // zero-length buffer trivially safe
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + len - 1;
    if (end < start) return false; // overflow check

    // kernel can do whatever it wants, but user calling contexts must only
    // interact with user-space addresses
    return !_is_user(tf) || (is_user_vaddr(start) && is_user_vaddr(end));
}

void syscall_init();
void syscall_test(void);
