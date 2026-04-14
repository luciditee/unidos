
#include "include/sched.h"
#include "include/kmain.h"
#include "include/pit.h"
#include "include/isr.h"
#include "include/pool.h"
#include "include/errno.h"
#include "include/panic.h"
#include "include/mm.h"
#include "include/io.h"
#include "include/io/fdpool.h"
#include "include/io/console.h"
#include "include/unistd.h"


#define RR_SCHEDULER_CADENCE 10 // every N ticks
#define MIN(a,b) ((a) < (b) ? (a) : (b))

static uint32_t rr_ticks_left = RR_SCHEDULER_CADENCE;

static thread_t* thread_pool = 0;
static size_t thread_pool_count = 0;
static process_t* process_pool = 0;
static size_t process_pool_count = 0;

static thread_t* thread_head = 0;
static thread_t* thread_current = 0;
static thread_t* thread_sleep_head = 0;

volatile uint8_t sched_pending = 0;
volatile uint64_t last_sched_tick = 0;
static uint32_t next_tid = 0;

static void thread_bootstrap(void);
static void sched_on_int81h(trap_frame_t* tf);
static void sched_idle(void);
static void wake_parent_waiter(process_t* child);

extern uint8_t tss32;
extern void mm_init(void);
extern void vma_init(void);

extern fd_entry_t* fde_fork_copy(fd_entry_t* src);

static void _sched_strncpy(char* dest, const char* src, size_t n) {
    size_t lim = MIN(n, MAXCOMLEN);
    size_t i = 0;
    for (; i < lim; i++) {
        dest[i] = src[i];
        if (src[i] == '\0') break;
    }
    if (lim > 0) {
        if (i == lim || src[i] != '\0') dest[lim - 1] = '\0';
    }
}

static inline void sched_set_tss_esp0(thread_t* t) {
    if (!t) return;
    volatile uint32_t* esp0 = (volatile uint32_t*)((uintptr_t)&tss32 + 4);
    volatile uint16_t* ss0  = (volatile uint16_t*)((uintptr_t)&tss32 + 8);
    *esp0 = (uint32_t)(uintptr_t)(t->kstack_top);
    *ss0 = GDT_SEL_KDATA;
}

static thread_t* alloc_thread_slot(size_t* out_index) {
    if (thread_pool_count == 0) return NULL;

    size_t start = (size_t)(next_tid % thread_pool_count);
    for (size_t scanned = 0, i = start; scanned < thread_pool_count; scanned++, i = (i + 1) % thread_pool_count) {
        if (thread_pool[i].slot_inuse == false) {
            if (out_index) *out_index = i;
            next_tid = (uint32_t)((i + 1) % thread_pool_count);
            return &thread_pool[i];
        }
    }
    return NULL;
}

static process_t* alloc_process_slot(size_t* out_index) {
    for (size_t i = 0; i < process_pool_count; i++) {
        if (process_pool[i].slot_inuse == false) {
            if (out_index) *out_index = i;
            return &process_pool[i];
        }
    }
    return NULL;
}

static void build_initial_frame(thread_t* t, void (*start_eip)(void)) {
    uint32_t* sp = (uint32_t*)(t->kstack_top);

    #define PUSH(v) (*--sp = (uint32_t)(v))

    // Note: These must match the exact order of pushes in isr_common_entry,
    // and consequently, the exact layout of trap_frame_t.
    //
    // User tail (SS3 / ESP3) -- always reserved, even for kernel threads.
    //
    // When iret sees a ring-0 CS it ignores these two dwords and pops only
    // EIP/CS/EFLAGS, so they are harmless for kernel threads.  But for
    // user-mode threads (e.g. a fork'd child), the CPU *does* pop them to
    // restore user SS:ESP.  sched_clone_fork copies the parent's user tail
    // into these slots; if we didn't reserve the space here, that copy
    // would write past the end of the trap frame and corrupt the stack,
    // leading to a #GP on the child's first iret.
    PUSH(0);                     // user SS  (placeholder)
    PUSH(0);                     // user ESP (placeholder)

    PUSH(0x00000202u);           // EFLAGS (IF=1)
    PUSH(GDT_SEL_KCODE);         // CS
    PUSH((uint32_t)start_eip);   // EIP
    PUSH(0);                     // error
    PUSH(0);                     // vector
    PUSH(0);                     // cr2

    PUSH(GDT_SEL_KDATA);         // gs
    PUSH(GDT_SEL_KDATA);         // fs
    PUSH(GDT_SEL_KDATA);         // es
    PUSH(GDT_SEL_KDATA);         // ds

    PUSH(0); // eax
    PUSH(0); // ecx
    PUSH(0); // edx
    PUSH(0); // ebx
    PUSH(0); // esp dummy
    PUSH(0); // ebp
    PUSH(0); // esi
    PUSH(0); // edi

    #undef PUSH
    t->saved_esp = (uint32_t)sp;
}

