
#include "../include/sys/types.h"
#include "../include/isr.h"
#include "../include/errno.h"
#include "../include/sched.h"

ssize_t _yield(trap_frame_t* tf) {
    (void)tf; // unused, suppress warning
    sched_thread_yield();
    return ESUCCESS;
}