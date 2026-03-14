%define VGA_TEXT_BUFFER 0xB8000
%define VGA_ATTR_DEFAULT    0x0F        ; bright white on black
%define VGA_CURSOR_PORT     0x3D4
%define VGA_DEFAULT_COLS    80
%define VGA_DEFAULT_ROWS    25
%define VGA_DEFAULT_CELLS   (VGA_DEFAULT_COLS * VGA_DEFAULT_ROWS)
%define VGA_ROW_BYTES       (VGA_DEFAULT_COLS * 2)
%define VGA_LAST_ROW        ((VGA_DEFAULT_ROWS - 1) * VGA_DEFAULT_COLS)
%define VGA_FG_BLACK        0
%define VGA_FG_BLUE         1
%define VGA_FG_GREEN        2
%define VGA_FG_CYAN         3
%define VGA_FG_RED          4
%define VGA_FG_MAGENTA      5
%define VGA_FG_BROWN        6
%define VGA_FG_GRAY         7
%define VGA_FG_BRIGHT       8
%define VGA_BG_BLACK        0
%define VGA_BG_BLUE         (1 << 4)
%define VGA_BG_GREEN        (2 << 4)
%define VGA_BG_CYAN         (3 << 4)
%define VGA_BG_RED          (4 << 4)
%define VGA_BG_MAGENTA      (5 << 4)
%define VGA_BG_BROWN        (6 << 4)
%define VGA_BG_GRAY         (7 << 4)
%define VGA_BLINK           (8 << 4)
%define VGA_COLOR(fg, bg)   ((fg) | (bg))

vgatext:
.puts:      ; Input: ESI=pointer to null-terminated string
            ; DL=VGA attribute byte for entire string
    push esi                    ; preserve ESI, EAX on stack
    push eax
    test esi, esi
    jz ._puts_done              ; if ESI is NULL, do nothing and return
    cld
._puts_next:
    lodsb
    test al, al
    jz ._puts_done
    mov ah, dl
    call .putch
    jmp ._puts_next
._puts_done:
    pop eax
    pop esi
    ret

.putch:     ; Input: AL=character, AH=attribute/color byte
            ; Nonprintable control chars \r, \n, \t, or \b are handled
    push ebx                    ; preserve registers on stack
    push ecx
    push edx

    cmp al, 0x0A                ; '\n'
    je ._putch_newline
    cmp al, 0x0D                ; '\r'
    je ._putch_carriageret
    cmp al, 0x09                ; '\t'
    je ._putch_tab
    cmp al, 0x08                ; '\b'
    je ._putch_backspace

     ; Printable character
    mov bx, ax                  ; save char/attr
    call .get_cursor_eax        ; AX = linear cursor index (cells)
    mov cx, ax                  ; save index for cursor update

    movzx edx, ax               ; zero-extend AX to EDX
    shl edx, 1                  ; cell index -> byte offset
    add edx, VGA_TEXT_BUFFER    ; offset from start of VGA buffer
    mov [edx], bx               ; write character/attribute word

    mov ax, cx                  ; restore cell index in AX
    inc ax                      ; next cell
    call .normalize_cursor_after_write
    call .set_cursor_linear     ; set cursor position to cell held in AX
    jmp ._putch_done
._putch_newline:
    call .get_cursor_eax        ; AX = index
    mov bx, VGA_DEFAULT_COLS    ; 80
    xor dx, dx                  ; clear high word of dividend
    div bx                      ; AX=row, DX=col
    inc ax                      ; next row
    mul bx                      ; AX=(row+1)*80
    call .normalize_cursor_after_write
    call .set_cursor_linear
    jmp ._putch_done
._putch_carriageret:
    call .get_cursor_eax        ; AX = index
    mov bx, VGA_DEFAULT_COLS
    xor dx, dx
    div bx                      ; AX=row, DX=col
    mul bx                      ; AX=row*80 (col 0)
    call .normalize_cursor_after_write
    call .set_cursor_linear
    jmp ._putch_done
._putch_tab:
    call .get_cursor_eax        ; AX = index
    mov bx, VGA_DEFAULT_COLS
    xor dx, dx
    div bx                      ; AX=row, DX=col

    mov cx, dx
    add cx, 4
    and cx, 0xFFFC              ; next multiple of 4
    cmp cx, bx
    jb ._tab_same_row
    inc ax                      ; wrap to next row if col >= 80
    xor cx, cx
._tab_same_row:
    mul bx                      ; AX=row*80
    add ax, cx                  ; + new col
    call .normalize_cursor_after_write
    call .set_cursor_linear
    jmp ._putch_done
._putch_backspace:
    call .get_cursor_eax        ; AX = index
    test ax, ax                 ; if cursor is at 0, do nothing
    jz ._putch_done             ;
    dec ax                      ; otherwise, simply decrement to move back 1 cell
    call .normalize_cursor_after_write
    call .set_cursor_linear
._putch_done:
    pop edx                     ; restore EDX
    pop ecx                     ; restore ECX
    pop ebx                     ; restore EBX
    ret