void sched_init(void) {
    // note: we'll always get a valid thread_pool_base and process_pool_base
    // back from this function. The only way that doesn't happen is if a
    // kernel panic happens inside pool_init.
    uint32_t thread_pool_base, process_pool_base;
    pool_init(&thread_pool_base, &thread_pool_count, 
        &process_pool_base, &process_pool_count);
    
    // init pools for mm and vma
    mm_init();
    vma_init();

    // set up pointers for indexing
    thread_pool = (thread_t*)thread_pool_base;
    process_pool = (process_t*)process_pool_base;
    
    thread_current = NULL;
    thread_head = NULL;
    thread_sleep_head = NULL;
    
    sched_pending = 0;
    rr_ticks_left = RR_SCHEDULER_CADENCE;

    isr_register(0x81, sched_on_int81h);

    process_t* idle_proc = proc_alloc(NULL, CTX_KERNEL, IDLECOMNAME);
    if (!idle_proc) panic("Failed to allocate process slot for idle thread", NULL);

    sched_add_thread(sched_idle, idle_proc);
}

static void sleepq_insert(thread_t* t) {
    t->sleep_next = NULL;

    if (!thread_sleep_head || t->wake_tick < thread_sleep_head->wake_tick) {
        t->sleep_next = thread_sleep_head;
        thread_sleep_head = t;
        return;
    }

    thread_t* it = thread_sleep_head;
    while (it->sleep_next && it->sleep_next->wake_tick <= t->wake_tick) {
        it = it->sleep_next;
    }
    t->sleep_next = it->sleep_next;
    it->sleep_next = t;
}

static void sleepq_remove(thread_t* t) {
    if (!thread_sleep_head || !t) return;

    if (thread_sleep_head == t) {
        thread_sleep_head = t->sleep_next;
        t->sleep_next = NULL;
        return;
    }

    thread_t* it = thread_sleep_head;
    while (it->sleep_next && it->sleep_next != t) {
        it = it->sleep_next;
    }
    if (it->sleep_next == t) {
        it->sleep_next = t->sleep_next;
        t->sleep_next = NULL;
    }
}

static thread_t* next_runnable(thread_t* start) {
    if (!thread_head) return NULL;
    thread_t* t = start ? start : thread_head;
    thread_t* begin = t;
    do {
        if (t->state == THREAD_RUNNABLE) return t;
        t = t->rq_next;
    } while (t && t != begin);
    return NULL;
}

thread_t* sched_add_thread(void (*entry)(void), process_t* proc) {
    if (!proc) return NULL;

    uint32_t flags = irq_save_disable();

    size_t slot;
    thread_t* t = alloc_thread_slot(&slot);
    if (!t) {
        irq_restore(flags);
        return NULL;
    }

    if (proc->main_thread == NULL) {
        proc->main_thread = t; // set main thread if not set
        proc->thread_list = t;
    } else {
        thread_t* it = proc->thread_list;
        while (it->threadlist_next) it = it->threadlist_next;
        it->threadlist_next = t;
    }
        
    proc->live_thread_count++;
    
    t->proc = proc;
    t->slot_inuse = true;
    t->slot_id = slot;
    t->state = THREAD_RUNNABLE;
    t->wake_tick = 0;
    t->exit_code = 0;
    t->entry = entry; // store real entry
    t->wait_kind = WAIT_NONE;
    t->wait_target_pid = 0;
    t->wait_owner = NULL;
    t->wait_next = NULL;
    t->rq_next = NULL;
    t->threadlist_next = NULL;

    pool_get_kstack(slot, (uint32_t*)&t->kstack_base, (uint32_t*)&t->kstack_top);

    build_initial_frame(t, thread_bootstrap);  // start at trampoline

    if (!thread_head) {
        thread_head = t;
        t->rq_next = t;   // circular
        // current stays NULL until first sched_do_switch
        irq_restore(flags);
        return t;
    }

    thread_t* tail = thread_head;
    while (tail->rq_next != thread_head) tail = tail->rq_next;
    tail->rq_next = t;
    t->rq_next = thread_head;

    irq_restore(flags);
    return t;
}

