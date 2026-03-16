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

global vgatext.get_cursor_eax
global vgatext.puts
global vgatext.puch
global vgatext.set_cursor_linear
global vgatext.set_cursor_enabled
global vgatext.cls

vgatext:
.puts:      ; Input: ESI=pointer to null-terminated string, or NULL
            ; DL=VGA attribute byte for entire string
    push esi                    ; preserve ESI, EAX on stack
    push eax                    
    test esi, esi
    jz ._puts_done              ; if ESI is NULL, do nothing and return
    cld                         ; ensure string operations use forward direction
._puts_next:
    lodsb                       ; load byte at ESI into AL, increment ESI
    test al, al                 ; test if done (null terminator)
    jz ._puts_done              ; jump to done if so
    mov ah, dl                  ; move attribute byte into AH for putch consumption
    call .putch                 ; print character AL=char AH=attr
    jmp ._puts_next             ; loop to print next character
._puts_done:                    
    pop eax                     ; restore registers and return
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
    call .get_cursor_eax        ; find where we are currently
    mov bx, VGA_DEFAULT_COLS    ; get the number of columns
    xor dx, dx                  ; clear dx for division
    div bx                      ; integer division to get current row in AX, discard column in DX
    mul bx                      ; AX=row*80 (col 0)
    call .normalize_cursor_after_write  ; cursor bounds check/scrolling
    call .set_cursor_linear     ; set cursor position to cell held in AX
    jmp ._putch_done            ; jump to exit
._putch_tab:
    call .get_cursor_eax        ; AX = index
    mov bx, VGA_DEFAULT_COLS    ; get the number of columns
    xor dx, dx                  ; clear dx for division
    div bx                      ; integer division to get current row in AX, current col in DX

    mov cx, dx                  ; save current col in CX for later
    add cx, 4                   ; move to next tab stop (every 4 columns)
    and cx, 0xFFFC              ; round to next multiple of 4
    cmp cx, bx                  ; if new col >= number of columns, we need to wrap to next line
    jb ._tab_same_row           ; otherwise, we can stay on same row
    inc ax                      ; wrap to next row if col >= 80
    xor cx, cx                  ; reset col to 0
._tab_same_row:
    mul bx                      ; AX=row*80
    add ax, cx                  ; + new col
    call .normalize_cursor_after_write ; cursor bounds check/scrolling
    call .set_cursor_linear     ; set cursor position to cell held in AX
    jmp ._putch_done            ; jump to exit
._putch_backspace:
    call .get_cursor_eax        ; AX = index
    test ax, ax                 ; if cursor is at 0, do nothing
    jz ._putch_done             ; 
    dec ax                      ; otherwise, simply decrement to move back 1 cell
    call .normalize_cursor_after_write ; cursor bounds check/scrolling
    call .set_cursor_linear
._putch_done:
    pop edx                     ; restore registers and return
    pop ecx                     ; 
    pop ebx                     ; 
    ret

._vga_scroll:   ; internal: scroll text buffer up by 1 row
    push eax    ; preserve registers on stack
    push ecx
    push esi
    push edi

    cld         ; ensure forward direction for string ops
    ; Move rows 1..24 up to 0..23. Each row is VGA_DEFAULT_COLS * 2 bytes, so 160 bytes per row.
    ; We can move 4 bytes at a time with movsd, so ECX = (160/4) * 24 = 960 dwords.
    mov esi, VGA_TEXT_BUFFER + VGA_ROW_BYTES      ; source: row 1
    mov edi, VGA_TEXT_BUFFER                      ; dest:   row 0
    mov ecx, ((VGA_DEFAULT_ROWS - 1) * VGA_ROW_BYTES) / 4 ; 3840/4 = 960 dwords
    rep movsd

    ; Clear last row (row 24) by writing spaces with default attribute
    mov edi, VGA_TEXT_BUFFER + ((VGA_DEFAULT_ROWS - 1) * VGA_ROW_BYTES)
    mov ah, VGA_ATTR_DEFAULT
    mov al, ' '
    mov ecx, VGA_DEFAULT_COLS
    rep stosw                                      ; clear last row

    pop edi     ; restore registers and return
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
    push edx                    ; preserve EDX on stack
    xor eax, eax                ; clear EAX
    mov dx, VGA_CURSOR_PORT     ; 0x3D4

    mov al, 0x0E                ; select high cursor byte
    out dx, al                  ; send command
    inc dx                      ; 0x3D5 is the port we get result from 
    in al, dx                   ; read port to get result from VGA CRTC
    mov ah, al                  ; AH now holds high byte

    dec dx                      ; 0x3D4
    mov al, 0x0F                ; select low cursor byte
    out dx, al                  ; send command
    inc dx                      ; 0x3D5 is VGA CRTC port for result of last command
    in al, dx                   ; read result. AL now holds low byte

    pop edx                     ; restore EDX, return with AX=cursor index
    ret

.set_cursor:                    ; Input: AL=row, AH=col
    push edx                    ; preserve EDX, EBX on stack
    push ebx                    
    xor edx, edx
    xor ebx, ebx

    mov bx, ax                  ; AX packs row/col: AL=row, AH=col. Shift to isolate col in BX
    shr bx, 8                   ; BX = col
    and ax, 0x00FF              ; AX = row

    mov dl, VGA_DEFAULT_COLS    ; 80
    mul dx                      ; AX = row * 80  (DX:AX full product)
    add ax, bx                  ; AX = linear cursor index
    pop ebx                     ; restore EBX, EDX. We do this so that ebx/edx are preserved
    pop edx                     ; regardless of calling set_cursor or set_cursor_linear
.set_cursor_linear:             ; Input: AX=linear cursor index
    push edx                    ; preserve EDX, EBX on stack
    push ebx                    

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
    push eax    ; preserve register state and flags
    push ecx
    push edx
    push edi
    pushfd

    ; build a default cell value: blank space char, white on black
    mov ax, (VGA_COLOR(VGA_FG_BRIGHT | VGA_FG_GRAY, VGA_BG_BLACK) << 8) | ' '
    mov ecx, VGA_DEFAULT_CELLS  ; set how many cells we expect to iterate
    mov edi, VGA_TEXT_BUFFER    ; point to start of VGA text buffer

    cld                         ; ensure forward direction for string ops
    rep stosw                   ; write default cell value held in AX into text buffer

    xor ax, ax                  ; ax=0 means top-left on screen (start of text buffer)
    call .set_cursor_linear     ; reset cursor to value in ax

    popfd       ; restore registers and flags, return
    pop edi
    pop edx
    pop ecx
    pop eax
    ret