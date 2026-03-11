%ifndef A20_TEST_BIDIRECTIONAL 
    %ifndef A20_TEST_MONODIRECTIONAL
        %define A20_TEST_BIDIRECTIONAL
    %endif
%endif

%ifdef A20_TEST_BIDIRECTIONAL
    %define A20_TEST_LABEL .a20AliasBidirectionalTest
%elifdef A20_TEST_MONODIRECTIONAL
    %define A20_TEST_LABEL .checkA20MemoryAlias
%endif

.initA20:
    cli                 ; clear interrupts flag (already done probably, but just in case)
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready
.tryEnableA20:
    ; BIOS approach
    mov ax, 0x2401      ; BIOS function to enable A20 line
    int 0x15            ; call BIOS to enable A20
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready        ; if A20 not enabled, try the fast A20 method
                        ; fallthrough to a20 ready code
.tryEnableA20Fast:
    in al, 0x92         ; read port 0x92, which controls the A20 gate among other things
    and al, 0xFE        ; LSB should be 0
    or al, 0x02         ; second-LSB should be 1 to enable A20
    out 0x92, al        ; write it back to the port to enable the A20 gate
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready        ; if A20 still not enabled, we can either try again or just fail. For now, we'll just fail.
                        ; fallthrough to 8042 keyboard controller method
.tryEnableA20KbdController:
    ; 8042 method: disable keyboard interface, read output port,
    ; set A20 bit, write output port back, re-enable keyboard interface.
    call .kbd_flush_obf

    ; Wait until 8042 input buffer is clear, then disable keyboard interface (0xAD)
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, 0xAD
    out 0x64, al

    ; Wait, then issue read-output-port command (0xD0)
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, 0xD0
    out 0x64, al

    ; Wait for output buffer to fill, then read current output-port value
    call .kbd_wait_obf_set
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .tryEnableA20KbdControllerBlind
    in  al, 0x60
    or  al, 0x02    ; set A20 enable bit
    and al, 0xFE    ; ensure reset bit stays clear
    mov bl, al      ; preserve byte to write back

    ; Wait, then issue write-output-port command (0xD1)
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, 0xD1
    out 0x64, al

    ; Wait, then write updated output-port value
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, bl
    out 0x60, al

    ; Wait, then re-enable keyboard interface (0xAE)
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, 0xAE
    out 0x64, al

    ; Final A20 verification for 8042 path
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready
    jmp .tryEnableA20KbdControllerBlind

.tryEnableA20KbdControllerBlind:
    ; Blind 8042 fallback: avoid D0/OBF dependency.
    ; Sequence: D1 command + fixed output-port byte that enables A20.
    call .kbd_flush_obf

    ; Ensure keyboard interface is disabled while updating output port.
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, 0xAD
    out 0x64, al

    ; Issue write-output-port command (D1).
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, 0xD1
    out 0x64, al

    ; Write fixed output-port value:
    ; 0xDF = set A20 bit, keep reset deasserted.
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, 0xDF
    out 0x60, al

    ; Re-enable keyboard interface.
    call .kbd_wait_ibf_clear
    pushf
    ; mov si, dot ; Previously used for breadcrumb debugging
    ; call .print
    popf
    jc .a20Failed
    mov al, 0xAE
    out 0x64, al

    ; Verify whether blind fallback enabled A20.
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready
    jmp .a20Failed

.kbd_flush_obf:
    mov cx, 0xFFFF
.flush_loop:
    in  al, 0x64
    test al, 0x01
    jz .flush_done
    in  al, 0x60
    loop .flush_loop
.flush_done:
    ret

.kbd_wait_ibf_clear:
    ; Wait until input buffer empty (status bit1 == 0)
    mov cx, 0xFFFF
.wait_ibf:
    in  al, 0x64
    test al, 0x02
    jz .ibf_ok
    loop .wait_ibf
    stc
    ret
