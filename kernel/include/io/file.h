
#pragma once

#include "kobject.h"
#include "sys/types.h"
#include "../errno.h"

#define PIPE_BUFFER_SIZE 4096

typedef struct file_target file_target_t;
typedef struct open_file open_file_t;

typedef ssize_t (*file_target_read_fn)(file_target_t* target, void* buffer, size_t length, uint32_t offset, void* context);
typedef ssize_t (*file_target_write_fn)(file_target_t* target, const void* buffer, size_t length, uint32_t offset, void* context);

typedef struct file_target_io_ops {
    file_target_read_fn read;
    file_target_write_fn write;
} file_target_io_ops_t;

typedef struct vnode {
    uint32_t size;
} vnode_t;

typedef struct kblkdev {
    uint32_t block_size;
} kblkdev_t;

// note: metadata only; actual buffers are allocated elsewhere and referenced by pointer
typedef struct kpipe {
    uint32_t size;
    uint8_t* buffer;
    size_t head;
    size_t tail;
} kpipe_t;

// note: as with pipes, this is metadata only
typedef struct kstream {
    uint32_t size;
    uint8_t* buffer;
} kstream_t;

// Union of all possible file target types, for easy allocation and reference in the file target pool
typedef union ftdata {
    vnode_t vnode;
    kblkdev_t blkdev;
    kpipe_t pipe;
    kstream_t stream;
} ftdata_t;

// Underlying object endpoint for open files, vnodes, block devices, pipes, streams, etc.
typedef struct file_target {
    kobject_t kobj; // type/refcount for this endpoint object
    file_target_io_ops_t io_ops;
    ftdata_t data;
    uint32_t pool_id;
} file_target_t;

// Systemwide instance representation of an open file
typedef struct open_file {
    kobject_t kobj; // refcount, flags, typeinfo
    uint32_t offset; // r/w position tracking
    file_target_t* target; // the actual thing being referenced by the open file
    uint32_t pool_id; // for bookkeeping which pool slot this open file occupies; used for debugging and potential future deallocation
} open_file_t;

// Per-process file descriptor entry--references an open file by 
// numeric ID
typedef struct fd_entry {
    uint32_t fd_id; // guaranteed unique ID across whole system
                    // TODO: maybe typedef this
    open_file_t* of;

    // File descriptors are stored as a linked list when referenced
    // by processes so as to not force us to allocate a static array
    // of file descriptors. This is unlikely to get iterated often
    // as our eventual FILE type will be a pointer directly to the
    // descriptor target--but also, because the target minspec for
    // this kernel is only 8MB of RAM, if we use an array of FD_SET_MAX
    // (1024) file descriptors per process, that's a whole page per process
    // devoted to *just* file descriptors, when most processes will
    // likely just have a handful open at a time. We can still enforce
    // FD_SET_MAX limits, we just won't statically allocate an array
    // per process.
    struct fd_entry* next;
    uint16_t local_id;  // for use by process, NOT unique across whole system,
                        // used in the event we iterate the linked list above
                        // to find the nth file descriptor.
} fd_entry_t;

void file_target_get(file_target_t* target);
void file_target_put(file_target_t* target);

void open_file_get(open_file_t* of);
void open_file_put(open_file_t* of);

ssize_t open_file_read(open_file_t* of, void* buffer, size_t length, uint32_t offset, void* context);
ssize_t open_file_write(open_file_t* of, const void* buffer, size_t length, uint32_t offset, void* context, errno_t* err_out);