bool sched_clone_fork(process_t* parent, process_t* child, trap_frame_t* parent_tf) {
    uint32_t flags = irq_save_disable();
    
    if (parent == NULL || thread_current == NULL || 
        child == NULL || parent_tf == NULL)
    {
        irq_restore(flags);
        return false;
    }
    
    // Initialize new thread to run the same code as parent, but with cloned trap frame
    thread_t* child_thread = sched_add_thread(thread_current->entry, child);
    if (child_thread == NULL) {
        irq_restore(flags);
        return false;
    }
    
    // copy open file descriptors from parent to child
    child->fd_list = fde_fork_copy(parent->fd_list); // copy file descriptor list from parent to child
    child->fd_count = parent->fd_count; // copy fd count

    if (child->fd_list == NULL && parent->fd_list != NULL) {
        // Failed to copy file descriptors; clean up and return failure
        thread_reap(child_thread);

        irq_restore(flags);
        return false;
    }

    // copy parent's trap frame to child_thread's saved_esp frame area
    *((trap_frame_t*)child_thread->saved_esp) = *parent_tf;

    // Copy the user tail (ESP3 / SS3) that lives just beyond trap_frame_t.
    // build_initial_frame reserved space for these two dwords.  The parent's
    // trap frame was pushed by a ring-3 int $0x80, so the CPU saved the
    // caller's SS:ESP right after EFLAGS.  We must propagate them to the
    // child so its iret restores a valid user stack.
    trap_frame_t* child_tf = (trap_frame_t*)child_thread->saved_esp;
    *tf_user_esp_slot(child_tf) = *tf_user_esp_slot(parent_tf);
    *tf_user_ss_slot(child_tf)  = *tf_user_ss_slot(parent_tf);

    child_tf->eax = 0; // fork returns 0 in child
    parent_tf->eax = child->pid; // fork returns child's pid in parent
    
    irq_restore(flags);
    return true;
}

uint32_t sched_do_switch(uint32_t old_esp) {
    sched_pending = 0;

    if (thread_current) {
        thread_current->saved_esp = old_esp;
        if (thread_current->state == THREAD_RUNNING)
            thread_current->state = THREAD_RUNNABLE;
    }

    //sweep_dead();

    if (!thread_head) return old_esp;

    if (!thread_current) {
        thread_current = next_runnable(thread_head);
        if (thread_current) {
            thread_current->state = THREAD_RUNNING;
            sched_set_tss_esp0(thread_current);

            if (thread_current->proc->addr_space)
                mm_switch(thread_current->proc->addr_space);
        }
        return thread_current ? thread_current->saved_esp : old_esp;
    }

    thread_current = next_runnable(thread_current->rq_next);
    if (thread_current) {
        thread_current->state = THREAD_RUNNING;
        sched_set_tss_esp0(thread_current);
        if (thread_current->proc->addr_space)
            mm_switch(thread_current->proc->addr_space);
    }
    return thread_current ? thread_current->saved_esp : old_esp;
}

void sched_block_current_locked(wait_kind_t wait_kind, pid_t wait_target_pid) {
    if (!thread_current) return; // should not happen

    thread_current->state = THREAD_BLOCKED;
    thread_current->wait_kind = wait_kind;
    thread_current->wait_target_pid = wait_target_pid;
    thread_current->wait_owner = thread_current->proc;

    thread_current->wait_next = thread_current->proc->waiters;
    thread_current->proc->waiters = thread_current;
}

void sched_block_current(wait_kind_t wait_kind, pid_t wait_target_pid) {
    if (!thread_current) return; // should not happen

    uint32_t flags = irq_save_disable();

    sched_block_current_locked(wait_kind, wait_target_pid);

    irq_restore(flags);

    sched_thread_yield();
}

