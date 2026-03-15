
; This file assumes kernel is available at 0x00100000 and 
; is in protected mode with flat segmentation. It also assumes
; that the VGA text mode is active and can be used for output,
; and that macros/functions in prekinit_vga.asm are available.

bits 32
org 0x00100000

%define DUMP_OFF_EFLAGS   0
%define DUMP_OFF_EDI      4
%define DUMP_OFF_ESI      8
%define DUMP_OFF_EBP      12
%define DUMP_OFF_ESP      16
%define DUMP_OFF_EBX      20
%define DUMP_OFF_EDX      24
%define DUMP_OFF_ECX      28
%define DUMP_OFF_EAX      32
%define DUMP_OFF_RET_EIP  36   ; caller return address

debug:
.hexprint:
    ; Input: EAX=value to print as 8 hex characters (MS nibble first).
    ; Output: value is printed via vgatext.putch using attribute 0x0F.
    push eax                    ; preserve EAX, ECX, EDX
    push ecx
    push edx

    mov edx, eax                ; working copy to shift through nibbles
    mov ecx, 8                  ; 8 hex digits in a 32-bit register
._next_nibble:
    mov eax, edx                ; get working copy, copy to EAX
    shr eax, 28                 ; isolate current top nibble in AL
    call ._nibble_to_ascii      ; AL = '0'..'9' or 'A'..'F'
    mov ah, 0x0F                ; attribute byte, bright white on black
    call vgatext.putch          ; print the character in AL with attribute AH
    shl edx, 4                  ; bring next nibble into bits 31..28
    loop ._next_nibble          ; repeat until all nibbles complete

    pop edx                     ; restore registers and return
    pop ecx
    pop eax
    ret

._nibble_to_ascii:
    and al, 0x0F                ; isolate low nibble
    add al, '0'                 ; convert 0..9 to '0'..'9'
    cmp al, '9'                 ; if > '9', adjust into 'A'..'F'
    jbe ._hexprint_done         ; if not, jump to return
    add al, 7                   ; if so, adjust for 'A'..'F', 
                                ; then fallthrough to return
._hexprint_done:
    ret

; HEXDUMP HELPER MACROS
; These macros are used by the .dumpregisters function to print register values in hex.
%macro dump_reg 2           ; Input: %1=label for register name, %2=register value
    push esi                ; preserve ESI across puts/hexprint calls
    push eax
    push edx
    mov dl, 0x0F            ; bright white on black
    mov esi, %1             ; load address of "EAX=" (label passed as first arg)
    call vgatext.puts
    mov eax, %2             ; move the register value into EAX (register passed as second arg)
    call .hexprint
    mov esi, _debug_tab     ; print a tab separator
    call vgatext.puts
    pop edx
    pop eax
    pop esi                 ; restore ESI
%endmacro

%macro dump_seg_reg 2       ; Input: %1=label for register name, %2=segment register (e.g. cs)
    xor eax, eax            ; clear EAX
    mov ax, %2              ; move segment register value into AX (register passed as second arg)
    dump_reg %1, eax        ; dump contents of eax to show segment register value
%endmacro

%macro dump_cr_reg 2        ; Input: %1=label for register name, %2=control register (e.g. cr0)
    mov eax, %2             ; copy complete CR value to eax
    dump_reg %1, eax        ; dump contents of eax to show control register value
%endmacro

%macro dump_reg_nl 0        ; Print a newline after dumping a set of registers. \n only.
    push esi                ; preserve ESI across puts call
    mov esi, _debug_newline ; load address of newline string
    call vgatext.puts       ; print the newline
    pop esi                 ; restore ESI
%endmacro

.dumpregisters:
    pusha                   ; preserve register state and flags state
    pushfd
    mov ebp, esp            ; set EBP to point to saved registers on stack for convenient access
    
    mov esi, regdump        ; print "REGISTERS:" header
    mov dl, 0x0F            ; default attribute byte, bright white on black
    call vgatext.puts

    ; dump GP registers
    dump_reg reg_eax, [ebp + DUMP_OFF_EAX]
    dump_reg reg_ebx, [ebp + DUMP_OFF_EBX]
    dump_reg reg_ecx, [ebp + DUMP_OFF_ECX]
    dump_reg reg_edx, [ebp + DUMP_OFF_EDX]

    dump_reg_nl

    ; dump stack registers. Note that ESP/EBP are the values from before pusha/pushfd
    ; so they reflect the state at the time of the call to dumpregisters, not the state
    ; inside dumpregisters.
    dump_reg reg_esp, [ebp + DUMP_OFF_ESP]
    dump_reg reg_ebp, [ebp + DUMP_OFF_EBP]
    dump_reg reg_esi, [ebp + DUMP_OFF_ESI]
    dump_reg reg_edi, [ebp + DUMP_OFF_EDI]

    dump_reg_nl

    ; dump EIP and EFLAGS from before the call to dumpregisters. This is more useful than
    ; the EIP/FLAGS inside dumpregisters, which just point to the next instructions in the dump code.
    dump_reg reg_eip, [ebp + DUMP_OFF_RET_EIP]   ; caller return address
    dump_reg reg_flags, [ebp + DUMP_OFF_EFLAGS]
    dump_seg_reg reg_cs, cs
    dump_seg_reg reg_ds, ds

    dump_reg_nl

    ; dump segment registers
    dump_seg_reg reg_es, es
    dump_seg_reg reg_fs, fs
    dump_seg_reg reg_gs, gs
    dump_seg_reg reg_ss, ss

    dump_reg_nl

    ; dump control registers
    ; note that reading these requires ring0 privileges.
    dump_cr_reg reg_cr0, cr0
    dump_cr_reg reg_cr2, cr2
    dump_cr_reg reg_cr3, cr3

    popfd   ; restore register state and flag state, return
    popa
    ret

regdump         db 'REGISTERS:',10,0
reg_eax         db 'EAX=', 0
reg_ebx         db 'EBX=', 0
reg_ecx         db 'ECX=', 0
reg_edx         db 'EDX=', 0
reg_esp         db 'ESP=', 0
reg_ebp         db 'EBP=', 0
reg_esi         db 'ESI=', 0
reg_edi         db 'EDI=', 0
reg_eip         db 'EIP=', 0
reg_cs          db ' CS=', 0
reg_ds          db ' DS=', 0
reg_es          db ' ES=', 0
reg_fs          db ' FS=', 0
reg_gs          db ' GS=', 0 
reg_ss          db ' SS=', 0
reg_cr0         db 'CR0=', 0
reg_cr2         db 'CR2=', 0
reg_cr3         db 'CR3=', 0
reg_flags       db 'FLG=', 0
_debug_tab      db 9, 0
_debug_newline  db 10,0