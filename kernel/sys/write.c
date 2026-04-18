
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kmem.h"
#include "isr.h"
#include "kmain.h"
#include "../include/errno.h"
#include "../include/unistd.h"
#include "../include/uaccess.h"
#include "../include/syscall.h"
#include "../include/sys/types.h"
#include "../include/sched.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define COPYIN_BUF_SIZE 128

static ssize_t __write_copyin(const char* str, size_t len, errno_t* err_out, open_file_t* of) {
    uint8_t copyin_buf[COPYIN_BUF_SIZE];
    size_t remaining = len, offset = 0;
    if (err_out) *err_out = ESUCCESS;

    while (remaining > 0) {
        // Determine how big of a chunk to copy in, then copy it in.
        size_t chunk = MIN(remaining, COPYIN_BUF_SIZE);
        errno_t err = ESUCCESS;
        copyin(str + offset, copyin_buf, chunk, &err);
        if (err != ESUCCESS) {
            if (err_out) *err_out = err;
            return (ssize_t)offset; // partial write count
        }
    
        // Write out to the active file descriptor
        // It's possible that open_file_write may truncate the write.
        // In the event of an error, we return the count of bytes that did get
        // successfully written out, but also pass-by-pointer the error code
        ssize_t write_res = open_file_write(of, copyin_buf, chunk, of->offset, NULL, &err);
        if (err != ESUCCESS) {
            if (err_out) *err_out = err;
            return (ssize_t)offset + ((write_res > 0) ? write_res : 0);
        }

        if (write_res != (ssize_t)chunk) {
            // Short write without a backend error is still a successful partial write.
            // NOTE: on i386, 64-bit offset_t updates are not atomic. If multiple
            // threads can share one open_file_t, this must be guarded by locking
            of->offset += (write_res > 0) ? (offset_t)write_res : 0;
            return (ssize_t)offset + ((write_res > 0) ? write_res : 0);
        }

        // If here, we successfully wrote the chunk (or the relevant part)
        // NOTE: same atomicity caveat as above for shared open_file_t updates.
        of->offset += (offset_t)chunk;
        offset += chunk;
        remaining -= chunk;
    }
    return (ssize_t)offset;
}

static inline bool _ptr_range_safe(const void* ptr, size_t len, trap_frame_t* tf) {
    if (len == 0) return 0;
    
    bool isUser = (tf->cs & 0x3) == 0x3; // note: syscall caller, not the current context

    /*kdbg_puts("isuser: ", 0x0E);
    kdbg_hex32(isUser, 0x0E);
    kdbg_puts("\r\n", 0x0E);*/

    // Check for overflow
    if ((uintptr_t)ptr + len < (uintptr_t)ptr) {
        //kdbg_puts("overflow", 0x0C);
        return false;
    }

    /*kdbg_puts("ptr: ", 0x0E);
    kdbg_hex32((uintptr_t)ptr, 0x0E);
    kdbg_puts("\r\n", 0x0E);
    kdbg_puts("len: ", 0x0E);
    kdbg_hex32(len, 0x0E);
    kdbg_puts("\r\n", 0x0E);*/

    // If this is the kernel, we assume we are writing somewhere we are supposed to.
    // If this is the user, the entire buffer must be in userspace. Writes to kernel
    // memory from userspace should only occur at the copyin/copyout layer, not write(2)
    return !isUser || ((uintptr_t)ptr < KERNEL_VIRTUAL_BASE 
        && ((uintptr_t)ptr + len) < KERNEL_VIRTUAL_BASE);
}

// write(2)
ssize_t _write(trap_frame_t* tf) {
    const char* str = (char*)tf->esi;
    unsigned int len = tf->edx;
    int fd = tf->ebx;

    /*kdbg_puts("TESTING: ", 0x0E);
    kdbg_hex32(len, 0x0E);
    kdbg_puts("\r\n", 0x0E);*/

    //return len; // temp

    // Null pointer on string is a nonstarter
    if (str == NULL)
        return -EFAULT;

    // Nothing to write, trivial success
    if (len == 0)
        return ESUCCESS;

    // Check that either the kernel called this, or the user did
    // AND the entire buffer is in user space. Also check for overflow
    if (!_ptr_range_safe(str, len, tf))
        return -EFAULT;

    // Get the process that called into this syscall
    process_t* proc = sched_current_process();
    if (!proc) return -EFAULT;
    
    // Get the file descriptor entry for the specified local fd
    open_file_t* out = sched_get_proc_local_fd(proc, fd);
    if (!out) return -EBADF;

    // With that FD isolated, pass it to the chunk writer in __write_copyin,
    // then return status/length back to caller
    errno_t err = ESUCCESS;
    ssize_t written = __write_copyin(str, len, &err, out);

    if (written > 0) return written;
    if (err != ESUCCESS) return -err;

    return 0;
}