void sched_wake_thread(thread_t* t) {
    if (!t) return;

    uint32_t flags = irq_save_disable();

    if (t->state == THREAD_SLEEPING) {
        t->state = THREAD_RUNNABLE;
        sleepq_remove(t);
    } else if (t->state == THREAD_BLOCKED) {
        t->state = THREAD_RUNNABLE;

        process_t* owner = t->wait_owner;
        if (owner) {
            if (owner->waiters == t) {
                owner->waiters = t->wait_next;
            } else {
                thread_t* it = owner->waiters;
                while (it && it->wait_next != t) it = it->wait_next;
                if (it) it->wait_next = t->wait_next;
            }
        }

        t->wait_kind = WAIT_NONE;
        t->wait_target_pid = 0;
        t->wait_owner = NULL;
        t->wait_next = NULL;
    }

    irq_restore(flags);
}

void sched_proc_cleanup_fds(process_t* proc) {
    if (!proc) return;

    // This function is intended to be called at process exit. Each fd_entry is
    // process-owned, and each entry may hold a reference to a shared open_file.
    // Drop the open_file reference first, then free the process-owned fd node.

    fd_entry_t* fd_it = proc->fd_list;
    while (fd_it) {
        // Always snapshot next first because current fd_entry is process-owned
        // and is freed at the end of each iteration.
        fd_entry_t* current = fd_it;
        fd_it = fd_it->next;

        open_file_put(current->of);

        // FD entries themselves are process-owned and must always be freed
        // when the process exits, regardless of shared open-file lifetime.
        fdpool_free_fdentry(current);
    }

    proc->fd_list = NULL;
    proc->fd_count = 0;
}

void sched_thread_exit(int code) {
    if (!thread_current) return; // should not happen
    
    uint32_t flags = irq_save_disable();

    thread_current->exit_code = code;
    thread_current->state = THREAD_ZOMBIE;
    thread_current->proc->live_thread_count--;

    kdbg_puts("Thread ", 0x0C); 
    kdbg_hex32((uint32_t)thread_current, 0x0C); 
    kdbg_puts(" exited with code ", 0x0C); 
    kdbg_hex32(code, 0x0C); 
    kdbg_puts("\r\n", 0x0C);

    if (thread_current->proc->live_thread_count == 0) {
        // No threads left? Mark process as zombie so it can be reaped
        thread_current->proc->state = PROC_ZOMBIE;
        thread_current->proc->exit_code = code;
        wake_parent_waiter(thread_current->proc);
        sched_proc_cleanup_fds(thread_current->proc);
    } else if (thread_current->proc->live_thread_count > 0) {
        // If there are still threads left, we need to patch the thread list to remove this one
        thread_t* it = thread_current->proc->thread_list;
        thread_t* prev = NULL;
        while (it && it != thread_current) {
            prev = it;
            it = it->threadlist_next;   
        }

        // Found the thread, patch it out
        if (it == thread_current) {
            if (prev) {
                prev->threadlist_next = it->threadlist_next;
            } else {
                // if here, the exiting thread is the main thread, but there are still other threads in the process. 
                // in this case, we patch the head of the thread list to point to the next thread, and update the main_thread pointer
                thread_current->proc->main_thread = it->threadlist_next;
                thread_current->proc->thread_list = it->threadlist_next;
            }

            it->threadlist_next = NULL; // clean up exiting thread's next pointer
        } else {
            // should never happen, but if it does, it means a thread got unlinked
            // from its parent process's thread list somehow
            panic("Thread exit: current thread not found in its process's thread list", NULL);
        }
    }

    irq_restore(flags);

    sched_thread_yield();
    for (;;) __asm__ __volatile__ ("hlt");
}

void sched_thread_sleep(uint64_t ticks) {
    if (!thread_current) return;

    uint32_t flags = irq_save_disable();

    thread_current->wake_tick = get_ticks() + ticks;
    thread_current->state = THREAD_SLEEPING;
    sleepq_insert(thread_current);

    irq_restore(flags);

    sched_thread_yield(); // immediate deschedule
}

void sched_thread_yield() {
    if (!thread_current) return; // should not happen
    
    sched_pending = 1;
    __asm__ __volatile__("int $0x81");
}

