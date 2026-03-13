bits 32
org 0x00100000

%define VGA_TEXT_BUFFER 0xB8000
%define VGA_ATTR        0x0F        ; bright white on black

start:
    cli
    ;mov word [0xB8000], 0x0F4B ; 'K'
    ;jmp .halt
    ;mov esi, kernel_msg
    mov edi, VGA_TEXT_BUFFER
    call utils.early_print

.halt:
    hlt
    jmp .halt

utils:
.early_print:
.next:
    lodsb
    test al, al
    jz .done
    mov ah, VGA_ATTR
    mov [edi], ax
    add edi, 2
    jmp .next
.done:
    ret

; Temporary buffer padding
; This will be removed as kernel grows. We are trying to ensure that
; the stage2 loader can safely handle loading a multiple-sector-long
; kernel image. When the kernel breaks past this limit, this padding
; will be removed as it no longer serves a purpose.
times (512*10)-($-$$) db 0
