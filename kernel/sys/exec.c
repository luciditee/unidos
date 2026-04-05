#include "sys/types.h"
#include "../include/isr.h"
#include "../include/errno.h"
#include "../include/sched.h"
#include "../include/syscall.h"
#include "../include/uaccess.h"
#include "../include/kmem.h"

// Minimal execve(2) implementation for milestone bring-up.
//
// Constraints of this first pass:
//  - Accept exactly one path: "/bin/syscalltest"
//  - Ignore argv/envp (must be NULL for now)
//  - Replace only a tiny fixed user image layout (1 code page + 1 stack page)
//  - Revector current trap frame EIP to new image entry and return through iret
//
// This keeps the implementation simple while process address spaces / CR3 switching
// are still under construction.

#define EXEC_PATH_MAX 64
#define EXEC_ALLOWED_PATH "/bin/syscalltest"

#define EXEC_CODE_VA      0x40000000u
#define EXEC_STACK_VA     0x40001000u

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
    // Bytewise copy avoids requiring the whole max buffer to be mapped.
    if (!user_path || !out || out_len < 2) return -EFAULT;

    for (size_t i = 0; i < out_len; i++) {
        char ch = '\0';
        errno_t err = copyin((const void*)((uintptr_t)user_path + i), &ch, 1);
        if (err != ESUCCESS) return err;

        out[i] = ch;
        if (ch == '\0') return ESUCCESS;
    }

    // No terminator seen within bound.
    out[out_len - 1] = '\0';
    return -ENAMETOOLONG;
}

static errno_t unmap_user_page_if_present(uint32_t va) {
    // Best-effort unmap of existing page at VA so exec can install a fresh image.
    paging_query_result_t q;
    paging_status_t qst = paging_query_page(va, &q);
    if (qst != PAGING_OK) return -EFAULT;

    if (!q.mapped) return ESUCCESS;

    paging_status_t ust = paging_unmap_and_free_page(va);
    if (ust != PAGING_OK) return -EFAULT;
    return ESUCCESS;
}

