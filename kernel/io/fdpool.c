#include "io.h"
#include "kmem.h"
#include "dpage.h"
#include "io/fdpool.h"

/*  File descriptor pool management
    This file handles demand paging of the file descriptor pools used
    both globally and by processes.
*/

extern void console_init(void);

// Open file pool describes global systemwide open files
static uint8_t* openfile_bitmap = (uint8_t*)OPENFILE_BITMAP_BASE;
static const uint32_t openfile_vaddr_size_bytes = (FD_BITMAP_BASE - OPENFILE_VIRTUAL_BASE) & ~0xFFFu;
static const uint32_t openfile_bitmap_size_bytes = OPENFILE_BITMAP_PAGES * 0x1000; // 1 bit per slot

// File descriptor pool describes per-process file descriptor entries
static uint8_t* fd_bitmap = (uint8_t*)FD_BITMAP_BASE;
static const uint32_t fd_vaddr_size_bytes = (FTARGET_BITMAP_BASE - FD_VIRTUAL_BASE) & ~0xFFFu; 
static const uint32_t fd_bitmap_size_bytes = FD_BITMAP_PAGES * 0x1000; // 1 bit per slot

// Mixed pool which holds actual vnode, blockdev, file, and pipe metadata
// Note: Actual pipe/IO buffers are NOT stored in this pool; but pointers to them
// are. This pool is just for the metadata structs
static uint8_t* ftarget_bitmap = (uint8_t*)FTARGET_BITMAP_BASE;
static const uint32_t ftarget_vaddr_size_bytes = (FTARGET_END - FTARGET_VIRTUAL_BASE) & ~0xFFFu;
static const uint32_t ftarget_bitmap_size_bytes = FTARGET_BITMAP_PAGES * 0x1000; // 1 bit per slot

void fdpool_init(void) {
    // Initialize file descriptor management structures
    // This includes setting up bitmaps and virtual memory mappings
    if (!dpage_init(OPENFILE_BITMAP_BASE, OPENFILE_VIRTUAL_BASE, openfile_bitmap))
        panic("failed to initialize open file demand-paged pool", NULL);
    if (!dpage_init(FD_BITMAP_BASE, FD_VIRTUAL_BASE, fd_bitmap))
        panic("failed to initialize fd demand-paged pool", NULL);    
    if (!dpage_init(FTARGET_BITMAP_BASE, FTARGET_VIRTUAL_BASE, ftarget_bitmap))
        panic("failed to initialize file target demand-paged pool", NULL);
}

void unix_io_init() {
    fdpool_init();
    console_init();
}

// Searches for a free spot in the openfile pool, marks it as allocated, populates
// some basic fields, and returns a pointer to the allocated open_file_t.
open_file_t* fdpool_alloc_openfile(file_target_t* target) {
    dpage_alloc_status_t out_status;
    open_file_t* ret = (open_file_t*)dpage_alloc(openfile_bitmap, 
        OPENFILE_VIRTUAL_BASE, openfile_vaddr_size_bytes,
        sizeof(open_file_t), openfile_bitmap_size_bytes,
        &out_status);

    if (ret != NULL) {
        ret->offset = 0;
        ret->kobj.refcount = 1;
        ret->kobj.flags = 0;
        ret->kobj.type = KOBJECT_TYPE_OPENFILE;
        ret->kobj.ops = (kobject_ops_t){0};
        ret->kobj.self = ret;
        ret->pool_id = (uint32_t)((uint32_t)ret - OPENFILE_VIRTUAL_BASE) / sizeof(open_file_t);
        ret->target = target;

        if (target)
            file_target_get(target);
    }
    
    return ret;
}

// Unconditionally frees an open file struct back to the pool.
// Note: Caller is responsible for ensuring that the open file being freed is actually
// not in use
void fdpool_free_openfile(open_file_t* of) {
    if (of == NULL) return;
    dpage_bm_clear(of->pool_id, openfile_bitmap, openfile_bitmap_size_bytes);
    *of = (open_file_t){0}; // zero out the struct for safety; not strictly necessary
}

