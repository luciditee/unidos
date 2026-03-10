

%include "media.asm"

; NOTE: At the moment, this file is EXACTLY 512 bytes.
; It cannot be added to without refactoring.

; Memory map constants
%define STAGE2_LOAD_SEGMENT 0x1000

; Support constants
; %define PRINT_MAX 11

bits 16
org 0x7C00

jmp short start ; skip BPB and jump to code
nop             ; BPB padding

; BIOS Parameter Block
OEMLabel            db 'UNIDOS  '
BytesPerSector      dw BYTES_PER_SECTOR
SectorsPerCluster   db FAT_SECTORS_PER_CLUSTER
ReservedSectors     dw 1+STAGE2_RESERVED_SECTORS
FatCount            db FAT_COUNT
RootEntries         dw ROOT_ENTRIES
TotalSectors16      dw TOTAL_SECTORS16
MediaDescriptor     db MEDIA_DESCRIPTOR
SectorsPerFAT       dw SECTORS_PER_FAT
SectorsPerTrack     dw SECTORS_PER_TRACK
Heads               dw FLOPPY_HEADS
HiddenSectors       dd 0
TotalSectors32      dd 0    ; Unused until fat32 support added, 0 for now
DriveNumber         db 0
Reserved1           db 0
BootSignature       db 0x29
VolumeID            dd 0x20260306
VolumeLabel         db 'UNIDOSBOOT '
FileSystemType      db 'FAT12   '

hang:
    hlt
    jmp hang

%include "lba2chs.asm"

start:
    cli                 ; kill interrupts for now
    xor ax, ax          ; Put registers into a known state and establish stack
    mov ds, ax          ;
    mov es, ax          ;
    mov ss, ax          ;
    mov sp, 0x7C00      ;
    cld                 ; Filesystem manipulation means string ops means we clear direction flag
    mov si, initStr
    call .print
    mov [flpdrv], dl    ; Store boot drive for later use in floppy reads
    mov ah, 0           ; Reset floppy drive (DL should have boot device already)
    int 0x13            ; TODO: Possibly jc .floppyerror on failure here?
    mov ax, fdResetError ; set up error msg for floppy drive reset failure
    jc .generror        ; jump if carry flag bit set meaning reset failed
    
    ; es:bx should be where buffer goes for floppy read
    mov ax, STAGE2_LOAD_SEGMENT ; Load stage2 to 0x10000
    mov es, ax          ; pass segment into ES
    xor bx, bx          ; set bx to 0 for 0 offset
    mov [lbaPos], 0     ; initialize LBA position to 0    
.stage2_load:           ; == FLOPPY READING LOOP TO LOAD STAGE2 TO MEMORY ==
    mov si, dot         ; load '.' character
    call .print         ; print one '.' character per sector read
    inc word [lbaPos]   ; increment LBA position (start from 1)
    mov ax, [lbaPos]    ; lba2chs needs LBA in AX
    call lba2chs        ; convert LBA to CHS in respective registers
    push ax             ; preserve LBA position before doing floppy read
    mov ax, 0x0201      ; BIOS floppy command to load one sector at a time
    mov dl, [flpdrv]    ; restore drive number to DL just in case it was clobbered
    call .floppyread    ; read the floppy sector into ES:BX
    add bx, BYTES_PER_SECTOR         ; move buffer position for next read
    pop ax              ; restore LBA position for loop check
    cmp ax, STAGE2_RESERVED_SECTORS ; check if we've read all reserved sectors
    jb .stage2_load     ; if not, keep loading
.stage2_handoff:        ; == PREPARE TO HANDOFF TO STAGE2 ==
    mov si, newline
    call .print         ; print newline after loading stage2
    xor ax, ax          ; make DS=0 for stage2
    mov ds, ax          ;
    mov ax, STAGE2_LOAD_SEGMENT ; set ES to where stage2 is loaded
    mov es, ax          ;
.stage2_magicstringverify:
    mov di, STAGE2_HEADER_OFFSET ; magic string exists at the header offset, some N bytes into stage2 image
    mov si, magic       ; load a copy of the magic string into SI for comparison
    mov cx, 4           ; cx used for limiting length of str comparsion
    repe cmpsb          ; Iteratre strings in di and si, comparing bytewise until not equal or cx=0
    mov ax, stage2HeaderMismatchError ; set up error message for stage2 header mismatch
    jne .generror       ; if magic string doesn't match, error out
    mov ax, STAGE2_LOAD_SEGMENT ; set up segment where stage2 is loaded for printing success message
    mov ds, ax          ; set DS so segment context is ready to go for stage2
    mov dl, [flpdrv]    ; restore drive number to DL just in case it was clobbered
    jmp STAGE2_LOAD_SEGMENT:0   ; go to stage 2

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

initStr                     db 'unidos boot stage1', 0
magic                       db MAGIC_4CHAR
flpdrv                      db 0
maxRetry                    db 0x03
dot                         db '.', 0
newline                     db 13, 10, 0
errorMsg                    db 13, 10, 'error: ',0
fallbackMsg                 db 'fallback', 13, 10, 0
haltMsg                     db ', press any key to reset system',0
fdReadError                 db 'floppy read error',0
fdResetError                db 'floppy reset error',0
stage2HeaderMismatchError   db 'stage2 header mismatch',0
flpdx                       dw 0x0000
flpax                       dw 0x0000
flpretry                    db 0
lbaPos                      dw 0

times 510-($-$$) db 0       ; Zero-pad the binary to 1 sector size
dw 0xAA55                   ; Mandatory boot signature
