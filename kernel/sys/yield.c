
#include "sys/types.h" // TODO: define one of our own
#include "../include/isr.h"
#include "../include/errno.h"
#include "../include/sched.h"

ssize_t _yield(trap_frame_t* tf) {
    sched_task_yield();
    return ESUCCESS;
}