// Returns a pointer to the openfile at the specified pool index, if and only if
// the corresponding bitmap entry indicates that the slot is allocated.
open_file_t* fdpool_get_openfile(uint32_t index) {
    if (index >= (openfile_vaddr_size_bytes / sizeof(open_file_t))) return NULL;
    if (!dpage_is_allocated(index, openfile_bitmap, openfile_bitmap_size_bytes)) return NULL;
    return (open_file_t*)(OPENFILE_VIRTUAL_BASE + index * sizeof(open_file_t));
}

fd_entry_t* fdpool_alloc_fdentry(open_file_t* of) {
    dpage_alloc_status_t out_status;
    fd_entry_t* ret = (fd_entry_t*)dpage_alloc(fd_bitmap, 
        FD_VIRTUAL_BASE, fd_vaddr_size_bytes,
        sizeof(fd_entry_t), fd_bitmap_size_bytes,
        &out_status);

    if (ret != NULL) {
        ret->fd_id = (uint32_t)((uint32_t)ret - FD_VIRTUAL_BASE) / sizeof(fd_entry_t);  
        ret->of = of; // set by caller
        ret->next = NULL; // to be set by caller
        ret->local_id = 0; // to be set by caller
    }
    
    return ret;
}

// Unconditionally frees a file descriptor entry back to the pool.
// Note: Caller is responsible for bookkeeping of linked list of fd_entry structs,
// as well as ensuring that the fd_entry being freed is actually referenced by a
// process's fd list and not currently in use by anyone.
void fdpool_free_fdentry(fd_entry_t* fd) {
    if (fd == NULL) return;
    dpage_bm_clear(fd->fd_id, fd_bitmap, fd_bitmap_size_bytes);
    *fd = (fd_entry_t){0}; // zero out the struct for safety; not strictly necessary
}

// Returns a pointer to the fd_entry at the specified pool index, if and only if
// the corresponding bitmap entry indicates that the slot is allocated.
fd_entry_t* fdpool_get_fdentry(uint32_t index) {
    if (index >= (fd_vaddr_size_bytes / sizeof(fd_entry_t))) return NULL;
    if (!dpage_is_allocated(index, fd_bitmap, fd_bitmap_size_bytes)) return NULL;
    return (fd_entry_t*)(FD_VIRTUAL_BASE + index * sizeof(fd_entry_t));
}

file_target_t* fdpool_alloc_ftarget(kobject_type_t type) {
    dpage_alloc_status_t out_status;
    file_target_t* ret = (file_target_t*)dpage_alloc(ftarget_bitmap, 
        FTARGET_VIRTUAL_BASE, ftarget_vaddr_size_bytes,
        sizeof(file_target_t), ftarget_bitmap_size_bytes,
        &out_status);

    if (ret != NULL) {
        ret->kobj.refcount = 1;
        ret->kobj.flags = 0;
        ret->kobj.type = type;
        ret->kobj.ops = (kobject_ops_t){0};
        ret->kobj.self = ret;
        ret->io_ops = (file_target_io_ops_t){0};
        ret->data = (ftdata_t){0};
        ret->pool_id = (uint32_t)((uint32_t)ret - FTARGET_VIRTUAL_BASE) / sizeof(file_target_t);
    }
    
    return ret;
}

file_target_t* fdpool_get_ftarget(uint32_t index) {
    if (index >= (ftarget_vaddr_size_bytes / sizeof(file_target_t))) return NULL;
    if (!dpage_is_allocated(index, ftarget_bitmap, ftarget_bitmap_size_bytes)) return NULL;
    return (file_target_t*)(FTARGET_VIRTUAL_BASE + index * sizeof(file_target_t));
}

void fdpool_free_ftarget(file_target_t* ft) {
    if (ft == NULL) return;
    dpage_bm_clear(ft->pool_id, ftarget_bitmap, ftarget_bitmap_size_bytes);
    *ft = (file_target_t){0}; // zero out the struct for safety; not strictly necessary
}
