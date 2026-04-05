
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "isr.h"
#include "./sys/types.h"

#define MAX_PROCESSES 65535
#define MAX_THREADS 128
#define STACK_CANARY 128
#define MAXCOMLEN 16
#define IDLECOMNAME "sysidle"

typedef enum process_state {
    PROC_UNUSED = 0x00,
    PROC_ALIVE = 0x01,
    PROC_ZOMBIE = 0x02,
    PROC_REAPED = 0x03
} process_state_t;

typedef enum process_context {
    CTX_USER = '\0',
    CTX_DRIVER = '^',
    CTX_KERNEL = '@'
} process_context_t;

typedef enum thread_state {
    THREAD_UNUSED = 0x00,
    THREAD_RUNNABLE = 0x01,
    THREAD_RUNNING = 0x02,
    THREAD_SLEEPING = 0x03,
    THREAD_BLOCKED = 0x04,
    THREAD_ZOMBIE = 0x05
} thread_state_t;

typedef enum wait_kind {
    WAIT_NONE = 0,
    WAIT_ANY = 1,
    WAIT_CHILD = 2,
    WAIT_SLEEP = 3,
    WAIT_IO = 4
} wait_kind_t;

typedef enum reap_result {
    REAP_SUCCESS = 0,
    REAP_NO_ZOMBIE = 1,
    REAP_INVALID_PID = 2,
    REAP_UNLINKED_CHILD = 3
} reap_result_t;

typedef struct mm {
    uintptr_t cr3_phys; // physical address of page directory
    uint32_t refcount;
    uintptr_t user_base; // base of user-space mapping (for sanity checks, not necessarily used for anything else)
    uintptr_t user_limit; // top of user-space mapping (must be < KERNEL_VIRTUAL_BASE)
    uint32_t page_count; 
} mm_t;

typedef struct process {
    bool slot_inuse;
    size_t slot_id;
    pid_t pid;
    pid_t ppid;
    process_state_t state;
    size_t live_thread_count;
    int exit_code;
    process_context_t context;
    char comm[MAXCOMLEN];
    struct process *parent;
    struct process *first_child;
    struct process *next_sibling;
    struct thread* main_thread;
    struct thread* thread_list;
    struct thread* waiters;
    mm_t* addr_space;
} process_t;

typedef struct thread {
    bool slot_inuse;
    size_t slot_id;
    int generation_id;
    tid_t tid;
    thread_state_t state;
    struct process* proc;
    trap_frame_t tf;
    uint32_t saved_esp;
    void* kstack_base;
    void* kstack_top;
    uint32_t wake_tick;
    int exit_code;
    void* entry; // real entry point for thread
    wait_kind_t wait_kind;
    pid_t wait_target_pid;
    struct process* wait_owner;
    struct thread* wait_next;
    struct thread* rq_next; // for runqueue
    struct thread* sleep_next; // for sleep queue
    struct thread* threadlist_next;
} thread_t;

extern volatile uint8_t sched_pending;

void sched_init(void);
thread_t* sched_add_thread(void (*entry)(void), process_t* proc);
uint32_t sched_do_switch(uint32_t old_esp);
void sched_on_tick(void);
void sched_thread_exit(int code);
void sched_thread_sleep(uint64_t ticks);
void sched_thread_yield(void);
void sched_block_current(wait_kind_t wait_kind, pid_t wait_target_pid);
void sched_wake_thread(thread_t* t);
void thread_kill_current(const char* reason, const size_t code);

thread_t* sched_current_thread(void);
process_t* sched_current_process(void);

process_t* proc_alloc(process_t* parent, process_context_t context, const char* comm);
void proc_free(process_t* p);
process_t* proc_find(pid_t pid);
void proc_add_child(process_t* parent, process_t* child);
void proc_remove_child(process_t* parent, process_t* child);
process_t* proc_find_zombie_child(process_t* parent, pid_t child_pid);
bool proc_has_children(process_t* parent);
void thread_reap(thread_t* t);
void proc_reap_child(process_t* parent, process_t* child, reap_result_t* out_result);
ssize_t proc_waitpid(process_t* parent, int32_t pid_filter, int* out_status);
bool sched_clone_fork(process_t* parent, process_t* child, trap_frame_t* parent_tf);
