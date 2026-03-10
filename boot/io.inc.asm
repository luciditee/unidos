
; Presumes the existence of the following variables, to be provided by
; the including file:
; flpdrv         - the value of DL corresponding to the boot drive
; maxRetry       - the maximum number of retries for floppy operations
; errorMsg       - the error message prefix string
; haltMsg        - the halt message string
; fallbackMsg    - fallback message string in case of reset failure
; fdReadError    - the floppy read error message string
; flpdx          - scratch area for saving DX across floppy calls
; flpax          - scratch area for saving AX across floppy calls
; flpretry       - floppy operation retry counter
; newline        - newline string for printing

.print:                     ; === BIOS PRINTING PROCEDURE ===
    pusha                   ; print called from anywhere so we save state
    ;mov cx, PRINT_MAX      ; in case we want to limit print length
.print_loop:
    ;cmp cx, 0              ; length limit comparison
    ;je .doneprint          ; length limit jump
    lodsb                   ; load byte at SI into AL and increment SI
    test al, al             ; check if we've hit the null terminator
    jz .doneprint           ; jump to restore state if so
    mov ah, 0x0E            ; BIOS teletype function
    mov bh, 0x00            ; page number (should be 0 for single display)
    mov bl, 0x07            ; text attribute (light gray on black)
    int 0x10                ; call BIOS to print character in AL
    ;dec cx                 ; length limit tracker decrement
    jmp .print_loop         ; repeat loop
.doneprint:
    popa                    ; restore register state
    ret

.floppyread:            ; === FLOPPY READ PROCEDURE ===
                        ; DL should have boot device, AL should have sector count
                        ; CH should be cylinder/track, CL should be sector number
    push es             ; these will be clobbered, we should preserve them
    push bx             ; ditto
    mov byte [flpretry], 0      ; clear retry counter
    mov [flpax], ax     ; we'll restore this later -- for now we want to enforce 'read' (2)
    mov ah, 0x02        ; set floppy command mode to 'read'
                        ; fall through to .floppyint
    mov [flpdx], dx     ; copy value of DX elsewhere in case it gets clobbered later
.floppyint:
    mov dx, [flpdx]     ; restore state of DX in case clobbered by BIOS earlier
    int 13h             ; call floppy routine
    jc .floppyretry     ; if CF set, there was an error
    pop bx              ; we've succeeded so we should restore the segment regs
    pop es              ; ditto
    mov ax, [flpax]     ; restore AX before returning
    ret                 ; if not, we can return from this procedure

.floppyretry:
    inc byte [flpretry] ; increment our retry count
    mov al, [flpretry]  ; put the number of attempts we've made in ax
    cmp al, [maxRetry]  ; check if it equals the maximum number of retries we permit
    je .floppyerror     ; if we've hit max attempts, error out
    mov ax, [flpax]     ; restore AX to the original value (number of sectors to read)
    mov dl, [flpdrv]    ; restore drive number to DL just in case it was clobbered
    mov ah, 0x00        ; reset drive just in case
    int 13h             ; call interrupt to reset (TODO: possibly jc .floppyerror on fail)
    mov ah, 0x02        ; BIOS may change AH, we want to reset it to mean 'read' (2)
    mov dx, [flpdx]     ; restore DX to guarantee correct drive is read
    jmp .floppyint      ; otherwise, go back and try again on the interrupt

.floppyerror:           
    mov ax, fdReadError      ; load SI with error message string
    jmp .generror

.generror:              ; == GENERAL ERROR PRINT ==
    mov si, errorMsg    ; load SI with error message prefix string
    call .print         ; print it
    mov si, ax          ; load SI with error message passed into ax earlier
    call .print         ; print it
    mov si, haltMsg     ; load SI with error message suffix string
    call .print         ; print it
    jmp .wait_key_reset ; we consider this unrecoverable for now

.wait_key_reset:
    xor ax, ax          ; clear ax register for keyboard input
    int 16h             ; await any key
    mov si, newline     ; load SI with newline before resetting
    call .print         ; print newline
    int 19h             ; reset system by jumping to BIOS entry point
    mov si, fallbackMsg ; if reset fails, print fallback message
    call .print         ; print fallback message
    jmp $               ; fallback loop