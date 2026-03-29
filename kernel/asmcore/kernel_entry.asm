bits 32

global start
extern kmain
extern ktrampoline
extern __kernel_start_lma
extern __bss_start_lma
extern __bss_end_lma

%ifndef KERNEL_VIRT_BASE
%define KERNEL_VIRT_BASE 0xC0000000
%endif

%define BOOTSTRAP_PDE_INDEX (KERNEL_VIRT_BASE >> 22)

section .text.start

start:
    cli

    ; Build minimal bootstrap paging:
    ; - identity map first 4 MiB
    ; - mirror first 4 MiB at KERNEL_VIRT_BASE
    ; so higher-half linked code can execute immediately after this point.
    call .enable_bootstrap_paging

    jmp kernel

.enable_bootstrap_paging:
    pushad                  ; push all GP registers since we clobber basically all of them
    cld                     ; direction flag should be clear for copy ops

    ; We're setting up a very minimal paging environment here *just* to get
    ; the kernel's higher-half code executing and bootstrap kmain
    
    ; Zero page directory.
    xor eax, eax            ; PDEs will each be 0, so EAX=0
    mov edi, bootstrap_pd   ; point EDI to start of bootstrap page directory
    mov ecx, 1024           ; 1024 PDEs in the page directory
    rep stosd               ; fill what EDI points to with what EAX contains up to ECX times

    ; Zero first page table.
    ; Same as above, but for PTEs instead of PDEs
    xor eax, eax            
    mov edi, bootstrap_pt0
    mov ecx, 1024
    rep stosd

    ; Zero higher-half bootstrap page table.
    ; Again, same as above, but to be used by higher-half mapping
    xor eax, eax            
    mov edi, bootstrap_pt_hi
    mov ecx, 1024
    rep stosd

    ; PT0 needs to contain something. Setup as identity map for first 4MiB
    ; with RW+Present flags    
    xor ebx, ebx            ; clear EBX, to be used as PF index
.fill_pt0:
    mov eax, ebx            ; physical address = page frame index * page size (4096)
    shl eax, 12             ; * 4096
    or eax, 0x003           ; RW=1, Present=1
    mov [bootstrap_pt0 + ebx*4], eax    ; set PTE for this page frame
    inc ebx                 ; next page frame
    cmp ebx, 1024           ; loop until we've filled all 1024 PTEs
    jl .fill_pt0

    mov eax, bootstrap_pt0  ; physical address of PT0
    or eax, 0x003           ; RW=1, Present=1

    ; Set PDE[0] to point to PT0
    ; establishes identity map
    mov [bootstrap_pd + (0 * 4)], eax


    ; PT_hi: map KERNEL_VIRT_BASE..KERNEL_VIRT_BASE+4MiB to
    ; __kernel_start_lma thru __kernel_start_lma+4MiB.
    ; Doing this by hand sucks, doing it with linker script voodoo is less bad
    ; and at least lets us set boundaries at compile-time
    mov ebx, __kernel_start_lma
    xor ecx, ecx
.fill_pt_hi:
    mov eax, ebx
    or eax, 0x003
    mov [bootstrap_pt_hi + ecx*4], eax  ; set PTE for this page frame
    add ebx, 0x1000
    inc ecx                 ; next page frame
    cmp ecx, 1024           ; loop until we've filled entire PTE
    jl .fill_pt_hi

    mov eax, bootstrap_pt_hi
    or eax, 0x003

    ; Set PDE[BOOTSTRAP_PDE_INDEX] to point to PT_hi
    ; PDE[KERNEL_VIRT_BASE >> 22] -> PT_hi (higher-half window)
    mov [bootstrap_pd + (BOOTSTRAP_PDE_INDEX * 4)], eax

    ; OK, done with page tables, page directory, and associated arithmetic pain
    ; Load page directry and populate cr0 to enable paging
    mov eax, bootstrap_pd   ; physical address of page directory
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ;jmp .paging_enabled
                            ; Intentional fallthrough to exit case
.paging_enabled:
    popad
    ret

%include "prekinit_constants.asm"
section .text
%include "prekinit_vga.asm"
section .text
%include "prekinit_debug.asm"
section .text
%include "prekinit_gdt_tss.asm"
section .text
%include "prekinit_idt.asm"
section .text
%include "prekinit_isr_stubs.asm"

section .text

kernel:
    cli
    ; Save pointer to kparams once higher-half alias mapping is active.
    mov [kparam_length], cx
    mov [kparam_ptr], esi

    call gdt_tss_init   ; GDT and TSS *must* exist before we can do anything else
    call idt_init       ; IDT must also exist, but handlers won't be installed until later
    call vgatext.cls    ; Clear screen. Partly serves as a debugging checkpoint.
    call .kernel_bss_zero ; Zero out the .bss section before we do anything else, to 
                          ; ensure global/static variables are zero-initialized as expected.
                        ; Deliberate fallthrough to maskpic and kmain call
.maskpic:
    ; Mask PIC while doing high-half remap
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al
.trampoline:
    ; Print entry message and jump to trampoline to do high-half remap
    mov esi, kentry_verstr
    mov dl, 0x0E
    call vgatext.puts
    call ktrampoline
    
    ; Point stack to high-half before unconditional jump to kmain
    mov eax, [g_stack_virt_addr]
    mov esp, eax
    mov [tss32 + 4], eax                ; Update TSS ESP0 to point to new stack in case of ring3->ring0 switch
    mov word [tss32 + 8], GDT_SEL_KDATA ; Update TSS SS0 as well
    jmp kmain

section .text

; Assumptions made at entry:
; - ESI points to kparams (or is NULL)
; - The stack is set up and can be used by the kernel
; - The CPU is in protected mode with flat segmentation (but needs new GDT/IDT)
; - The VGA text mode is active and can be used for output
    ; (Old code below, will remove later)
    ; Fallthrough to kmain
    ; cdecl call: kmain(uint32_t kparam_ptr, uint32_t kparam_length)
%if 0
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
%endif

.halt:
    cli
    hlt
    jmp .halt

.kernel_bss_zero:
    ; Zero out the .bss section (uninitialized global/static variables)
    push edi
    push ecx
    push eax
    mov edi, __bss_start_lma
    mov ecx, __bss_end_lma
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
extern g_stack_virt_addr

section .data
kparam_length dw 0
kparam_ptr dd 0

section .bss.start nobits align=4096
bootstrap_pd  resd 1024
bootstrap_pt0 resd 1024
bootstrap_pt_hi resd 1024