
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
extern ssize_t _waitpid(trap_frame_t* tf);
extern ssize_t _fork(trap_frame_t* tf);
extern ssize_t _execve(trap_frame_t* tf);

static uint32_t callcounter = 0;

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
    syscall_table[2] = _fork;
    syscall_table[4] = _write;
    syscall_table[7] = _waitpid;
    syscall_table[11] = _execve;
    syscall_table[158] = _yield;
}

extern uint8_t userprog_test[];
extern uint8_t userprog_end[];

void syscall_test(void) {
    process_t* proc = sched_current_process();
    if (!proc || !proc->addr_space) {
        kdbg_puts("syscall_test: no process or address space\r\n", 0x0C);
        return;
    }

    mm_t* mm = proc->addr_space;
    size_t code_size = userprog_end - userprog_test;

    const uint32_t code_va  = 0x40000000;
    const uint32_t stack_va = 0x40001000;

    int res = mm_map_region(mm, code_va, 0x1000,
                            VMA_TEXT, VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC,
                            userprog_test, code_size);
    if (res != 0) {
        kdbg_puts("syscall_test: failed to map code\r\n", 0x0C);
        return;
    }

    res = mm_map_region(mm, stack_va, 0x1000,
                        VMA_STACK, VMA_PROT_READ | VMA_PROT_WRITE,
                        NULL, 0);
    if (res != 0) {
        kdbg_puts("syscall_test: failed to map stack\r\n", 0x0C);
        return;
    }

    kdbg_puts("User program loaded via mm_map_region. Jumping to it...\r\n", 0x0A);
    mm_switch(mm);
    enter_usermode(code_va, stack_va + 0x1000 - 16,
                   GDT_SEL_UCODE | 3, GDT_SEL_UDATA | 3);
}