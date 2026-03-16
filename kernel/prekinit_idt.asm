
bits 32

%include "prekinit_constants.asm"

global idt_init
global isr_common_entry

extern vgatext.puts
extern debug.dumpregisters

section .text

; Build one IDT gate (interrupt gate, 32-bit, present, DPL=0)
; in: eax = handler address, edi = &idt[n]
%macro IDT_SET_GATE 0
    mov word [edi + 0], ax                 ; offset[15:0]
    mov word [edi + 2], GDT_SEL_KCODE      ; selector
    mov byte [edi + 4], 0                  ; zero
    mov byte [edi + 5], 10001110b          ; P=1,DPL=0,32-bit interrupt gate
    shr eax, 16
    mov word [edi + 6], ax                 ; offset[31:16]
%endmacro

idt_init:
    pushad
    cld

    xor ebx, ebx
.init_loop:
    mov eax, [isr_stub_table + ebx*4]
    lea edi, [idt_table + ebx*8]
    IDT_SET_GATE
    inc ebx
    cmp ebx, 256
    jl .init_loop

    lidt [idtr]
    popad
    ret

; Common entry for all vectors.
; Stack on entry (normalized):
;   [esp+0]  = vector
;   [esp+4]  = error code (real or synthetic 0)
;   [esp+8]  = eip
;   [esp+12] = cs
;   [esp+16] = eflags
isr_common_entry:
    cld
    mov eax, cr2
    push eax
    push gs
    push fs
    push es
    push ds
    pushad

    mov ax, GDT_SEL_KDATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov esi, isr_fatal_msg
    mov dl, 0x0C                ; bright red on black   
    call vgatext.puts

    lea ebp, [esp + 52]      ; normalized frame base
    ; [ebp+0]  vector
    ; [ebp+4]  error
    ; [ebp+8]  eip
    ; [ebp+12] cs
    ; [ebp+16] eflags
    ; [esp+48] cr2

    ; Basic diagnostic dump
    mov eax, [ebp + 0]       ; vector
    mov edx, [ebp + 4]       ; error
    call debug.dumpregisters
    mov esi, _debug_newline
    call vgatext.puts

.halt:
    cli
    hlt
    jmp .halt

section .data
align 8
idt_table:
    times 256 dq 0

idtr:
    dw (256*8 - 1)
    dd idt_table

isr_fatal_msg db 10,'FATAL EXCEPTION ',10,0