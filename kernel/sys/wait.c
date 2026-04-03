#include "sys/types.h"
#include "../include/isr.h"
#include "../include/errno.h"
#include "../include/sched.h"
#include "../include/syscall.h"
#include "../include/uaccess.h"

// waitpid(2): minimal path for Milestone 7
// Supported now:
//  - waitpid(-1, &status, 0)
//  - waitpid(child_pid, &status, 0)
// Unsupported options return -EINVAL.
ssize_t _waitpid(trap_frame_t* tf) {
    int32_t pid = (int32_t)tf->ebx;
    int* user_status = (int*)tf->ecx;
    int options = (int)tf->edx;

    if (options != 0)
        return -EINVAL;

    if (user_status && !_ptr_safe(user_status, sizeof(int), tf))
        return -EFAULT;

    process_t* parent = sched_current_process();
    if (!parent)
        return -ESRCH;

    int status = 0;
    int32_t ret = proc_waitpid(parent, pid, &status);
    if (ret < 0)
        return ret;

    if (user_status) {
        errno_t err = copyout(&status, user_status, sizeof(int));
        if (err != ESUCCESS)
            return err;
    }

    return (ssize_t)ret;
}
