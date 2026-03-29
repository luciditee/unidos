
#include "sys/types.h"
#include "include/syscall.h"
#include "kmain.h"
#include "kmem.h"
#include "paging.h"
#include "panic.h"
#include "isr.h"
#include "errno.h"
#include "include/sched.h"

#define SYSCALL_MAX 512 // TODO: this was arbitrarily chosen, but when we are done with syscalls this should be adjusted

extern void enter_usermode(uint32_t entry_addr, uint32_t user_esp, uint32_t user_cs, uint32_t user_ds);

// System call function type
typedef ssize_t (*syscall)(trap_frame_t* tf);

// Syscall table mapping. NULL == not implemented.
static syscall syscall_table[SYSCALL_MAX] = {0};

// Externs for syscall functions defined elsewhere
extern ssize_t _write(trap_frame_t* tf);
extern ssize_t _exit(trap_frame_t* tf);
extern ssize_t _yield(trap_frame_t* tf);

void syscall_handler(trap_frame_t* tf) {
    uint32_t nr = tf->eax;

    /*kdbg_hex32(nr, 0x0E);
    kdbg_puts("\r\n", 0x0E);*/

    if (nr >= SYSCALL_MAX || syscall_table[nr] == NULL) {
        tf->eax = (uint32_t)-ENOSYS;
        return;
    }

    syscall fn = syscall_table[nr];
    ssize_t result = fn(tf);
    tf->eax = (uint32_t)result;
}

void syscall_init() {
    // Register syscall handler for vector 0x80 (arbitrary choice for now)
    isr_register(0x80, syscall_handler);

    // Register syscalls to their respective indices
    // Note: Mapping these to Linux's syscall numbers for i386 where possible,
    //       which helps towards ABI compatibility in the future
    // References:
    // https://git.whoi.edu/dgiaya/linux/-/blob/0215ffb08ce99e2bb59eca114a99499a4d06e704/arch/i386/kernel/syscall_table.S
    // Not the same as x86-64 syscalls! If the above link dies, the Linux i386 source
    // tree location for syscall_table.S can be found at (linux)/arch/i386/kernel/sys_call_table.S
    syscall_table[1] = _exit;       // noreturn
    syscall_table[4] = _write;
    syscall_table[158] = _yield;
}

extern uint8_t userprog_test[];
extern uint8_t userprog_end[];

void syscall_test(void) {    
    size_t syscall_size = userprog_end - userprog_test;
    
    const uint32_t loadAddr = 0x40000000; // user-space test VA (clean PDE)
    const uint32_t stackAddr = 0x40001000; // user-space test stack VA (next page after code)
    uint32_t user_phys = 0;
    uint32_t stack_phys = 0;
    pmm_alloc_result_t ares1, ares2;
    user_phys = pmm_get_next_available_block(&ares1);
    stack_phys = pmm_get_next_available_block(&ares2);
    if (ares1 != PMM_ALLOC_SUCCESS || ares2 != PMM_ALLOC_SUCCESS) { 
        kdbg_puts("Failed to phys alloc page frame for testing syscall\r\n", 0x0C);
        return;
    }

    paging_status_t status = paging_map_page(
    loadAddr, user_phys, PG_PRESENT | PG_RW | PG_USER, NULL);
    if (status != PAGING_OK) {
        if (!(status == PAGING_ERR_ALREADY_MAPPED && user_phys != loadAddr)) {
            kdbg_puts("Failed to map test page for testing syscall (code ", 0x0C);
            kdbg_hex32(status, 0x0C);
            kdbg_puts(")\r\n", 0x0C);
            return;
        }
    }

    paging_status_t stack_status = paging_map_page(
        stackAddr, stack_phys, PG_PRESENT | PG_RW | PG_USER, NULL);
    if (stack_status != PAGING_OK) {
        if (!(stack_status == PAGING_ERR_ALREADY_MAPPED && stack_phys != stackAddr)) {
            kdbg_puts("Failed to map test stack page for testing syscall (code ", 0x0C);
            kdbg_hex32(stack_status, 0x0C);
            kdbg_puts(")\r\n", 0x0C);
            return;
        }
    }

    kdbg_puts("Copying test program\r\n", 0x0A);
    kmemcpy((void*)loadAddr, userprog_test, syscall_size);

    kdbg_puts("User program loaded. Jumping to it...\r\n", 0x0A);
    enter_usermode(loadAddr, stackAddr + 0x1000 - 16, GDT_SEL_UCODE | 3, GDT_SEL_UDATA | 3);
}