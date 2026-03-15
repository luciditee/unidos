bits 32
org 0x00100000

start:
    cli
    
    ; Save pointer to kparam/length
    mov [kparam_length], cx
    mov [kparam_ptr], esi

    jmp kernel

%include "prekinit_constants.asm"
%include "prekinit_vga.asm"
%include "prekinit_debug.asm"
%include "prekinit_gdt_tss.asm"

; Assumptions made at entry:
; - ESI points to kparams (or is NULL)
; - The stack is set up and can be used by the kernel
; - The CPU is in protected mode with flat segmentation (but needs new GDT/IDT)
; - The VGA text mode is active and can be used for output

kernel:
    call gdt_tss_init
    call vgatext.cls
    
    mov dl, 1
    mov cl, 32
.testprint:
    ;mov esi, teststr
    call vgatext.puts ; kparams should be pointed to by ESI (or NULL)
    inc dl
    and dx, 0x00FF
    dec cl
    jnz .testprint

    mov eax, newline
    mov esi, eax
    call vgatext.puts

    mov eax, 0xDEADBEEF
    call debug.dumpregisters

.halt:
    hlt
    jmp .halt

teststr db 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789', 0
newline db 13,10,0
kparam_length dw 0
kparam_ptr dd 0

; Temporary buffer padding
; This will be removed as kernel grows. We are trying to ensure that
; the stage2 loader can safely handle loading a multiple-sector-long
; kernel image. When the kernel breaks past this limit, this padding
; will be removed as it no longer serves a purpose.
times (512*10)-($-$$) db 0
