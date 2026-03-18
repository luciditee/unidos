
#pragma once

#include <stdint.h>
#include <stddef.h>

#define TASK_STACK_SIZE 4096
#define MAX_THREADS 128

#define TASK_IN_USE 1
#define TASK_UNUSED 0

typedef struct task {
    uint8_t in_use;
    uint32_t saved_esp;
    struct task* next;
    uint8_t stack[TASK_STACK_SIZE];
} task_t;

extern volatile uint8_t sched_pending;

task_t* sched_add_task(void (*entry)(void));
uint32_t sched_do_switch(uint32_t old_esp);
