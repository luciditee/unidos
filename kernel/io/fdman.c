
#include "io.h"
#include "io/file.h"
#include "kmem.h"
#include "dpage.h"

/* File descriptor management 
   This file handles tables of file descriptors for use by processes
 */


fd_entry_t* fde_fork_copy(fd_entry_t* src) {
    if (src == NULL) return NULL;

    fd_entry_t* copy_head = NULL;
    fd_entry_t* copy_tail = NULL;

    // Under POSIX.1, a forked child gets copies of the parent's file descriptors.
    // This means new a copied fd_entry_t struct (as well as its downstream linked list),
    // but the open_file_t it references is shared between parent and child.
    while (src) {
        fd_entry_t* copy = fdpool_alloc_fdentry(src->of);
        if (!copy_head) copy_head = copy; // set head if not set yet

        if (copy == NULL) {
            // Allocation failure -- free everything we've allocated so far and return NULL
            while (copy_head) {
                fd_entry_t* next = copy_head->next;
                open_file_put(copy_head->of);
                fdpool_free_fdentry(copy_head);
                copy_head = next;
            }
            return NULL;
        }
        
        copy->of = src->of; // shared open file struct
        copy->local_id = src->local_id; // same local ID in child as parent for ease of lookup

        if (copy->of)
            open_file_get(copy->of);

        // Insert copy into linked list
        if (copy_tail)copy_tail->next = copy;
        else copy_head = copy;

        copy_tail = copy;

        src = src->next;
    }

    return copy_head;
}
