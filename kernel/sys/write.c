
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>  // TODO: define one of our own
#include "kmem.h"
#include "isr.h"
#include "kmain.h"
#include "../include/errno.h"
#include "../include/unistd.h"
#include "../include/uaccess.h"
#include "../include/syscall.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define COPYIN_BUF_SIZE 128

static ssize_t __write_copyin(const char* str, size_t len, errno_t* err_out) {
    uint8_t copyin_buf[COPYIN_BUF_SIZE];
    size_t remaining = len, offset = 0;
    if (err_out) *err_out = ESUCCESS;

    while (remaining > 0) {
        size_t chunk = MIN(remaining, COPYIN_BUF_SIZE);
        errno_t err = copyin(str + offset, copyin_buf, chunk);
        if (err != ESUCCESS) {
            if (err_out) *err_out = err;
            return (ssize_t)offset; // partial write count
        }
        
        kdbg_putsn((char*)copyin_buf, 0x07, chunk);
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
    
    switch (fd) {
        case STDIN_FILENO: 
            return -EBADF;  // Bad file number because writing to input doesn't make sense
        case STDOUT_FILENO: // intentional fallthrough
        case STDERR_FILENO: { // TODO: separate handling
            // TODO: This is temporary. A proper file descriptor system plus a
            // tty driver has to be implemented for this to be correct, not
            // to mention a way to handle concurrency. For now, we just throw
            // the string to VGA
            errno_t err = ESUCCESS;
            ssize_t written = __write_copyin(str, len, &err);

            if (written > 0) return (errno_t)written;
            if (err != ESUCCESS) return err;
            return 0;
        }
        default: return -EBADF;  // Bad file number (for now);
    }
}
