#include "sys/types.h"
#include "../include/isr.h"
#include "../include/errno.h"
#include "../include/sched.h"
#include "../include/syscall.h"
#include "../include/uaccess.h"
#include "../include/kmem.h"

// Minimal execve(2) implementation for milestone bring-up.
//
// POSIX semantics: execve() replaces the calling process's entire user-space
// image with a new program.  The process retains its PID, parent, children,
// open file descriptors (in a full implementation), etc.  Only the address
// space contents and execution context (registers) change.
//
// Constraints of this first pass:
//  - Accept exactly one path: "/bin/syscalltest"
//  - Ignore argv/envp (must be NULL for now)
//  - The executable image is a flat binary embedded at link time
//    (userprog_test..userprog_end), not loaded from a filesystem
//  - Fixed user layout: 1 code page at 0x40000000, 1 stack page at 0x40001000
//
// Strategy - "staging mm" pattern:
//   1. Build a brand-new, empty mm_t (fresh page directory)
//   2. Populate it with the new code + stack via mm_map_region()
//   3. On success: switch CR3, swap proc->addr_space, destroy the old mm
//   4. On failure: destroy the staging mm - old image is completely untouched
//
// This gives us point-of-no-return atomicity for free: nothing in the old
// address space is modified until we are certain the new one is fully built.

#define EXEC_PATH_MAX 64
#define EXEC_ALLOWED_PATH "/bin/syscalltest"

#define EXEC_CODE_VA      0x40000000u   // user code is loaded here
#define EXEC_STACK_VA     0x40001000u   // user stack page lives here
#define EXEC_PAGE_SIZE    0x1000u       // 4 KiB

// Linker-exported symbols that bracket the embedded user-mode flat binary
// To be removed later when an actual binary loader is put in place
extern uint8_t userprog_test[];
extern uint8_t userprog_end[];

static bool path_matches_allowed(const char* path) {
    // Tiny string compare (kernel-local, NUL-terminated).
    static const char allowed[] = EXEC_ALLOWED_PATH;
    for (size_t i = 0;; i++) {
        if (path[i] != allowed[i]) return false;
        if (path[i] == '\0') return true;
    }
}

static errno_t copyin_path_bounded(const char* user_path, char* out, size_t out_len) {
    // Copy a userspace pathname one byte at a time until NUL or max length.
    // Bytewise copyin avoids requiring the whole max buffer to be mapped.
    if (!user_path || !out || out_len < 2) return EFAULT;

    for (size_t i = 0; i < out_len; i++) {
        char ch = '\0';
        errno_t err = ESUCCESS;
        copyin((const void*)((uintptr_t)user_path + i), &ch, 1, &err);
        if (err != ESUCCESS) return err;

        out[i] = ch;
        if (ch == '\0') return ESUCCESS;
    }

    // No terminator seen within bound.
    out[out_len - 1] = '\0';
    return ENAMETOOLONG;
}

