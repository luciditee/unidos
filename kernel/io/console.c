
#include "io/console.h"
#include "panic.h"
#include "kmain.h"
#include "../include/errno.h"

/* Handler for stdin/stdout/stderr (bootstrapped for now -- 
   will need proper semantics later) */

#define CONSOLE_COUNT 1

// Temporary. In the future, multiple TTYs/consoles will be supported.
static size_t selected_console = 0;
static console_t consoles[CONSOLE_COUNT]; // for now, just one, but this'll ultimately be other TTY devices later

// Forward declaration of console ops
void console_op_get(kobject_t* obj);
void console_op_put(kobject_t* obj);
static ssize_t console_read(file_target_t* target, void* buffer, size_t length, uint32_t offset, void* context);
static ssize_t console_write(file_target_t* target, const void* buffer, size_t length, uint32_t offset, void* context);

void console_init(void) {
    // Initialize block device descriptors for output/input (stdout/stdin/stderr will eventually point here)
    for (size_t i =  0; i < CONSOLE_COUNT; i++) {
        consoles[i].target = fdpool_alloc_ftarget(KOBJECT_TYPE_STREAM);
        if (!consoles[i].target) {
            // fail hard if we can't allocate a file target for the console, since without it we can't do any I/O at all
            panic("failed to allocate file target for console", NULL);
        }

        consoles[i].target->kobj.ops = (kobject_ops_t){
            .get = console_op_get,
            .put = console_op_put
        };

        consoles[i].target->io_ops = (file_target_io_ops_t){
            .read = console_read,
            .write = console_write,
        };

        consoles[i].of = fdpool_alloc_openfile(consoles[i].target);

        if (!consoles[i].of) {
            // fail hard if we can't allocate an open file for the console, since without it we can't do any I/O at all
            panic("failed to allocate openfile for console", NULL);
        }
    }
}

open_file_t* console_get_stdio(void) {
    // TODO: assert valid selected_console
    return consoles[selected_console].of;
}

void console_op_get(kobject_t* obj) {
    (void)obj;
}

void console_op_put(kobject_t* obj) {
    (void)obj;
}

static ssize_t console_read(file_target_t* target, void* buffer, size_t length, uint32_t offset, void* context) {
    (void)target;
    (void)buffer;
    (void)length;
    (void)offset;
    (void)context;

    // Placeholder until keyboard/TTY buffering is implemented.
    return -EAGAIN;
}

static ssize_t console_write(file_target_t* target, const void* buffer, size_t length, uint32_t offset, void* context) {
    (void)target;
    (void)offset;
    (void)context;

    if (length == 0)
        return 0;

    if (!buffer)
        return -EFAULT;

    kdbg_putsn((const char*)buffer, 0x07, (uint32_t)length);
    return (ssize_t)length;
}
