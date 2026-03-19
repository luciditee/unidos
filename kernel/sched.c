
#include "include/sched.h"
#include "include/kmain.h"
#include "include/pit.h"
#include "include/isr.h"
#include "io.h"

#define RR_SCHEDULER_CADENCE 10 // every N ticks
#define U64_MAX 0xFFFFFFFFFFFFFFFFULL

static uint32_t rr_ticks_left = RR_SCHEDULER_CADENCE;
static task_t* current = NULL;
static task_t* head = NULL;
static task_t* sleep_head = NULL;
volatile uint8_t sched_pending = 0;
volatile uint64_t last_sched_tick = 0;

static task_t list[MAX_THREADS] = {0};

static void task_bootstrap(void);
static void sched_on_int80h(trap_frame_t* tf);
static void sched_idle(void);

static task_t* alloc_slot(void) {
    for (size_t i = 0; i < MAX_THREADS; i++) {
        if (list[i].in_use == TASK_UNUSED) return &list[i];
    }
    return NULL;
}

/*
 * Matches ISR restore order exactly.
 */
static void build_initial_frame(task_t* t, void (*start_eip)(void)) {
    uint32_t* sp = (uint32_t*)(t->stack + TASK_STACK_SIZE);

    #define PUSH(v) (*--sp = (uint32_t)(v))

    // Note: These must match the exact order of pushes in isr_common_entry,
    // and consequently, the exact layout of trap_frame_t.
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
    for (size_t i = 0; i < MAX_THREADS; i++) {
        list[i].in_use = TASK_UNUSED;
        list[i].state = INVALID;
        list[i].next = NULL;
        list[i].sleep_next = NULL;
        list[i].saved_esp = 0;
        list[i].wake_tick = 0;
        list[i].return_code = 0;
    }
    head = NULL;
    current = NULL;
    sched_pending = 0;
    rr_ticks_left = RR_SCHEDULER_CADENCE;

    isr_register(0x80, sched_on_int80h);
    sched_add_task(sched_idle);
}

static void sleepq_insert(task_t* t) {
    t->sleep_next = NULL;

    if (!sleep_head || t->wake_tick < sleep_head->wake_tick) {
        t->sleep_next = sleep_head;
        sleep_head = t;
        return;
    }

    task_t* it = sleep_head;
    while (it->sleep_next && it->sleep_next->wake_tick <= t->wake_tick) {
        it = it->sleep_next;
    }
    t->sleep_next = it->sleep_next;
    it->sleep_next = t;
}

static void sleepq_remove(task_t* t) {
    if (!sleep_head || !t) return;

    if (sleep_head == t) {
        sleep_head = t->sleep_next;
        t->sleep_next = NULL;
        return;
    }

    task_t* it = sleep_head;
    while (it->sleep_next && it->sleep_next != t) {
        it = it->sleep_next;
    }
    if (it->sleep_next == t) {
        it->sleep_next = t->sleep_next;
        t->sleep_next = NULL;
    }
}

static void sweep_dead(void) {
    if (!head) return;

    task_t* tail = head;
    while (tail->next != head) tail = tail->next;

    task_t* prev = tail;
    task_t* t = head;

    do {
        task_t* next = t->next;

        if (t->state == DEAD) {
            sleepq_remove(t);   // important: dead task might still be in sleep queue
            prev->next = next;

            if (t == head) {
                head = (next == t) ? NULL : next;
            }
            if (t == current) {
                current = NULL;
            }

            t->next = NULL;
            t->sleep_next = NULL;
            t->state = INVALID;
            t->in_use = TASK_UNUSED;

            if (!head) return;
            t = next;
            continue;
        }

        prev = t;
        t = next;
    } while (t != head);
}

static task_t* next_runnable(task_t* start) {
    if (!head) return NULL;
    task_t* t = start ? start : head;
    task_t* begin = t;
    do {
        if (t->state == RUNNABLE) return t;
        t = t->next;
    } while (t && t != begin);
    return NULL;
}

task_t* sched_add_task(void (*entry)(void)) {
    uint32_t flags = irq_save_disable();

    task_t* t = alloc_slot();
    if (!t) {
        irq_restore(flags);
        return NULL;
    }

    t->in_use = TASK_IN_USE;
    t->state = RUNNABLE;
    t->wake_tick = 0;
    t->return_code = 0;
    t->entry = entry; // store real entry
    t->next = NULL;

    build_initial_frame(t, task_bootstrap);  // start at trampoline

    if (!head) {
        head = t;
        t->next = t;   // circular
        // current stays NULL until first sched_do_switch
        irq_restore(flags);
        return t;
    }

    task_t* tail = head;
    while (tail->next != head) tail = tail->next;
    tail->next = t;
    t->next = head;

    irq_restore(flags);
    return t;
}

static int stack_corrupt(task_t* t, uint32_t esp) {
    uintptr_t lo = (uintptr_t)&t->stack[0];
    uintptr_t hi = (uintptr_t)&t->stack[TASK_STACK_SIZE];
    uintptr_t guard = lo + STACK_CANARY;   // red zone at bottom (downward-growing stack)

    uintptr_t p = (uintptr_t)esp;
    if (p < guard || p > hi) return 1;
    return 0;
}

uint32_t sched_do_switch(uint32_t old_esp) {
    sched_pending = 0;

    if (current) {
        if (stack_corrupt(current, old_esp)) {
            current->state = DEAD;
        } else
            current->saved_esp = old_esp;
    }

    sweep_dead();

    if (!head) return old_esp;

    if (!current) {
        current = next_runnable(head);
        return current ? current->saved_esp : old_esp;
    }

    current = next_runnable(current->next);
    return current ? current->saved_esp : old_esp;
}

void sched_task_exit(int code) {
    if (!current) return; // should not happen
    
    uint32_t flags = irq_save_disable();

    current->return_code = code;
    current->state = DEAD;

    irq_restore(flags);

    sched_task_yield();
    for (;;) __asm__ __volatile__ ("hlt");
}

void sched_task_sleep(uint64_t ticks) {
    if (!current) return;

    uint32_t flags = irq_save_disable();

    current->wake_tick = get_ticks() + ticks;
    current->state = SLEEPING;
    sleepq_insert(current);

    irq_restore(flags);

    sched_task_yield(); // immediate deschedule
}

void sched_task_yield() {
    if (!current) return; // should not happen
    
    sched_pending = 1;
    __asm__ __volatile__("int $0x80");
}

void sched_on_tick(void) {
    uint64_t now = get_ticks();
    int woke_any = 0;

    // wake all tasks whose deadline has passed
    while (sleep_head && now >= sleep_head->wake_tick) {
        task_t* t = sleep_head;
        sleep_head = t->sleep_next;
        t->sleep_next = NULL;

        if (t->in_use == TASK_IN_USE && t->state == SLEEPING) {
            t->state = RUNNABLE;
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

static void task_bootstrap(void) {
    void (*fn)(void) = (current ? current->entry : NULL);
    if (fn) fn();
    sched_task_exit(0);
    for (;;) { __asm__ __volatile__("hlt"); }
}

static void sched_on_int80h(trap_frame_t* tf) {
    // Note: common ISR handler stub triggers the scheduler on return,
    // so this function is a no-op.
    (void)tf; // ignore trap frame
}

static void sched_idle() {
    for (;;) {
        __asm__ __volatile__("sti");
        __asm__ __volatile__("hlt");
    }
}
