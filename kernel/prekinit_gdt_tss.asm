%define GDT_SEL_KCODE 0x08
%define GDT_SEL_KDATA 0x10
%define GDT_SEL_UCODE 0x18
%define GDT_SEL_UDATA 0x20
%define GDT_SEL_TSS   0x28

bits 32

extern kernel_stack_top

global gdt_tss_init
global gdt_ptr
global gdt_start
global tss32

align 8
gdt_start:
    ; Entry 0
    ; NULL descriptor. Required for x86 segmentation, which we generally don't use.
    ; Invalid by design.    
    dq 0x0000000000000000

    ; Entry 1
    ; Kernel code segment/ring0.
    ; Limit 0xFFFFF. 4KiB page granularity.
    ; 0x9A or 10011010b: present, dpl0/ring0, code, executable, readable.
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00

    ; Kernel data segment/ring0.
    ; Limit 0xFFFFF. 4KiB page granularity.
    ; 0x92 or 10010010b: present, dpl0/ring0, data, writable, not executable.
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00

    ; User code segment/ring3.
    ; Limit 0xFFFFF. 4KiB page granularity.
    ; 0xFA or 11111010b: present, dpl3/ring3, code, executable, readable.
    dw 0xFFFF, 0x0000
    db 0x00, 11111010b, 11001111b, 0x00

    ; User data segment/ring3.
    ; Limit 0xFFFFF. 4KiB page granularity.
    ; 0xF2 or 11110010b: present, dpl3/ring3, data, writable, not executable.
    dw 0xFFFF, 0x0000
    db 0x00, 11110010b, 11001111b, 0x00

tss_desc: ; Note: Still part of GDT struct, but reserved for TSS.
    ; 32-bit available TSS descriptor (system segment type = 9)
    ; Base is 0 here, but patched at runtime because linear address may not be known
    ; ahead of time.
    dw (tss32_end - tss32 - 1)                ; limit = TSS size - 1, or 104 bytes long (0x67)
    dw 0x0000                                 ; base 15:0
    db 0x00                                   ; base 23:16
    db 10001001b                              ; present, DPL0, type=9 (avail 32b TSS)
    db 00000000b                              ; byte granularity
    db 0x00                                   ; base 31:24

gdt_end: ; End of GDT struct.

gdt_ptr:
    ; Meta-struct which defines the bounds of the GDT for lgdt instruction
    ; Initial word is size of GDT - 1, and the following dword is GDT linear address
    dw gdt_end - gdt_start - 1
    dd gdt_start

align 16
tss32:
    ; 32-bit TSS struct
    ; Populated by gdt_tss_init, but defined here for size calculations and
    ; descriptor patching.
    ;
    ; Only fields we care about are ESP0/SS0 for ring3->ring0 stack switch,
    ; and I/O map base to prevent any I/O bitmap from being present.
    ; https://wiki.osdev.org/Task_State_Segment
    times 104 db 0
tss32_end:

gdt_tss_init:
    ; Avoid clobbering ecx
    push ecx

    ; Patch TSS base into descriptor
    mov eax, tss32
    mov word [tss_desc + 2], ax
    shr eax, 16
    mov byte [tss_desc + 4], al
    mov byte [tss_desc + 7], ah

    ; Zero entire TSS
    xor eax, eax
    mov edi, tss32
    mov ecx, (tss32_end - tss32) / 4
    cld
    push es         ; save old ES
    push ds         ; save old DS, but then
    pop es          ; move it into ES
    rep stosd       ; Fill TSS with zeroes as expected writing ES:EDI
    pop es          ; restore old ES
    ; Note: We don't need to restore DS because we reload segment registers
    ; after loading GDT

    ; Populate minimum required TSS fields for ring3->ring0 stack switch.
    ; ESP0/SS0 are used when the CPU switches from user->kernel e.g. interrupt handling
    mov eax, kernel_stack_top
    mov [tss32 + 4], eax                      ; ESP0
    mov word [tss32 + 8], GDT_SEL_KDATA       ; SS0

    ; Set I/O map base to end of TSS to prevent any I/O bitmap from being present.
    ; User tasks cannot use IN/OUT unless IOPL allows (and we won't be doing that).
    mov word [tss32 + 0x66], (tss32_end - tss32) ; I/O map base = sizeof(TSS) => no bitmap

    ; Activate GDT
    lgdt [gdt_ptr]

    pop ecx

    ; Reload CS with far jump.
    jmp GDT_SEL_KCODE:.flush_cs

.flush_cs:
    ; Reload data segment registers.
    mov ax, GDT_SEL_KDATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Load task register with TSS.
    ; 386 automatically recognizes TSS descriptor in GDT and loads
    ; base/limit from it, so just need to give selector.
    mov ax, GDT_SEL_TSS
    ltr ax

    ; Read back TR to verify LTR succeeded.
    ; if ltr fails, you fault/reset before reaching this point.
    ; str gives current TR selector for an explicit check.
    str ax
    cmp ax, GDT_SEL_TSS
    jne .ltr_fail

    ret

.ltr_fail:
    ; Halt loop if LTR failed, which likely means TSS descriptor 
    ; was malformed or not present in GDT.
    cli
    hlt
    jmp .ltr_fail
    