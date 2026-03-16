
bits 32

global isr_stub_table
extern isr_common_entry

section .text

%macro ISR_NOERR 1
isr_stub_%1:
    push dword 0            ; synthetic error code
    push dword %1           ; vector
    jmp isr_common_entry
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push dword %1           ; vector (CPU already pushed error code)
    jmp isr_common_entry
%endmacro

%assign i 0
%rep 256
    ; CPU error-code vectors (32-bit protected mode)
    %if (i=8) || (i=10) || (i=11) || (i=12) || (i=13) || (i=14) || (i=17)
        ISR_ERR i
    %else
        ISR_NOERR i
    %endif
%assign i i+1
%endrep

section .rodata
align 4
isr_stub_table:
%assign j 0
%rep 256
    dd isr_stub_%+j
%assign j j+1
%endrep