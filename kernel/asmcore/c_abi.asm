bits 32

global kdbg_puts
global kdbg_putsn
global kdbg_hex32
global kdbg_get_cursor
global kdbg_dump_current
global kdbg_dump_frame

extern vgatext.puts
extern vgatext.putsn
; extern vgatext.set_cursor_linear
extern vgatext.get_cursor_eax
extern debug.hexprint
extern debug.dump_current
extern debug.dump_frame

; Prologue and epilogue macros for C-callable functions
; These can be omitted in cases where the callee takes no arguments
%macro cprologue 0  ; Set up frame for C function calling asm.
    push ebp        ; C expects EBP preserved
    mov ebp, esp    ; set EBP to point to args on stack
%endmacro 

%macro cepilogue 0  ; Clean up frame for C function calling asm.
    pop ebp         ; restore EBP, return
    ret
%endmacro

; Preserve callee-saved GPRs required by i386 SysV ABI.
; Caller-saved regs (EAX/ECX/EDX) may be clobbered by callees.
%macro save_callee_saved 0
    push ebx
    push esi
    push edi
%endmacro

%macro restore_callee_saved 0
    pop edi
    pop esi
    pop ebx
%endmacro

section .text

; void kdbg_puts(const char* s, uint32_t attr);
kdbg_puts:
    cprologue
    save_callee_saved
    pushfd
    cli

    mov esi, [ebp + 8]       ; string
    mov edx, [ebp + 12]      ; attribute byte
    call vgatext.puts

    popfd
    restore_callee_saved
    cepilogue

; void kdbg_putsn(const char* s, uint32_t attr, uint32_t len);
kdbg_putsn:
    cprologue
    save_callee_saved
    pushfd
    cli

    mov esi, [ebp + 8]       ; string
    mov edx, [ebp + 12]      ; attribute byte
    mov ecx, [ebp + 16]      ; length
    call vgatext.putsn

    popfd
    restore_callee_saved
    cepilogue

; void kdbg_hex32(uint32_t value, uint32_t attr);
kdbg_hex32:
    cprologue
    save_callee_saved

    mov eax, [ebp + 8]       ; value
    mov edx, [ebp + 12]      ; attr
    call debug.hexprint

    restore_callee_saved
    cepilogue

; uint32_t kdbg_get_cursor(void);
kdbg_get_cursor:
    ; no args, no locals
    save_callee_saved
    call vgatext.get_cursor_eax
    restore_callee_saved
    ; EAX already holds return value
    ret

; void kdbg_dump_current(void)
kdbg_dump_current:
    save_callee_saved
    call debug.dump_current
    restore_callee_saved
    ret

; void kdbg_dump_frame(const void* frame)
kdbg_dump_frame:
    cprologue
    save_callee_saved
    
    mov esi, [ebp + 8]
    call debug.dump_frame

    restore_callee_saved
    cepilogue

