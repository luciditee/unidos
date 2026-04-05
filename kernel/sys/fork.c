#include "sys/types.h"
#include "../include/isr.h"
#include "../include/errno.h"
#include "../include/sched.h"
#include "../include/syscall.h"
#include "../include/uaccess.h"

ssize_t _fork(trap_frame_t* tf) {
    process_t* parent = sched_current_process();
    
    if (!parent)
        return -ESRCH;

    // allocate child process with same image name and context identifier
    process_t* child = proc_alloc(parent, parent->context, parent->comm);
    if (!child)
        return -EAGAIN;
    
    // clone, or fail with EAGAIN if clone does not succeed.
    if (!sched_clone_fork(parent, child, tf)) {
        proc_free(child);
        return -EAGAIN;
    }

    return child->pid;
}