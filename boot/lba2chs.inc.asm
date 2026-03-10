lba2chs:
    ; Convert LBA in AX to CHS in CH, DH, CL
    ; Input: AX = LBA sector number
    ; Output: CH = cylinder, DH = head, CL = sector (1-based)
    push ax
    push bx
;    mov [lba2chs_saved_dl], dl

    xor dx, dx
    mov bx, (SECTORS_PER_TRACK * FLOPPY_HEADS)
    div bx              ; AX = cylinder, DX = remainder
    push ax             ; save cylinder

    mov ax, dx
    xor dx, dx
    mov bx, SECTORS_PER_TRACK
    div bx              ; AX = head, DX = sector (0-based)

    mov dh, al
    mov cl, dl
    inc cl              ; BIOS sector number is 1-based

    pop ax              ; restore cylinder
    mov ch, al          ; cylinder low 8 bits
    mov al, ah
    and al, 0x03
    shl al, 6
    and cl, 0x3F
    or cl, al           ; cylinder high 2 bits into CL[7:6]

;    mov dl, [lba2chs_saved_dl]
    pop bx
    pop ax
    ret