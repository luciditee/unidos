bits 32

; Expose symbols for syscall test runner:
; These can be used from C e.g. extern userprog_test to get the address
; of the test function for IRET to jump to
global userprog_test
global userprog_end

align 4096
section .rodata

userprog_test:
    ;mov eax, 4
    ;mov esi, .testdata
    ;mov edx, 19
    ;mov ebx, 1
    ;int 0x80
    ;mov ebx, eax
    ;mov eax, 1
    ;int 0x80
    ;jmp $
    incbin "usermode_test.bin"

    ;.testdata db "hello from usermode!", 0
userprog_end:

times 4096-($-$$) db 0
