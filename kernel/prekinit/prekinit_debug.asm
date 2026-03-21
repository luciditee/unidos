
; This file assumes kernel is available at 0x00100000 and 
; is in protected mode with flat segmentation. It also assumes
; that the VGA text mode is active and can be used for output,
; and that macros/functions in prekinit_vga.asm are available.

bits 32

global debug.hexprint
global debug.dumpregisters
global debug.dump_current
global debug.dump_frame

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

; Offsets for debug.dump_current snapshot (after pushfd + pushad):
;   [base+00] EDI
;   [base+04] ESI
;   [base+08] EBP
;   [base+12] ESP (pre-pushad value)
;   [base+16] EBX
;   [base+20] EDX
;   [base+24] ECX
;   [base+28] EAX
;   [base+32] EFLAGS (from pushfd)
%define SNAP_EDI     0
%define SNAP_ESI     4
%define SNAP_EBP     8
%define SNAP_ESP     12
%define SNAP_EBX     16
%define SNAP_EDX     20
%define SNAP_ECX     24
%define SNAP_EAX     28
%define SNAP_EFLAGS  32

; Offsets for debug.dump_frame input (ESI -> frame):
;   [base+00] vector
;   [base+04] error
;   [base+08] eip
;   [base+12] cs
;   [base+16] eflags
%define FR_VEC       0      ; base+0 = vector number (0-255)
%define FR_ERR       4      ; base+4 = error code (real or synthetic)
%define FR_EIP       8      ; base+8 = EIP at time of interrupt (or syscall)
%define FR_CS        12     ; base+12 = CS at time of interrupt (or syscall)
%define FR_EFLAGS    16     ; base+16 = EFLAGS at time of interrupt (or syscall)

%define DBG_ATTR     0x0F   ; VGA attribute byte for printing

%macro DBG_PUTS 1
    mov edx, DBG_ATTR
    mov esi, %1
    call vgatext.puts
%endmacro

%macro DBG_LABEL_HEX 2
    ; %1 = label string pointer
    ; %2 = dword offset from frame base in EBX
    DBG_PUTS %1
    mov eax, [ebx + %2]
    mov edx, DBG_ATTR
    call debug.hexprint
    DBG_PUTS _debug_tab
%endmacro

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
    
    mov esi, _regdump        ; print "REGISTERS:" header
    mov dl, 0x0F            ; default attribute byte, bright white on black
    call vgatext.puts

    ; dump GP registers
    dump_reg _reg_eax, [ebp + DUMP_OFF_EAX]
    dump_reg _reg_ebx, [ebp + DUMP_OFF_EBX]
    dump_reg _reg_ecx, [ebp + DUMP_OFF_ECX]
    dump_reg _reg_edx, [ebp + DUMP_OFF_EDX]

    dump_reg_nl

    ; dump stack registers. Note that ESP/EBP are the values from before pusha/pushfd
    ; so they reflect the state at the time of the call to dumpregisters, not the state
    ; inside dumpregisters.
    dump_reg _reg_esp, [ebp + DUMP_OFF_ESP]
    dump_reg _reg_ebp, [ebp + DUMP_OFF_EBP]
    dump_reg _reg_esi, [ebp + DUMP_OFF_ESI]
    dump_reg _reg_edi, [ebp + DUMP_OFF_EDI]

    dump_reg_nl

    ; dump EIP and EFLAGS from before the call to dumpregisters. This is more useful than
    ; the EIP/FLAGS inside dumpregisters, which just point to the next instructions in the dump code.
    dump_reg _reg_eip, [ebp + DUMP_OFF_RET_EIP]   ; caller return address
    dump_reg _reg_flags, [ebp + DUMP_OFF_EFLAGS]
    dump_seg_reg _reg_cs, cs
    dump_seg_reg _reg_ds, ds

    dump_reg_nl

    ; dump segment registers
    dump_seg_reg _reg_es, es
    dump_seg_reg _reg_fs, fs
    dump_seg_reg _reg_gs, gs
    dump_seg_reg _reg_ss, ss

    dump_reg_nl

    ; dump control registers
    ; note that reading these requires ring0 privileges.
    dump_cr_reg _reg_cr0, cr0
    dump_cr_reg _reg_cr2, cr2
    dump_cr_reg _reg_cr3, cr3

    popfd   ; restore register state and flag state, return
    popa
    ret

