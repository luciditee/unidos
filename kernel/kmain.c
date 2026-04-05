#include "kglobal.h"
#include "kmain.h"
#include "isr.h"
#include "io.h"
#include "pit.h"
#include "kbm.h"
#include "bootinfo.h"
#include "include/sched.h"
#include "kmem.h"
#include "kremap.h"
#include "exception.h"
#include "include/syscall.h"
#include "include/errno.h"

static pid_t g_wait_specific_pid = 0;

static void on_int3(trap_frame_t* tf) {
    (void)tf;
    kdbg_puts("INT3 handled in C\r\n", 0x0A);
    kdbg_dump_current();
}

static void test_wait_parent_thread(void) {
    kdbg_puts("[wait-selftest] parent: waiting for any child...\r\n", 0x0E);

    int status = 0;
    int32_t pid = proc_waitpid(sched_current_process(), -1, &status);
    if (pid < 0) {
        kdbg_puts("[wait-selftest] parent: waitpid failed: ", 0x0C);
        kdbg_hex32((uint32_t)(-pid), 0x0C);
        kdbg_puts("\r\n", 0x0C);
        sched_thread_exit(1);
    }

    kdbg_puts("[wait-selftest] parent: reaped pid=", 0x0A);
    kdbg_hex32((uint32_t)pid, 0x0A);
    kdbg_puts(" status=", 0x0A);
    kdbg_hex32((uint32_t)status, 0x0A);
    kdbg_puts("\r\n", 0x0A);

    kdbg_puts("[wait-selftest] parent: waiting for specific child pid=", 0x0E);
    kdbg_hex32((uint32_t)g_wait_specific_pid, 0x0E);
    kdbg_puts("...\r\n", 0x0E);

    status = 0;
    pid = proc_waitpid(sched_current_process(), (int32_t)g_wait_specific_pid, &status);
    if (pid < 0) {
        kdbg_puts("[wait-selftest] parent: waitpid(specific) failed: ", 0x0C);
        kdbg_hex32((uint32_t)(-pid), 0x0C);
        kdbg_puts("\r\n", 0x0C);
        sched_thread_exit(2);
    }

    kdbg_puts("[wait-selftest] parent: reaped specific pid=", 0x0A);
    kdbg_hex32((uint32_t)pid, 0x0A);
    kdbg_puts(" status=", 0x0A);
    kdbg_hex32((uint32_t)status, 0x0A);
    kdbg_puts("\r\n", 0x0A);

    // ECHILD path 1: ask for already-reaped specific child
    pid = proc_waitpid(sched_current_process(), (int32_t)g_wait_specific_pid, &status);
    if (pid != -ECHILD) {
        kdbg_puts("[wait-selftest] parent: expected -ECHILD for specific pid, got ", 0x0C);
        kdbg_hex32((uint32_t)pid, 0x0C);
        kdbg_puts("\r\n", 0x0C);
        sched_thread_exit(3);
    }
    kdbg_puts("[wait-selftest] parent: specific pid -ECHILD OK\r\n", 0x0A);

    // ECHILD path 2: no children left for wait-any
    pid = proc_waitpid(sched_current_process(), -1, &status);
    if (pid != -ECHILD) {
        kdbg_puts("[wait-selftest] parent: expected -ECHILD for wait-any, got ", 0x0C);
        kdbg_hex32((uint32_t)pid, 0x0C);
        kdbg_puts("\r\n", 0x0C);
        sched_thread_exit(4);
    }
    kdbg_puts("[wait-selftest] parent: wait-any -ECHILD OK\r\n", 0x0A);

    sched_thread_exit(0);
}

static void test_wait_child_thread(void) {
    kdbg_puts("[wait-selftest] child: running\r\n", 0x0B);
    sched_thread_sleep(25); // ensure parent reaches blocking wait first
    kdbg_puts("[wait-selftest] child: exiting with code 0x2A\r\n", 0x0B);
    sched_thread_exit(0x2A);
}

static void test_wait_child2_thread(void) {
    kdbg_puts("[wait-selftest] child2: running\r\n", 0x09);
    sched_thread_sleep(60); // exits after child1 so parent can test specific wait second
    kdbg_puts("[wait-selftest] child2: exiting with code 0x33\r\n", 0x09);
    sched_thread_exit(0x33);
}

static void start_waitpid_selftest(void) {
    process_t* parent = proc_alloc(NULL, CTX_KERNEL, "twait-parent");
    if (!parent) {
        kdbg_puts("[wait-selftest] failed: parent proc alloc\r\n", 0x0C);
        return;
    }

    process_t* child1 = proc_alloc(parent, CTX_KERNEL, "twait-child1");
    if (!child1) {
        kdbg_puts("[wait-selftest] failed: child1 proc alloc\r\n", 0x0C);
        return;
    }

    process_t* child2 = proc_alloc(parent, CTX_KERNEL, "twait-child2");
    if (!child2) {
        kdbg_puts("[wait-selftest] failed: child2 proc alloc\r\n", 0x0C);
        return;
    }

    g_wait_specific_pid = child2->pid;

    if (!sched_add_thread(test_wait_parent_thread, parent)) {
        kdbg_puts("[wait-selftest] failed: parent thread create\r\n", 0x0C);
        return;
    }

    if (!sched_add_thread(test_wait_child_thread, child1)) {
        kdbg_puts("[wait-selftest] failed: child1 thread create\r\n", 0x0C);
        return;
    }

    if (!sched_add_thread(test_wait_child2_thread, child2)) {
        kdbg_puts("[wait-selftest] failed: child2 thread create\r\n", 0x0C);
        return;
    }

    kdbg_puts("[wait-selftest] started\r\n", 0x0A);
}

// Prints a 1 on the screen, then burns cycles.
void test_task1() {
    while (1) {
        kdbg_puts("1", 0x0D);
        for (volatile int i = 0; i < 1000; i++);
    }
}

// Identical to above, but prints 2
void test_task2() {
    while (1) {
        kdbg_puts("2", 0x0D);
        for (volatile int i = 0; i < 1000; i++);
    }
}

// Exits after printing a few times, to test task cleanup.
void test_task3() {
    for (volatile int i = 0; i < 5; i++){
        kdbg_puts("3", 0x0D);
        for (volatile int j = 0; j < 1000; j++);
    }
}

// Exits immediately after printing a 4 to test exit code handling.
void test_task4() {
    kdbg_puts("4", 0x0D);
    sched_thread_exit(42);
}

// Prints a 5, sleeps for a while, then prints another 5 to test sleeping.
void test_task5() {
    for (;;) {
        kdbg_puts("5", 0x0C);
        sched_thread_sleep(100); // sleep for 100 ticks
    }
}

extern void test_paging();

void ktrampoline() {
    bootinfo_init();
    mem_init();
    kernel_highhalf_remap();
    return;
}

void kmain() {
    kdbg_puts("Entered kmain\r\n", 0x0F);

    exception_init();
    pic_remap();
    pit_init();    
    test_paging();
    kb_init();
    sched_init();
    syscall_init();
    //start_waitpid_selftest();
    __asm__ __volatile__ ("sti");

    process_t* p = proc_alloc(NULL, CTX_KERNEL, "testproc"); 
    sched_add_thread(syscall_test, p);

    //trigger_intentional_page_fault();

    //sched_add_thread(test_task1);
    //sched_add_thread(test_task2);
    //sched_add_thread(test_task2); // test multiple instances
    /*sched_add_thread(test_task3);
    sched_add_thread(test_task4);
    sched_add_thread(test_task5);*/

    HALT_FOREVER;
}