._vga_scroll:                  ; scroll text buffer up by 1 row
    push eax
    push ecx
    push esi
    push edi

    cld
    mov esi, VGA_TEXT_BUFFER + VGA_ROW_BYTES      ; source: row 1
    mov edi, VGA_TEXT_BUFFER                      ; dest:   row 0
    mov ecx, ((VGA_DEFAULT_ROWS - 1) * VGA_ROW_BYTES) / 4 ; 3840/4 = 960 dwords
    rep movsd

    mov edi, VGA_TEXT_BUFFER + ((VGA_DEFAULT_ROWS - 1) * VGA_ROW_BYTES)
    mov ah, VGA_ATTR_DEFAULT
    mov al, ' '
    mov ecx, VGA_DEFAULT_COLS
    rep stosw                                      ; clear last row

    pop edi
    pop esi
    pop ecx
    pop eax
    ret

.normalize_cursor_after_write:    ; in: AX=cursor index, out: AX normalized
._chk:
    cmp ax, VGA_DEFAULT_CELLS
    jb ._done
    call ._vga_scroll
    sub ax, VGA_DEFAULT_COLS
    jmp ._chk
._done:
    ret

.get_cursor_eax:                ; Returns cursor position in AX (EAX zero-extended)
    push edx
    xor eax, eax
    mov dx, VGA_CURSOR_PORT     ; 0x3D4

    mov al, 0x0E
    out dx, al                  ; select high cursor byte
    inc dx                      ; 0x3D5
    in al, dx
    mov ah, al                  ; AH = high byte

    dec dx                      ; 0x3D4
    mov al, 0x0F
    out dx, al                  ; select low cursor byte
    inc dx                      ; 0x3D5
    in al, dx                   ; AL = low byte

    pop edx
    ret

.set_cursor:                    ; Input: AL=row, AH=col
    push edx                    ; preserve EDX, EBX on stack
    push ebx                    
    xor edx, edx
    xor ebx, ebx

    mov bx, ax                  ; BH=col. After SHR, BX=col
    shr bx, 8                   ; BX = col
    and ax, 0x00FF              ; AX = row

    mov dl, VGA_DEFAULT_COLS    ; 80
    mul dx                      ; AX = row * 80  (DX:AX full product)
    add ax, bx                  ; AX = linear cursor index
    pop ebx                     ; restore EBX, EDX. We do this so that ebx/edx are preserved
    pop edx                     ; regardless of calling set_cursor or set_cursor_linear
.set_cursor_linear:             ; Input: AX=linear cursor index
    push edx                    ; preserve DX on stack
    push ebx                    ; preserve BX on stack

    mov bx, ax                  ; preserve position bytes: BL=low, BH=high
    mov dx, VGA_CURSOR_PORT     ; port 0x3D4
                                ; from here on, DX will have the port we're using
    mov al, 0x0F
    out dx, al                  ; select low cursor register, indicate our intent to write to it (CRTC)
    inc dx                      ; next port up from 3D4h
    mov al, bl                  ; get low byte back from BL
    out dx, al                  ; write low byte of position

    dec dx                      ; back down to 0x3D4
    mov al, 0x0E                ; select high cursor register
    out dx, al                  ; write command indicating intent to set high byte
    inc dx                      ; next port up from 0x3D4 again
    mov al, bh                  ; get high byte back from BH
    out dx, al                  ; write high byte of position

    pop ebx                     ; restore EBX, EDX, return
    pop edx
    ret

.set_cursor_enabled:            ; Input: AL=0 disable, AL!=0 enable
    pushfd                      ; preserve flags, eax, edx
    push eax
    push edx

    mov ah, al                  ; save requested state
    mov dx, VGA_CURSOR_PORT     ; 0x3D4
    mov al, 0x0A                ; CRTC cursor start register
    out dx, al                  ; select register 0x0A
    inc dx                      ; 0x3D5
    in al, dx                   ; read current cursor start value
    test ah, ah                 ; check for nonzero (enable)
    jz ._disable_cursor         ; if zero, jump to disable case
                                ; fallthrough to enable case
._enable_cursor:
    and al, 0xDF                ; clear bit 5 = enable
    jmp ._write_cursor_state    ; jump to output routine
._disable_cursor:
    or  al, 0x20                ; set bit 5 = disable
                                ; fallthrough to output routine
._write_cursor_state:
    out dx, al                  ; write updated cursor start register
    pop edx                     ; restore registers and flags, return
    pop eax
    popfd                       
    ret

.cls:
    push eax
    push ecx
    push edx
    push edi
    pushfd

    mov ax, (VGA_COLOR(VGA_FG_BRIGHT | VGA_FG_GRAY, VGA_BG_BLACK) << 8) | ' '
    mov ecx, VGA_DEFAULT_CELLS
    mov edi, VGA_TEXT_BUFFER

    cld
    rep stosw

    xor ax, ax
    call .set_cursor_linear     ; reset cursor to 0

    popfd
    pop edi
    pop edx
    pop ecx
    pop eax

    

    ret