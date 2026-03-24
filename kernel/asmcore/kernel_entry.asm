bits 32

global start
extern kmain
extern __bss_start
extern __kernel_end

section .text.start

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
%include "prekinit_idt.asm"
%include "prekinit_isr_stubs.asm"

section .text

; Assumptions made at entry:
; - ESI points to kparams (or is NULL)
; - The stack is set up and can be used by the kernel
; - The CPU is in protected mode with flat segmentation (but needs new GDT/IDT)
; - The VGA text mode is active and can be used for output

kernel:
    call gdt_tss_init   ; GDT and TSS *must* exist before we can do anything else
    call idt_init       ; IDT must also exist, but handlers won't be installed until later
    call vgatext.cls    ; Clear screen. Partly serves as a debugging checkpoint.
    call .kernel_bss_zero ; Zero out the .bss section before we do anything else, to 
                          ; ensure global/static variables are zero-initialized as expected.
                        ; Deliberate fallthrough to maskpic and kmain call
.maskpic:
    ; Mask PIC while early C bootstrap is brought up.
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al

    ; Print entering kmain message
    mov esi, kentry_verstr
    mov dl, 0x0E
    call vgatext.puts
    ; Fallthrough to kmain
    ; cdecl call: kmain(uint32_t kparam_ptr, uint32_t kparam_length)
.run_kmain:
    movzx eax, word [kparam_length]
    push eax
    push dword [kparam_ptr]
    call kmain
    add esp, 8
.kmain_returned:
    mov esi, kmain_returned
    mov dl, 0x0C
    call vgatext.puts

.halt:
    hlt
    jmp .halt

.kernel_bss_zero:
    ; Zero out the .bss section (uninitialized global/static variables)
    push edi
    push ecx
    push eax
    mov edi, __bss_start
    mov ecx, __kernel_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb
    pop eax
    pop ecx
    pop edi
    ret

section .rodata
kentry_verstr db UNIDOS_VERSION,13,10,0
kmain_returned db 13,10,'fatal: kmain returned unexpectedly',13,10,0

section .data
kparam_length dw 0
kparam_ptr dd 0