// execve(2) system call scaffold -- not quite the real thing yet
// Binary loader needs to be worked out before this can be treated as anything
// more than a PoC
ssize_t _execve(trap_frame_t* tf) {
    if (!tf)
        return -EINVAL;

    // Get syscall arguments (following Linux i386 syscall convention)
    const char* user_path = (const char*)tf->ebx;
    const char* const* user_argv = (const char* const*)tf->ecx;
    const char* const* user_envp = (const char* const*)tf->edx;

    // Make sure we're in a user context
    if (!_is_user(tf))
        return -EPERM;

    // argv/envp not yet supported, reject early so callers know the contract
    if (user_argv != NULL || user_envp != NULL)
        return -EINVAL;

    // Validate the user pointer is inside the caller's address space before
    // we attempt the byte-by-byte copyin
    if (!_ptr_safe(user_path, 1, tf))
        return -EFAULT;

    // Copy path string into kernel memory with sanity checks
    char kpath[EXEC_PATH_MAX];

    errno_t cpy = copyin_path_bounded(user_path, kpath, sizeof(kpath));
    if (cpy != ESUCCESS)
        return -cpy;

    // Path lookup
    if (!path_matches_allowed(kpath))
        return -ENOENT;

    // VALIDATE IMAGE
    // The "executable" is a flat binary linked into the kernel image between
    // userprog_test and userprog_end.  Sanity-check it before allocating
    // anything, so we fail fast with no cleanup needed.
    size_t image_size = (size_t)(userprog_end - userprog_test);
    if (image_size == 0)
        return -ENOEXEC;            // empty image
    if (image_size > EXEC_PAGE_SIZE)
        return -E2BIG;              // only one code page for now

    // Build blank address space
    mm_t* new_mm = mm_create();
    if (!new_mm)
        return -ENOMEM;

    // Map code page.
    //
    // NOTE: A proper ELF loader would map .text as r-x and .data/.bss as rw-.
    // Our flat binary packs code, static data, and BSS into a single page,
    // so it needs rwx for now.
    // TODO: Tighten such that it follows ELF convention as expected in the future
    int rc = mm_map_region(
        new_mm,
        EXEC_CODE_VA,                               // virtual address in user half
        EXEC_PAGE_SIZE,                              // one 4 KiB page
        VMA_TEXT,                                    // code region
        VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC,  // rwx (flat binary: code+data in one page)
        userprog_test,                               // source: embedded flat binary
        image_size                                   // only copy actual image bytes
    );
    if (rc != 0) {
        // Staging mm is destroyed - nothing in the old address space was touched.
        mm_destroy(new_mm);
        return -ENOMEM;
    }

    // Map stack page
    rc = mm_map_region(
        new_mm,
        EXEC_STACK_VA,                      // virtual address for stack page
        EXEC_PAGE_SIZE,                     // one 4 KiB page
        VMA_STACK,                          // stack region
        VMA_PROT_READ | VMA_PROT_WRITE,     // rw- (stacks don't need exec)
        NULL,                               // no initial data - zeroed
        0
    );
    if (rc != 0) {
        // mm_destroy frees both the PD and any pages/PTs already mapped
        // (including the code page we successfully mapped above).
        mm_destroy(new_mm);
        return -ENOMEM;
    }

    // Commit new address space to current process and switch to it
    process_t* proc = sched_current_process();
    mm_t* old_mm = proc->addr_space;

    // Reset trap frame
    if (!tf_has_user_tail(tf)) {
        // Should never happen due to TF check at the beginning, but here
        // just in case
        return -EINVAL;
    }

    // Commit new address space
    proc->addr_space = new_mm;
    mm_switch(new_mm);      // writes new_mm->cr3_phys into CR3
    mm_destroy(old_mm);     // reclaims all old user pages, PTs, PD, VMAs

    // User stack pointer: top of stack page, aligned down 16 bytes.
    // The 16-byte alignment satisfies the i386 ABI stack alignment convention
    // and leaves a small red zone for the entry stub.
    uint32_t new_user_esp = EXEC_STACK_VA + EXEC_PAGE_SIZE - 16;
    *tf_user_esp_slot(tf) = new_user_esp;
    *tf_user_ss_slot(tf) = (GDT_SEL_UDATA | 3);   // user data segment, RPL 3

    // Segment selectors - all point at the flat user data/code descriptors.
    tf->cs = GDT_SEL_UCODE | 3;    // user code segment, RPL 3
    tf->ds = GDT_SEL_UDATA | 3;
    tf->es = GDT_SEL_UDATA | 3;
    tf->fs = GDT_SEL_UDATA | 3;
    tf->gs = GDT_SEL_UDATA | 3;

    // Entry point - iret will pop this into EIP.
    tf->eip = EXEC_CODE_VA;

    // Zero general-purpose registers for deterministic start state.
    // (A real exec would place argc/argv/envp on the user stack and set
    // registers per the ABI; for now, all zeros is fine.)
    tf->eax = 0;
    tf->ebx = 0;
    tf->ecx = 0;
    tf->edx = 0;
    tf->esi = 0;
    tf->edi = 0;
    tf->ebp = 0;


    // note: syscall dispatch writes this return value into tf->eax, but it is
    // immediately overwritten by our zeroing above - which is fine, because
    // on a successful execve the caller never observes a return value.

    // The new image starts executing from scratch.
    return 0;
}