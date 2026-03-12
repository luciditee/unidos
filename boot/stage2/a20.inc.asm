%ifndef A20_TEST_BIDIRECTIONAL 
    %ifndef A20_TEST_MONODIRECTIONAL
        ; Default to stronger two-way alias test
        %define A20_TEST_BIDIRECTIONAL
    %endif
%endif

%ifdef A20_TEST_BIDIRECTIONAL
    ; Select which test routine to call through one macro so the A20 flow
    ; does not need duplicated test-call branches.
    %define A20_TEST_LABEL .a20AliasBidirectionalTest
%elifdef A20_TEST_MONODIRECTIONAL
    %define A20_TEST_LABEL .checkA20MemoryAlias
%endif

%define PORT_CMD_8042           0x64
%define PORT_DATA_8042          0x60
%define PORT_DEBUG              0x80
%define PORT_FASTA20            0x92

%define CMD_DISABLE_KBD         0xAD
%define CMD_ENABLE_KBD          0xAE
%define CMD_READ_OUTPUT_PORT    0xD0
%define CMD_WRITE_OUTPUT_PORT   0xD1
%define CMD_STATUS              0x64
%define CMD_SETA20_BIT          0xDF

.initA20:
    ; == A20 ENABLE ENTRYPOINT ==
    ; Called after root scan / kernel discovery.
    ; We keep this self-contained and do not assume previous A20 state.
    cli                 ; interrupts may harm timing-sensitive operations, so we disable them
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready        ; A20 already enabled, skip all enable methods
                        ; Generally never happens, but in the event we squeeze
                        ; this into stage1 or chain from another bootlaoder, we
                        ; check here. Otherwise, fall through to first method
.tryEnableA20:
    ; METHOD 1: BIOS INT 15h AX=2401 (preferred when implemented)
    ; This works on newer BIOSes, QEMU, etc. but is fairly rare on older
    ; true 386 hardware. Still the fastest/lowest-overhead method, so we
    ; always try it first. Unsupporting BIOSes may fail or ignore the command,
    ; so we test using the memory aliasing test and move to the next method if
    ; the test fails.

    mov ax, 0x2401      ; BIOS function to enable A20 line
    int 0x15            ; call BIOS to enable A20
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready
                        ; fallthrough if BIOS method did not enable A20
.tryEnableA20Fast:
    ; METHOD 2: PORT 0x92 "FAST A20"
    ; Usually available on late PS/2 386 models (Model 80) or 486-onward,
    ; but not guaranteed.
    ;
    ; Note: bit1 = A20 enable, bit0 = reset, so we AND mask bit0 and OR
    ; mask bit1 to 1
    in al, PORT_FASTA20 ; read port 0x92, which controls the A20 gate among other things
    and al, 0xFE        ; LSB should be 0
    or al, 0x02         ; second-LSB should be 1 to enable A20
    out PORT_FASTA20, al ; write it back to the port to enable the A20 gate
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready
                        ; fallthrough to 8042 keyboard-controller method
.tryEnableA20KbdController:
    ; METHOD 3A: 8042 WRITE/READBACK METHOD
    ; 
    ; Sends the following command sequence to the 8042:
    ; Disable keyboard input -> Read D0 -> Write D1 -> Update A20 
    ; bit -> Re-enable keyboard
    ;
    ; https://wiki.osdev.org/A20_Line#Keyboard_Controller_2
    ;
    ; Not all controllers reliably implement this method, so we instead
    ; use the blind method on those boards.
    call .kbd_flush_obf

    ; Wait until 8042 input buffer is clear, then disable keyboard interface (0xAD)
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, CMD_DISABLE_KBD
    out PORT_CMD_8042, al

    ; Wait, then issue read-output-port command (0xD0)
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, CMD_READ_OUTPUT_PORT
    out PORT_CMD_8042, al

    ; Wait for output buffer to fill, then read current output-port value
    call .kbd_wait_obf_set
    jc .tryEnableA20KbdControllerBlind
    in  al, PORT_DATA_8042
    or  al, 0x02    ; set A20 enable bit
    and al, 0xFE    ; ensure reset bit stays clear
    mov bl, al      ; preserve byte to write back

    ; Wait, then issue write-output-port command (0xD1)
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, CMD_WRITE_OUTPUT_PORT
    out PORT_CMD_8042, al

    ; Wait, then write updated output-port value
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, bl
    out PORT_DATA_8042, al

    ; Wait, then re-enable keyboard interface (0xAE)
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, CMD_ENABLE_KBD
    out PORT_CMD_8042, al

    ; Final A20 verification for 8042 path
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready
    jmp .tryEnableA20KbdControllerBlind

