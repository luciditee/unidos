bits 32
org 0x00100000

start:
    cli
    jmp kernel

%include "prekinit_vga.asm"

kernel:
    call vgatext.cls
    mov dl, 1
    mov cl, 5
.testprint:
    ;mov esi, teststr
    call vgatext.puts ; kparams should be pointed to by ESI (or NULL)
    inc dl
    and dx, 0x00FF
    dec cl
    jnz .testprint

.halt:
    hlt
    jmp .halt

teststr db 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789', 0

; Temporary buffer padding
; This will be removed as kernel grows. We are trying to ensure that
; the stage2 loader can safely handle loading a multiple-sector-long
; kernel image. When the kernel breaks past this limit, this padding
; will be removed as it no longer serves a purpose.
times (512*10)-($-$$) db 0
