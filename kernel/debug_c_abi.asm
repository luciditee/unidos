bits 32

global kdbg_puts
global kdbg_hex32
global kdbg_get_cursor
global kdbg_dump_current
global kdbg_dump_frame

extern vgatext.puts
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

%macro with_saved_esi_begin 0  ; Save ESI across a block of code, for use in puts/hexprint calls.
    push esi
%endmacro

%macro with_saved_esi_end 0
    pop esi
%endmacro

section .text

; void kdbg_puts(const char* s, uint32_t attr);
kdbg_puts:
    cprologue
    with_saved_esi_begin
    push edx        ; preserve EDX across puts call

    mov esi, [ebp + 8]       ; string
    mov edx, [ebp + 12]      ; attribute byte
    call vgatext.puts

    pop edx     ; restore EDX, ESI, EBP, return
    with_saved_esi_end
    cepilogue

; void kdbg_hex32(uint32_t value, uint32_t attr);
kdbg_hex32:
    cprologue
    push edx    ; preserve EDX across hexprint call

    mov eax, [ebp + 8]       ; value
    mov edx, [ebp + 12]      ; attr
    call debug.hexprint

    pop edx
    cepilogue

; uint32_t kdbg_get_cursor(void);
kdbg_get_cursor:
    ; no args, no locals
    call vgatext.get_cursor_eax
    ; EAX already holds return value
    ret

; void kdbg_dump_current(void)
kdbg_dump_current:
    call debug.dump_current
    ret

; void kdbg_dump_frame(const void* frame)
kdbg_dump_frame:
    cprologue
    with_saved_esi_begin
    
    mov esi, [ebp + 8]
    call debug.dump_frame

    with_saved_esi_end
    cepilogue