.tryEnableA20KbdControllerBlind:
    ; METHOD 3B: BLIND 8042 CONTROLLER WRITE
    ; Some cheaper board designs don't return D0 reliably or at all,
    ; but will correctly set the A20 gate. The more robust designs
    ; (like IBM PS/2 386s) will succeed on method 3A, but most people
    ; will probably run this code on 386 clone hardware, so this method
    ; is a necessary fallback for 8042-based A20
    ;
    ; 0xDF = 11011111b (CMD_SETA20_BIT) 
    ; This translates to "set A20 bit, keep reset deasserted"
    call .kbd_flush_obf

    ; Ensure keyboard interface is disabled while updating output port.
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, CMD_DISABLE_KBD    ; "disable keyboard interface" command
    out PORT_CMD_8042, al    ; send to 8042 command port

    ; Issue write-output-port command (D1).
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, CMD_WRITE_OUTPUT_PORT    ; "write output port" command
    out PORT_CMD_8042, al    ; send to 8042 command port

    ; Write fixed output-port value    
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, CMD_SETA20_BIT    ; 0xDF = set A20 bit, keep reset deasserted.
    out PORT_DATA_8042, al    ; 0x60 = 8042 data port, used in conjunction with port 0x64 above

    ; Re-enable keyboard interface.
    call .kbd_wait_ibf_clear
    jc .a20Failed
    mov al, CMD_ENABLE_KBD    ; "enable keyboard interface" command
    out PORT_CMD_8042, al    ; send to 8042 command port

    ; Verify whether blind fallback enabled A20.
    call A20_TEST_LABEL
    cmp ax, 1
    je .a20Ready
    jmp .a20Failed

.kbd_flush_obf:
    ; Drain stale bytes from 8042 output buffer (status bit0=1).
    ; This reduces chance of consuming unrelated old keyboard/controller data.
    mov cx, 0xFFFF
.flush_loop:
    in  al, PORT_CMD_8042
    test al, 0x01
    jz .flush_done
    in  al, PORT_DATA_8042
    loop .flush_loop
.flush_done:
    ret

.kbd_wait_ibf_clear:
    ; Wait until 8042 input buffer is empty (status bit1 == 0).
    ; Carry clear on success, set on timeout.
    mov cx, 0xFFFF
.wait_ibf:
    in  al, PORT_CMD_8042
    test al, 0x02
    jz .ibf_ok
    loop .wait_ibf
    stc
    ret
.ibf_ok:
    clc
    ret

.kbd_wait_obf_set:
    ; Wait until 8042 output buffer is full (status bit0 == 1).
    ; Uses two-level timeout so slower machines/emulators still succeed.
    ; Carry clear on success, set on timeout.
    push dx
    mov cx, 0xFFFF
    mov dx, 0x04
.wait_obf:
    in  al, PORT_CMD_8042
    test al, 0x01
    jnz .obf_ok
    in al, PORT_DEBUG
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
    ; All methods failed. System cannot reliably continue to high-memory
    ; loader flow without A20; stop with explicit error.
    mov si, a20Error    ; print error message about A20 requirement
    call .print
    jmp halt            ; halt the system as we can't continue without A20 enabled


 %ifdef A20_TEST_MONODIRECTIONAL                       
.checkA20MemoryAlias:   ; == A20 ALIASING TEST (MONODIRECTIONAL) ==
                        ; uses memory aliasing test to check for a20 gate enablement
                        ; returns AX = 1 if A20 enabled, 0 if not
                        ; This method is less preferred/robust than the bidirectional
                        ; approach, but is simpler and we keep it here just in case
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

    ; Attempt to read back from FFFF:0610.
    ; With A20 disabled, 0xFFFF:0x0610 aliases to 0x0000:0x0600.
    ; With A20 enabled, they are distinct physical locations.
    mov ax, 0xFFFF
    mov es, ax          ; ES=0xFFFF
    mov bx, 0x0610      ; BX=0x0610
    mov ax, [es:bx]     ; read word at 0000:0600 and 0xFFFF:0610, which should be aliased if A20 is disabled and not aliased if A20 is enabled
                        ; if different, A20 is enabled
    cmp ax, 0x1234      ; if we find the test value at the aliased address, A20 is disabled
    je .a20AliasReturnDisabled
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
%endif

%ifdef A20_TEST_BIDIRECTIONAL
.a20AliasBidirectionalTest:
    ; == A20 ALIASING TEST (BIDIRECTIONAL) ==
    ; Note: Preferred test over the monodirectional test as it essentially
    ; guarantees no quirky aliasing as might occur with cheaper chipsets
    ;
    ; Writes P to low alias candidate, writes Q to high alias partner,
    ; then verifies both values are retained. If Q overwrites P or vice versa,
    ; then A20 is not enabled and we return failure. Else, success.
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
    ; Return AX=1 on success
    mov ax, 1
    pop bx
    pop es
    ret
.a20AliasBidirectionalTestFailed:
    ; Return AX=0 on failure
    xor ax, ax
    pop bx
    pop es
    ret
%endif

.a20Ready:
    ; Finalize controller state and continue with unreal-mode setup path.
    ; Keep keyboard interface enabled when leaving A20 setup logic.
    call .kbd_wait_ibf_clear        ; ensure input buffer is clear
    mov al, CMD_ENABLE_KBD          ; re-enable keyboard
    out PORT_CMD_8042, al           ; send enable command to 8042
    jmp .enterUnreal