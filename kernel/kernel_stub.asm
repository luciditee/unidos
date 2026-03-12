bits 32
org 0x00100000

%define VGA_TEXT_BUFFER 0xB8000
%define VGA_ATTR        0x0F        ; bright white on black

start:
    cli
    ;mov word [0xB8000], 0x0F4B ; 'K'
    ;jmp .halt
    mov esi, kernel_msg
    mov edi, VGA_TEXT_BUFFER
    call print_string_pm32

.halt:
    hlt
    jmp .halt

print_string_pm32:
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

kernel_msg db 'Kernel reached (PM32)', 0

times (512*10)-($-$$) db 0