void sched_on_tick(void) {
    uint64_t now = get_ticks();
    int woke_any = 0;

    // wake all threads whose deadline has passed
    while (thread_sleep_head && now >= thread_sleep_head->wake_tick) {
        thread_t* t = thread_sleep_head;
        thread_sleep_head = t->sleep_next;
        t->sleep_next = NULL;

        if (t->slot_inuse && t->state == THREAD_SLEEPING) {
            t->state = THREAD_RUNNABLE;
            woke_any = 1;
        }
    }

    if (--rr_ticks_left == 0) {
        rr_ticks_left = RR_SCHEDULER_CADENCE;
        sched_pending = 1;
    } else if (woke_any) {
        sched_pending = 1;
    }
}

static void thread_bootstrap(void) {
    void (*fn)(void) = (thread_current ? thread_current->entry : NULL);
    if (fn) fn();
    sched_thread_exit(0);
    for (;;) { __asm__ __volatile__("hlt"); }
}

void thread_kill_current(const char* reason, const size_t code) {
    kdbg_puts("Killing thread ", 0x0C); kdbg_hex32((uint32_t)thread_current, 0x0C);
    kdbg_puts(": ", 0x0C); kdbg_puts(reason, 0x0C);
    kdbg_puts(" (code ", 0x0C); kdbg_hex32(code, 0x0C); kdbg_puts(")\r\n", 0x0C);

    sched_thread_exit(code);
}

static inline void thread_unlink_from_runq(thread_t* t) {
    if (!t || !thread_head) return;

    if (thread_head == t) {
        if (t->rq_next == t) {
            thread_head = NULL;
        } else {
            thread_t* tail = thread_head;
            while (tail->rq_next != thread_head) tail = tail->rq_next;
            thread_head = t->rq_next;
            tail->rq_next = thread_head;
        }
        t->rq_next = NULL;
        return;
    }

    thread_t* it = thread_head;
    thread_t* begin = it;
    do {
        if (it->rq_next == t) break;
        it = it->rq_next;
    } while (it && it != begin);

    if (it->rq_next == t) {
        it->rq_next = t->rq_next;
        t->rq_next = NULL;
    }
}

static void wake_parent_waiter(process_t* child) {
    if (!child || !child->parent) return;

    process_t* parent = child->parent;
    thread_t* it = parent->waiters;
    while (it) {
        bool match_any = it->wait_kind == WAIT_ANY;
        bool match_pid = it->wait_kind == WAIT_CHILD && it->wait_target_pid == child->pid;
        if (match_any || match_pid) {
            sched_wake_thread(it);
            return;
        }
        it = it->wait_next;
    }
}

thread_t* sched_current_thread(void) {
    return thread_current;
}

process_t* sched_current_process(void) {
    return thread_current ? thread_current->proc : NULL;
}

static void sched_on_int81h(trap_frame_t* tf) {
    // Note: common ISR handler stub triggers the scheduler on return,
    // so this function is a no-op.
    (void)tf; // ignore trap frame
}

static void sched_idle() {
    /*for (;;) {
        __asm__ __volatile__("sti");
        __asm__ __volatile__("hlt");
    }*/
    for (;;) {
        kdbg_puts("Idle\r\n", 0x0B);
        for (volatile int i = 0; i < 400000000; i++); // burn cycles
    }
}