.ibf_ok:
    clc
    ret

.kbd_wait_obf_set:
    ; Wait until output buffer full (status bit0 == 1)
    push dx
    mov cx, 0xFFFF
    mov dx, 0x04
.wait_obf:
    in  al, 0x64
    test al, 0x01
    jnz .obf_ok
    in al, 0x80
    loop .wait_obf
    dec dx
    jnz .obf_reset
    stc
    pop dx
    ret
.obf_ok:
    clc
    pop dx
    ret
.obf_reset:
    mov cx, 0xFFFF
    jmp .wait_obf

.a20Failed:
    mov si, a20Error    ; print error message about A20 requirement
    call .print
    jmp halt            ; halt the system as we can't continue without A20 enabled

.checkA20MemoryAlias:   ; uses memory aliasing test to check for a20 gate enablement
                        ; returns AX = 1 if A20 enabled, 0 if not
    push es             ; these will be clobbered and need preservation
    push bx
    ; Reset the alias location's value so we don't get spurious false positives
    mov ax, 0xFFFF
    mov es, ax
    mov bx, 0x0610
    mov word [es:bx], 0x1337 ; Reset this value just in case

    ; Write a test value 0x1234 to 0000:0600
    mov ax, 0x0000      ; set up test values for memory alias test
    mov es, ax          ; ES=0x0000
    mov bx, 0x0600      ; BX=0x0600
    mov word [es:bx], 0x1234

    ; Attempt to read that same test value back from FFFF:0610
    ; If A20 enabled, we will read something other than 0x1234
    ; Otherwise, if A20 disabled, we will read 0x1234 due to aliasing
    mov ax, 0xFFFF
    mov es, ax          ; ES=0xFFFF
    mov bx, 0x0610      ; BX=0x0610
    mov ax, [es:bx]     ; read word at 0000:0600 and 0xFFFF:0610, which should be aliased if A20 is disabled and not aliased if A20 is enabled
                        ; if different, A20 is enabled
    cmp ax, 0x1234      ; if we find the test value at the aliased address, we're still in real mode
    je .a20AliasReturnDisabled ; if it matches, A20 is still disabled, so we jump to the code to try enabling it again (or fail if we've already tried)
.a20AliasReturnEnabled:
    pop bx
    pop es
    mov ax, 1      ; set AX to 1 meaning A20 enabled if we jump to a20_enabled, otherwise it will remain 0 meaning A20 not enabled
    ret
.a20AliasReturnDisabled:
    pop bx
    pop es
    xor ax, ax          ; clear AX to 0 meaning A20 not enabled
    ret

.a20AliasBidirectionalTest:
    push es
    push bx

    ; Write test value P to a low address A
    xor ax, ax
    mov es, ax
    mov bx, 0x0600
    mov word [es:bx], 0x1234

    ; Write a different test value Q to the high address B
    ; This will overwrite P in A if A20 is disabled
    mov ax, 0xFFFF
    mov es, ax
    mov bx, 0x0610
    mov word [es:bx], 0x5678
    
    ; Read back A, ensure we get P
    xor ax, ax
    mov es, ax
    mov bx, 0x0600
    mov ax, [es:bx]
    cmp ax, 0x1234
    jne .a20AliasBidirectionalTestFailed

    ; Read back B, ensure we get Q
    mov ax, 0xFFFF
    mov es, ax
    mov bx, 0x0610
    mov ax, [es:bx]
    cmp ax, 0x5678
    jne .a20AliasBidirectionalTestFailed
.a20AliasBidirectionalTestPassed:
    ; Return 1 on success
    mov ax, 1
    pop bx
    pop es
    ret
.a20AliasBidirectionalTestFailed:
    ; Return 0 on failure
    xor ax, ax
    pop bx
    pop es
    ret

.a20Ready:
    mov al, 0xAE          ; re-enable keyboard
    out 0x64, al
    jmp .setupProtectedModePrereqs