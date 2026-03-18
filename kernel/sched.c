
#include "include/sched.h"
#include "include/kmain.h"

static task_t* current = NULL;
static task_t* head = NULL;
volatile uint8_t sched_pending = 0;

static task_t list[MAX_THREADS] = {0};

static task_t* alloc_slot(void) {
    for (size_t i = 0; i < MAX_THREADS; i++) {
        if (list[i].in_use == TASK_UNUSED) return &list[i];
    }
    return NULL;
}

/*
 * Matches ISR restore order exactly.
 */
static void build_initial_frame(task_t* t, void (*entry)(void)) {
    uint32_t* sp = (uint32_t*)(t->stack + TASK_STACK_SIZE);

    #define PUSH(v) (*--sp = (uint32_t)(v))

    // Note: These must match the exact order of pushes in isr_common_entry,
    // and consequently, the exact layout of trap_frame_t.
    PUSH(0x00000202u);           // EFLAGS (IF=1)
    PUSH(GDT_SEL_KCODE);         // CS
    PUSH((uint32_t)entry);       // EIP
    PUSH(0);                     // error code (discarded)
    PUSH(0);                     // vector (discarded)
    PUSH(0);                     // cr2 (discarded)

    PUSH(GDT_SEL_KDATA);         // gs
    PUSH(GDT_SEL_KDATA);         // fs
    PUSH(GDT_SEL_KDATA);         // es
    PUSH(GDT_SEL_KDATA);         // ds

    PUSH(0); // eax
    PUSH(0); // ecx
    PUSH(0); // edx
    PUSH(0); // ebx
    PUSH(0); // esp dummy for popad
    PUSH(0); // ebp
    PUSH(0); // esi
    PUSH(0); // edi   <- restore starts here (popad)

    #undef PUSH

    t->saved_esp = (uint32_t)sp;
}

void sched_init(void) {
    for (size_t i = 0; i < MAX_THREADS; i++) {
        list[i].in_use = TASK_UNUSED;
        list[i].next = NULL;
        list[i].saved_esp = 0;
    }
    head = NULL;
    current = NULL;
    sched_pending = 0;
}

task_t* sched_add_task(void (*entry)(void)) {
    task_t* t = alloc_slot();
    if (!t) return NULL;

    t->in_use = TASK_IN_USE;
    t->next = NULL;
    build_initial_frame(t, entry);

    if (!head) {
        head = t;
        t->next = t;   // circular
        // current stays NULL until first sched_do_switch
        return t;
    }

    task_t* tail = head;
    while (tail->next != head) tail = tail->next;
    tail->next = t;
    t->next = head;
    return t;
}

uint32_t sched_do_switch(uint32_t old_esp) {
    if (!head) {
        sched_pending = 0;
        return old_esp;
    }

    if (!current) {
        current = head;          // first activation
        sched_pending = 0;
        return current->saved_esp;
    }

    kdbg_puts(" ", 0x00);
    current->saved_esp = old_esp;
    current = current->next;     // circular invariant
    sched_pending = 0;
    return current->saved_esp;
}