process_t* proc_alloc(process_t* parent, process_context_t context, const char* comm) {
    uint32_t flags = irq_save_disable();

    size_t slot;
    process_t* p = alloc_process_slot(&slot);
    if (!p) {
        irq_restore(flags);
        return NULL;
    }

    p->slot_inuse = true;
    p->slot_id = slot;
    p->pid = slot; // for simplicity, pid is just the slot number
    p->ppid = parent ? parent->pid : 0;
    p->state = PROC_ALIVE;
    p->exit_code = 0;
    p->live_thread_count = 0;
    p->fd_count = 0;
    p->fd_list = NULL;

    // set parentage, context, and image name
    p->parent = parent;
    p->context = parent ? parent->context : context;
    _sched_strncpy(p->comm, comm ? comm : "<anonymous>", MAXCOMLEN);    

    p->first_child = NULL;
    p->next_sibling = NULL;
    p->main_thread = NULL;
    p->thread_list = NULL;
    p->waiters = NULL;

    if (p->context != CTX_KERNEL) {
        p->addr_space = parent != NULL ? mm_clone_user_eager(parent->addr_space) : mm_create();
        if (!p->addr_space) {
            // Fail soft, return null because we were unable to create mm address space
            p->slot_inuse = false;
            irq_restore(flags);
            return NULL;
        }
    } else
        p->addr_space = NULL;

    if (parent) {
        // add to parent's child list
        if (!parent->first_child) {
            parent->first_child = p;
        } else {
            process_t* sibling = parent->first_child;
            while (sibling->next_sibling) sibling = sibling->next_sibling;
            sibling->next_sibling = p;
        }
    }

    // Bootstrap stdio once for root user processes. fork() children inherit
    // descriptors from the parent and should not get a second bootstrap list
    if (p->context != CTX_KERNEL && parent == NULL) {
        open_file_t* stdio = console_get_stdio();
        fd_entry_t* stdin_entry = fdpool_alloc_fdentry(stdio);
        if (stdin_entry && stdio) open_file_get(stdio);

        fd_entry_t* stdout_entry = fdpool_alloc_fdentry(stdio);
        if (stdout_entry && stdio) open_file_get(stdio);

        fd_entry_t* stderr_entry = fdpool_alloc_fdentry(stdio);
        if (stderr_entry && stdio) open_file_get(stdio);

        if (!stdin_entry || !stdout_entry || !stderr_entry) {
            // Fail soft if we can't allocate fd entries for stdio.
            if (stdin_entry) {
                open_file_put(stdin_entry->of);
                fdpool_free_fdentry(stdin_entry);
            }

            if (stdout_entry) {
                open_file_put(stdout_entry->of);
                fdpool_free_fdentry(stdout_entry);
            }

            if (stderr_entry) {
                open_file_put(stderr_entry->of);
                fdpool_free_fdentry(stderr_entry);
            }
        } else {
            // Build fd linked list.
            stdin_entry->next = stdout_entry;
            stdout_entry->next = stderr_entry;
            p->fd_list = stdin_entry;
            stdin_entry->local_id = STDIN_FILENO;
            stdout_entry->local_id = STDOUT_FILENO;
            stderr_entry->local_id = STDERR_FILENO;
            p->fd_count = 3;
        }
    }

    irq_restore(flags);
    return p;
}

open_file_t* sched_get_proc_local_fd(process_t* proc, int local_fd) {
    if (!proc) return NULL;

    fd_entry_t* it = proc->fd_list;
    while (it) {
        if (it->local_id == local_fd) return it->of;
        it = it->next;
    }
    return NULL;
}

void proc_free(process_t* p) {
    if (!p) return;

    uint32_t flags = irq_save_disable();

    // remove from parent's child list
    if (p->parent) {
        if (p->parent->first_child == p) {
            p->parent->first_child = p->next_sibling;
        } else {
            process_t* sibling = p->parent->first_child;
            while (sibling && sibling->next_sibling != p) sibling = sibling->next_sibling;
            if (sibling) sibling->next_sibling = p->next_sibling;
        }
    }

    if (p->addr_space) {
        mm_destroy(p->addr_space);
        p->addr_space = NULL;
    }

    // mark process slot as free
    p->slot_inuse = false;
    p->live_thread_count = 0;
    p->state = PROC_UNUSED;
    p->main_thread = NULL;
    p->thread_list = NULL;
    p->waiters = NULL;
    p->first_child = NULL;
    p->next_sibling = NULL;
    p->parent = NULL;

    irq_restore(flags);
}

process_t* proc_find(pid_t pid) {
    for (size_t i = 0; i < process_pool_count; i++) {
        if (process_pool[i].slot_inuse && process_pool[i].pid == pid) {
            return &process_pool[i];
        }
    }
    return NULL;    
}

void proc_add_child(process_t* parent, process_t* child) {
    if (!parent || !child) return;

    uint32_t flags = irq_save_disable();

    child->parent = parent;
    child->ppid = parent->pid;

    if (!parent->first_child) {
        parent->first_child = child;
    } else {
        process_t* sibling = parent->first_child;
        while (sibling->next_sibling) sibling = sibling->next_sibling;
        sibling->next_sibling = child;
    }

    irq_restore(flags);
}

