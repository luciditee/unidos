
#pragma once
#include <stdint.h>
#include <stddef.h>

#define TASK_STACK_SIZE 4096
#define MAX_THREADS 128

#define TASK_IN_USE 1
#define TASK_UNUSED 0

typedef enum task_state {
    INVALID =   0x00,
    RUNNABLE = 0x01,
    BLOCKED  =  0x02,
    SLEEPING  = 0x04,
    DEAD =      0x08,
    ZOMBIE =    0x10
} task_state_t;

typedef struct task {
    uint8_t in_use;
    task_state_t state;
    uint32_t saved_esp;
    uint64_t wake_tick;
    int return_code;
    struct task* next;
    void (*entry)(void);
    uint8_t stack[TASK_STACK_SIZE];
} task_t;

extern volatile uint8_t sched_pending;

void sched_init(void);
task_t* sched_add_task(void (*entry)(void));
uint32_t sched_do_switch(uint32_t old_esp);
void sched_on_tick(void);
void sched_task_exit(int code);
void sched_task_sleep(uint64_t ticks);
void sched_task_yield(void);