.dump_current:
    ; Capture *current* machine state in a call-depth-independent way.
    ;
    ; pushfd/pushad order is intentional:
    ; - pushfd first places EFLAGS below the pushad block.
    ; - pushad then places GPRs at ESP..ESP+31.
    ;
    ; Final snapshot layout (EBX base set below):
    ;   [base+00] EDI
    ;   [base+04] ESI
    ;   [base+08] EBP
    ;   [base+12] ESP value from before pushad
    ;   [base+16] EBX
    ;   [base+20] EDX
    ;   [base+24] ECX
    ;   [base+28] EAX
    ;   [base+32] EFLAGS (from pushfd)
    ;
    ; This avoids the old issue where caller depth changed offsets.
    pushfd
    pushad

    mov ebx, esp                 ; EBX = stable base pointer for snapshot dereferences

    ; Header + GP registers (line 1)
    DBG_PUTS _dbg_curr_hdr
    DBG_LABEL_HEX _reg_eax, SNAP_EAX
    DBG_LABEL_HEX _reg_ebx, SNAP_EBX
    DBG_LABEL_HEX _reg_ecx, SNAP_ECX
    DBG_LABEL_HEX _reg_edx, SNAP_EDX

    ; Newline + index/stack-ish registers (line 2)
    DBG_PUTS _debug_newline
    DBG_LABEL_HEX _reg_esi, SNAP_ESI
    DBG_LABEL_HEX _reg_edi, SNAP_EDI
    DBG_LABEL_HEX _reg_ebp, SNAP_EBP
    DBG_LABEL_HEX _reg_esp, SNAP_ESP

    ; Newline + flags + EIP (line 3)
    DBG_PUTS _debug_newline
    DBG_LABEL_HEX _dbg_efl, SNAP_EFLAGS
    ; Note: we can't get the "current" EIP directly, but we can show the caller's EIP from the stack.
    mov eax, [ebx + SNAP_ESP]   ; caller return address is at ESP before pushad
    mov edx, DBG_ATTR
    call debug.hexprint 
    DBG_PUTS _debug_newline

    ; Restore caller context exactly as it was at entry.
    popad
    popfd
    ret

; Input: ESI = pointer to frame tail
.dump_frame:
    ; Dump normalized interrupt/frame tail provided by ISR path.
    ;
    ; Expected ESI layout (follows trap_frame_t struct layout by design):
    ; [ESI+0]  vector  (which IDT vector invoked)
    ; [ESI+4]  error   (CPU error code or synthetic 0)
    ; [ESI+8]  eip     (return instruction pointer)
    ; [ESI+12] cs      (return code segment)
    ; [ESI+16] eflags  (return flags)
    ;
    ; This routine does not infer stack offsets; it only dereferences
    ; the caller-provided frame pointer, so it is suitable for C wrappers.
    pushad

    mov ebx, esi                 ; EBX = stable frame base for DBG_LABEL_HEX

    ; Line 1: vector/error/eip/cs context
    DBG_PUTS _dbg_frame_hdr
    DBG_LABEL_HEX _dbg_vec, FR_VEC
    DBG_LABEL_HEX _dbg_err, FR_ERR
    DBG_LABEL_HEX _reg_eip, FR_EIP
    DBG_LABEL_HEX _dbg_cs,  FR_CS

    ; Line 2: flags
    DBG_PUTS _debug_newline
    DBG_LABEL_HEX _dbg_efl, FR_EFLAGS
    DBG_PUTS _debug_newline

    ; Restore caller GP registers.
    popad
    ret

section .rodata
_dbg_curr_hdr db 13,10,'[current] ',13,10,0
_dbg_frame_hdr db 13,10,'[frame] ',13,10,0
_dbg_vec db 'VEC=',0
_dbg_err db 'ERR=',0
_dbg_cs  db ' CS=',0
_dbg_efl db 'EFL=',0
_regdump         db 'REGISTERS:',10,0
_reg_eax         db 'EAX=', 0
_reg_ebx         db 'EBX=', 0
_reg_ecx         db 'ECX=', 0
_reg_edx         db 'EDX=', 0
_reg_esp         db 'ESP=', 0
_reg_ebp         db 'EBP=', 0
_reg_esi         db 'ESI=', 0
_reg_edi         db 'EDI=', 0
_reg_eip         db 'EIP=', 0
_reg_cs          db ' CS=', 0
_reg_ds          db ' DS=', 0
_reg_es          db ' ES=', 0
_reg_fs          db ' FS=', 0
_reg_gs          db ' GS=', 0 
_reg_ss          db ' SS=', 0
_reg_cr0         db 'CR0=', 0
_reg_cr2         db 'CR2=', 0
_reg_cr3         db 'CR3=', 0
_reg_flags       db 'FLG=', 0
_debug_tab      db 9, 0
_debug_newline  db 10,0