void proc_remove_child(process_t* parent, process_t* child) {
    if (!parent || !child) return;

    uint32_t flags = irq_save_disable();

    if (parent->first_child == child) {
        parent->first_child = child->next_sibling;
    } else {
        process_t* sibling = parent->first_child;
        while (sibling && sibling->next_sibling != child) sibling = sibling->next_sibling;
        if (sibling) sibling->next_sibling = child->next_sibling;
    }

    child->parent = NULL;
    child->ppid = 0;

    irq_restore(flags);
}

process_t* proc_find_zombie_child(process_t* parent, pid_t child_pid) {
    if (!parent) return NULL;

    for (process_t* child = parent->first_child; child; child = child->next_sibling) {
        bool match_pid = (child_pid == (pid_t)-1) || (child->pid == child_pid);
        if (match_pid && child->state == PROC_ZOMBIE) {
            return child;
        }

        if (parent == child)
            panic("Circular parent-child relationship detected in proc_find_zombie_child", NULL);
    }
    return NULL;
}

bool proc_has_children(process_t* parent) {
    if (!parent) return false;
    return parent->first_child != NULL;
}

void thread_reap(thread_t* t) {
    if (!t) return;

    uint32_t flags = irq_save_disable();

    thread_unlink_from_runq(t);
    sleepq_remove(t);

    if (t->proc) {
        if (t->proc->thread_list == t) {
            t->proc->thread_list = t->threadlist_next;
        } else {
            thread_t* it = t->proc->thread_list;
            while (it && it->threadlist_next != t) it = it->threadlist_next;
            if (it) it->threadlist_next = t->threadlist_next;
        }

        if (t->proc->main_thread == t)
            t->proc->main_thread = t->proc->thread_list;
    }

    t->state = THREAD_UNUSED;
    t->slot_inuse = false;
    t->rq_next = NULL;
    t->sleep_next = NULL;
    t->entry = NULL;
    t->proc = NULL;
    t->threadlist_next = NULL;
    t->wait_kind = WAIT_NONE;
    t->wait_target_pid = 0;
    t->wait_owner = NULL;
    t->wait_next = NULL;

    irq_restore(flags);
}

void proc_reap_child(process_t* parent, process_t* child, reap_result_t* out_result) {
    if (!parent || !out_result) return;
    if (!child) {
        *out_result = REAP_INVALID_PID;
        return;
    }

    if (child->parent != parent) {
        *out_result = REAP_UNLINKED_CHILD;
        return;
    }

    if (child->state != PROC_ZOMBIE) {
        *out_result = REAP_NO_ZOMBIE;
        return;
    }

    // reap threads belonging to this process
    thread_t* t = child->thread_list;
    while (t) {
        thread_t* next = t->threadlist_next;
        thread_reap(t);
        t = next;
    }

    // valid zombie child found
    proc_free(child);
    *out_result = REAP_SUCCESS;
}

ssize_t proc_waitpid(process_t* parent, int32_t pid_filter, int* out_status) {
    if (!parent)
        return -ESRCH;
    
    pid_t match_pid = (pid_filter == -1) ? (pid_t)-1 : (pid_t)pid_filter;

    if (pid_filter < -1)
        return -EINVAL;

    uint32_t flags;
    for (;;) {
        flags = irq_save_disable();
        bool have_matching_child = false;

        if (match_pid == (pid_t)-1) {
            have_matching_child = proc_has_children(parent);
        } else {
            for (process_t* c = parent->first_child; c; c = c->next_sibling) {
                if (c->pid == match_pid) {
                    have_matching_child = true;
                    break;
                }
            }
        }

        process_t* zombie = proc_find_zombie_child(parent, match_pid);
        if (zombie) {
            int status = zombie->exit_code;
            pid_t reaped_pid = zombie->pid;
            reap_result_t rr = REAP_NO_ZOMBIE;
            proc_reap_child(parent, zombie, &rr);
            irq_restore(flags);

            if (rr != REAP_SUCCESS)
                return -ECHILD;

            if (out_status) *out_status = status;
            return (int32_t)reaped_pid;
        }

        if (!have_matching_child) {
            irq_restore(flags);
            return -ECHILD;
        }
    

        sched_block_current_locked((match_pid == (pid_t)-1) ? WAIT_ANY : WAIT_CHILD, match_pid);
        irq_restore(flags);
        sched_thread_yield();
    }
}