ssize_t _execve(trap_frame_t* tf) {
    if (!tf)
        return -EINVAL;

    // Linux i386 calling convention for execve(path, argv, envp):
    //   ebx = path, ecx = argv, edx = envp
    const char* user_path = (const char*)tf->ebx;
    const char* const* user_argv = (const char* const*)tf->ecx;
    const char* const* user_envp = (const char* const*)tf->edx;

    // This minimal path is only for user-originated syscall context.
    if (!_is_user(tf)) {
        //kdbg_puts("execve: non-user context\r\n", 0x0C);
        return -EPERM;
    }

    // For first pass, keep contract explicit and narrow: argv/envp unsupported.
    if (user_argv != NULL || user_envp != NULL) {
        //kdbg_puts("execve: argv/envp unsupported\r\n", 0x0C);
        return -EINVAL;
    }

    // Quick range check before deeper copyin validation.
    if (!_ptr_safe(user_path, 1, tf)) {
        //kdbg_puts("execve: invalid path pointer\r\n", 0x0C);
        return -EFAULT;
    }

    char kpath[EXEC_PATH_MAX];
    errno_t cpy = copyin_path_bounded(user_path, kpath, sizeof(kpath));
    if (cpy != ESUCCESS) {
        //kdbg_puts("execve: failed to copyin path\r\n", 0x0C);
        return cpy;
    }

    //kdbg_puts("execve: requested path: ", 0x0E);
    //kdbg_puts(kpath, 0x0E);
    //kdbg_puts("\r\n", 0x0E);
    //kdbg_puts("execve: raw ebx pointer value: 0x", 0x0E);
    //kdbg_hex32((uint32_t)user_path, 0x0E);
    //kdbg_puts("\r\n", 0x0E);


    // Constrained bring-up policy: exactly one executable path is recognized.
    if (!path_matches_allowed(kpath)) {
        /*kdbg_puts("execve: path not found: ", 0x0C);
        kdbg_puts(kpath, 0x0C);
        kdbg_puts("\r\n", 0x0C);*/
        //kdbg_puts("execve: path mismatch\r\n", 0x0C);
        return -ENOENT;
    }

    // Embedded image bounds exported by linker/asm for user test program.
    size_t image_size = (size_t)(userprog_end - userprog_test);
    if (image_size == 0) {
        //kdbg_puts("execve: empty image\r\n", 0x0C);
        return -ENOEXEC;
    }
    if (image_size > 0x1000u) {
        //kdbg_puts("execve: image too large\r\n", 0x0C);
        return -E2BIG; // this minimal loader only supports one code page
    }

    // Remove any old mappings in our fixed layout slots.
    errno_t unmap_code = unmap_user_page_if_present(EXEC_CODE_VA);
    if (unmap_code != ESUCCESS) {
        //kdbg_puts("execve: failed to unmap code page\r\n", 0x0C);
        return unmap_code;
    }

    errno_t unmap_stack = unmap_user_page_if_present(EXEC_STACK_VA);
    if (unmap_stack != ESUCCESS) {
        //kdbg_puts("execve: failed to unmap stack page\r\n", 0x0C);
        return unmap_stack;
    }

    // Allocate physical frames for fresh code + stack pages.
    pmm_alloc_result_t a_code, a_stack;
    uint32_t phys_code = pmm_get_next_available_block(&a_code);
    if (a_code != PMM_ALLOC_SUCCESS) {
        //kdbg_puts("execve: failed to allocate code page\r\n", 0x0C);    
        return -ENOMEM;
    }

    uint32_t phys_stack = pmm_get_next_available_block(&a_stack);
    if (a_stack != PMM_ALLOC_SUCCESS) {
        //kdbg_puts("execve: failed to allocate stack page\r\n", 0x0C);
        pmm_dealloc_specific_block(phys_code);
        return -ENOMEM;
    }

    // Map the new pages as user-accessible, present, and writable.
    paging_status_t m_code = paging_map_page(
        EXEC_CODE_VA,
        phys_code,
        PG_PRESENT | PG_RW | PG_USER,
        NULL
    );
    if (m_code != PAGING_OK) {
        pmm_dealloc_specific_block(phys_stack);
        pmm_dealloc_specific_block(phys_code);
        //kdbg_puts("execve: failed to map code page\r\n", 0x0C);
        return -EFAULT;
    }

    paging_status_t m_stack = paging_map_page(
        EXEC_STACK_VA,
        phys_stack,
        PG_PRESENT | PG_RW | PG_USER,
        NULL
    );
    if (m_stack != PAGING_OK) {
        paging_unmap_and_free_page(EXEC_CODE_VA);
        pmm_dealloc_specific_block(phys_stack);
        //kdbg_puts("execve: failed to map stack page\r\n", 0x0C);
        return -EFAULT;
    }

    // Load user image bytes into code page.
    kmemcpy((void*)EXEC_CODE_VA, userprog_test, image_size);

    // Zero the rest of code page for deterministic state.
    for (uint32_t i = (uint32_t)image_size; i < 0x1000u; i++) {
        ((volatile uint8_t*)EXEC_CODE_VA)[i] = 0;
    }

    // Zero entire user stack page.
    for (uint32_t i = 0; i < 0x1000u; i++) {
        ((volatile uint8_t*)EXEC_STACK_VA)[i] = 0;
    }

    if (!tf_has_user_tail(tf)) {
        //kdbg_puts("execve: missing user tail in trap frame\r\n", 0x0C);
        return -EINVAL;
    }

    // reset user return stack pointer
    uint32_t new_user_esp = EXEC_STACK_VA + 0x1000u - 16; // stack grows down, so start at top of page
    *tf_user_esp_slot(tf) = new_user_esp; // user ESP saved in user tail slot
    *tf_user_ss_slot(tf) = (GDT_SEL_UDATA | 3);

    // reset user segments
    tf->cs = GDT_SEL_UCODE | 3; // user code segment with RPL 3
    tf->ds = GDT_SEL_UDATA | 3; // user data segment
    tf->es = GDT_SEL_UDATA | 3; // ditto
    tf->fs = GDT_SEL_UDATA | 3; // ditto
    tf->gs = GDT_SEL_UDATA | 3; // ditto


    // Commit new execution context in the current trap frame.
    //
    // On return from syscall handler, iret will consume tf->eip/cs/eflags and
    // continue in user mode at the new image entry.
    tf->eip = EXEC_CODE_VA;

    // We do not yet build argc/argv/envp on the user stack. For now, clear GPRs
    // so start state is deterministic.
    tf->ebx = 0;
    tf->ecx = 0;
    tf->edx = 0;
    tf->esi = 0;
    tf->edi = 0;
    tf->ebp = 0;

    // Success: syscall_handler will write this into EAX before returning.
    return 0;
}