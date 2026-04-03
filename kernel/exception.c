
#include "paging.h"
#include "syscall.h"
#include "kmain.h"
#include "exception.h"
#include "include/sched.h"
#include "panic.h"

#define GET_USERMODE(n) ((n & 0x3) == 3)

void exception_init() {
    isr_register(0x06, exception_ud);
    isr_register(0x0B, exception_np);
    isr_register(0x0C, exception_ss);
    isr_register(0x0D, exception_gpf);
    isr_register(0x0E, exception_pf);
}

void exception_gpf(trap_frame_t* tf) {
    kdbg_puts("#GP\r\n", 0x0C);
    kdbg_dump_current();

    if (GET_USERMODE(tf->cs)) {
        kdbg_puts("Killing offending thread...\r\n", 0x0C);
        thread_kill_current("General Protection Fault", tf->error);
    } else {
        panic("General Protection Fault in kernel mode", tf);
        HALT_FOREVER;
    }
}

void exception_pf(trap_frame_t* tf) {
    
    #define PRINT_PF do {\
        kdbg_puts("#PF: cr2=", 0x0C); kdbg_hex32(cr2, 0x0C);\
        kdbg_puts(" err=", 0x0C); kdbg_hex32(err, 0x0C);\
        kdbg_puts(" P=", 0x0C); kdbg_hex32(present, 0x0C);\
        kdbg_puts(" W=", 0x0C); kdbg_hex32(write, 0x0C);\
        kdbg_puts(" U=", 0x0C); kdbg_hex32(user, 0x0C);\
        kdbg_puts("\r\n", 0x0C);\
        kdbg_dump_frame((const trap_tail_t*)&tf->vector);\
    kdbg_dump_current(); } while (0);    

    uint32_t err = tf->error; 
    uint32_t cr2 = read_cr2();

    // 386-relevant bits
    uint32_t present = (err & 0x1);        // 0: not-present, 1: protection violation
    uint32_t write   = (err >> 1) & 0x1;   // 0: read, 1: write
    uint32_t user    = ((err >> 2) & 0x1);  // 0: supervisor, 1: user

    // Note: Always use trap frame to classify origin, NEVER use CPL at time of #PF
    // as we're already in the #PF handler which puts us in kernel mode
    if (user) {
        PRINT_PF;

        kdbg_puts("Trap frame excerpt at #PF:\r\n", 0x0C);
        kdbg_puts("EIP: ", 0x0C); kdbg_hex32(tf->eip, 0x0C); 
        kdbg_puts(" CS: ", 0x0C); kdbg_hex32(tf->cs, 0x0C);
        kdbg_puts(" EFLAGS: ", 0x0C); kdbg_hex32(tf->eflags, 0x0C); 
        kdbg_puts("\r\n\r\n", 0x0C);

        // TODO: actual SIGKILL semantics in the future
        kdbg_puts("Killing offending thread...\r\n", 0x0C);
        thread_kill_current("Page fault", err);
    } else {
        PRINT_PF;
        panic("Page fault in kernel mode", tf);
    }

    #undef PRINT_PF    
}

void exception_ud(trap_frame_t* tf) {
    kdbg_puts("#UD\r\n", 0x0C);
    kdbg_dump_current();

    if (GET_USERMODE(tf->cs)) {
        kdbg_puts("Killing offending thread...\r\n", 0x0C);
        thread_kill_current("Invalid Opcode", tf->error);
    } else {
        panic("Invalid Opcode in kernel mode", tf);
        HALT_FOREVER;
    }
}

void exception_ss(trap_frame_t* tf) {
    kdbg_puts("#SS\r\n", 0x0C);
    kdbg_dump_current();

    if (GET_USERMODE(tf->cs)) {
        kdbg_puts("Killing offending task...\r\n", 0x0C);
        thread_kill_current("Stack Segment Fault", tf->error);
    } else {
        panic("Stack Segment Fault in kernel mode", tf);
        HALT_FOREVER;
    }
}

void exception_np(trap_frame_t* tf) {
    kdbg_puts("#NP\r\n", 0x0C);
    kdbg_dump_current();

    if (GET_USERMODE(tf->cs)) {
        kdbg_puts("Killing offending thread...\r\n", 0x0C);
        thread_kill_current("Segment Not Present", tf->error);
    } else {
        panic("Segment Not Present in kernel mode", tf);
        HALT_FOREVER;
    }
}
