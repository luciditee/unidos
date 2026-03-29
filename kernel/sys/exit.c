#include "sys/types.h" // TODO: define one of our own
#include "../include/isr.h"
#include "../include/errno.h"
#include "../include/sched.h"
#include "../include/panic.h"

__attribute__((noreturn)) ssize_t _exit(trap_frame_t* tf) {
    task_kill_current("Task exited naturally", tf->ebx);

   panic("BUG: task_kill_current returned in _exit, which should never happen", tf);
   __builtin_unreachable();
}