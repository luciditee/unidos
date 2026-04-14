
#pragma once

#include <stdint.h>
#include "file.h"

// Searches for a free spot in the openfile pool, marks it as allocated, populates
// some basic fields, and returns a pointer to the allocated open_file_t.
open_file_t* fdpool_alloc_openfile(file_target_t* target);

// Unconditionally frees an open file struct back to the pool.
// Note: Caller is responsible for ensuring that the open file being freed is actually
// not in use
void fdpool_free_openfile(open_file_t* of);

// Returns a pointer to the openfile at the specified pool index, if and only if
// the corresponding bitmap entry indicates that the slot is allocated.
open_file_t* fdpool_get_openfile(uint32_t index);

// Allocates a file descriptor entry referencing a specified open file, for use
// with a process's fd list. This function does not adjust open_file or target
// refcounts; callers should use open_file_get/open_file_put at ownership boundaries.
fd_entry_t* fdpool_alloc_fdentry(open_file_t* of);

// Unconditionally frees a file descriptor entry back to the pool.
// Note: Caller is responsible for bookkeeping of linked list of fd_entry structs,
// as well as ensuring that the fd_entry being freed is actually referenced by a
// process's fd list and not currently in use by anyone.
void fdpool_free_fdentry(fd_entry_t* fd);

// Returns a pointer to the fd_entry at the specified pool index, if and only if
// the corresponding bitmap entry indicates that the slot is allocated.
fd_entry_t* fdpool_get_fdentry(uint32_t index);

file_target_t* fdpool_alloc_ftarget(kobject_type_t type);

file_target_t* fdpool_get_ftarget(uint32_t index);

void fdpool_free_ftarget(file_target_t* ft);
