bits 32
global enter_usermode

; Bare minimum IRET trampoline for entering usermode from the kernel

; void enter_usermode(uint32_t eip, uint32_t user_esp, uint16_t user_cs, uint16_t user_ds);
; cdecl: [esp+4]=eip, [esp+8]=user_esp, [esp+12]=user_cs, [esp+16]=user_ds
; void enter_usermode(uint32_t entry_addr, uint32_t user_esp, uint32_t user_cs, uint32_t user_ds);
enter_usermode:
    push ebp
    mov  ebp, esp

    ; Load args once (cdecl):
    mov  eax, [ebp+8]    ; entry_addr
    mov  edx, [ebp+12]   ; user_esp
    mov  ecx, [ebp+16]   ; user_cs
    mov  ebx, [ebp+20]   ; user_ds

    ; User data segments must be set before iret
    mov  ds, bx
    mov  es, bx
    mov  fs, bx
    mov  gs, bx

    ; Build iret frame: SS, ESP, EFLAGS, CS, EIP
    push ebx             ; SS = user_ds
    push edx             ; ESP = user_esp
    pushfd
    or dword [esp], 0x200 ; IF=1
    push ecx             ; CS = user_cs
    push eax             ; EIP = entry_addr
    iretd
    