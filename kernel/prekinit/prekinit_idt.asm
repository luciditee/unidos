
bits 32

%include "prekinit_constants.asm"

global idt_init
global isr_common_entry
global idt_table
global idtr
extern sched_pending
extern sched_do_switch

extern isr_dispatch

section .text

; Build one IDT gate (interrupt gate, 32-bit, present, DPL=0)
; in: eax = handler address, edi = &idt[n]
%macro IDT_SET_GATE 1
    mov word [edi + 0], ax                 ; offset[15:0]
    mov word [edi + 2], GDT_SEL_KCODE      ; selector
    mov byte [edi + 4], 0                  ; zero
    mov byte [edi + 5], %1
    shr eax, 16
    mov word [edi + 6], ax                 ; offset[31:16]
%endmacro

idt_init:
    pushad  ; save all general-purpose registers
    cld     ; clear direction flag to ensure string ops increment

    xor ebx, ebx    ; ebx is our vector index, start at 0
.init_loop:         ; for each IDT entry...
    mov eax, [isr_stub_table + ebx*4]   ; get the handler address for this vector
    lea edi, [idt_table + ebx*8]        ; get the address of the current IDT entry (8 bytes each)
    IDT_SET_GATE 10001110b             ; P=1,DPL=0,32-bit interrupt gate
    inc ebx                             ; next vector
    cmp ebx, 128                        ; 128 is a special exception, so we jump if we're there
    je .init_syscall
    cmp ebx, 256                        ; loop until all 256 vectors are set up
    jl .init_loop
    jmp .loadidt
.init_syscall:
    ; Syscall gate is vector 0x80 (128), DPL=3 so user code can invoke it.
    ; POSSIBLE TODO: Patch IDT in C code instead of hardcoding here. At the moment,
    ; this is the only DPL3 gate we have, so this is what we will use for now
    mov eax, [isr_stub_table + 128*4]   ; get syscall handler address
    lea edi, [idt_table + 128*8]        ; get address of IDT entry for vector 128
    IDT_SET_GATE 11101110b             ; P=1,DPL=3,32-bit interrupt gate
    inc ebx
    jmp .init_loop

.loadidt:
    lidt [idtr]     ; load the IDT with this new table
    popad           ; restore GP registers and return
    ret

; Common entry for all vectors.
; Stack on entry (normalized):
;   [esp+0]  = vector
;   [esp+4]  = error code (real or synthetic 0)
;   [esp+8]  = eip
;   [esp+12] = cs
;   [esp+16] = eflags
isr_common_entry:
    cld             ; set direction flag 0/fwd
                    ; note: this happens throughout kernel code and is because
                    ; string ops are used everywhere, and we cannot be certain of
                    ; the state of EFLAGS.

    mov eax, cr2    ; copy CR2 to eax (later useful for #PF)
    push eax        ; save CR2 on stack for isr_dispatch
    push gs         ; note: all stack manipulation here and at the end of the common entry
    push fs         ; are to match the trap_frame_t struct exactly (in reverse order, because
    push es         ; that's how x86 stacks work)
    push ds
    pushad          ; push general-purpose registers (also for trap_frame) but not eflags

    mov ax, GDT_SEL_KDATA ; load kernel data segment selector into AX
    mov ds, ax      ; all segment registers should point here for isr_dispatch
    mov es, ax      ; since we use a flat segmentation model
    mov fs, ax
    mov gs, ax

    ; pass trap_frame_t* (ESP points at trap_frame.edi)
    push esp            ; push pointer to trap frame as argument
    call isr_dispatch   ; call isr_dispatch to handle interrupt in C
    add esp, 4          ; clean up argument

    cmp byte [sched_pending], 0
    je .no_switch
    push esp                  ; old_esp (points at edi slot)
    call sched_do_switch      ; eax = new task saved_esp
    add esp, 4
    mov esp, eax              ; commit context switch
.no_switch:

    ; restore context
    popad               ; restore GP registers
    pop ds              ; restore segment registers in reverse order
    pop es
    pop fs
    pop gs
    add esp, 4      ; discard saved cr2
    add esp, 8      ; discard normalized vector + error
    iretd           ; return from interrupt, restoring EIP, CS from stack

.halt:
    cli
    hlt
    jmp .halt

section .data
align 8
idt_table:
    times 256 dq 0  ; 256 entries, 8 bytes each, initialized to zero

idtr:               ; IDT register structure for lidt instruction
    dw (256*8 - 1)  ; limit = size
    dd idt_table    ; pointer to IDT base
