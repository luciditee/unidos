bits 16
org 0x0000

%include "media.asm"
%include "constants.inc.asm"

%ifndef STAGE2_RESERVED_SECTORS
%define STAGE2_RESERVED_SECTORS 4
%endif

%define TOTAL_RESERVED_SECTORS (1 + STAGE2_RESERVED_SECTORS)
%define FAT_LOAD_BUFFER_ESBX 0x6000
%define KERNEL_LOAD_DEST 0x0010
%define CODE_SELECTOR 0x08
%define DATA_SELECTOR 0x10

; Skip the header region
entry:
    jmp stage2
    nop

times STAGE2_HEADER_OFFSET-($-$$) db 0

; Header region
header:
    magic db MAGIC_4CHAR
    versionMajor db 1
    versionMinor db 0

    ; bootinfo constants
    totalSize           dw 0 ; must be filled in by stage2
    stage2Flags         dw 0 ; reserved for now
    checksum32          dd 0
    bootDrive           db 0 ; contents of DL register upon stage1 handoff
    mediaType           db 1 ; for now, always 0x01 meaning FAT12 floppy

    SectorsPerCluster   db FAT_SECTORS_PER_CLUSTER
    ReservedSectors     dw TOTAL_RESERVED_SECTORS
    FatCount            db FAT_COUNT
    RootEntries         dw ROOT_ENTRIES
    TotalSectors16      dw TOTAL_SECTORS16
    MediaDescriptor     db MEDIA_DESCRIPTOR
    SectorsPerFAT       dw SECTORS_PER_FAT
    SectorsPerTrack     dw SECTORS_PER_TRACK
    Heads               dw FLOPPY_HEADS

    ; BPB info from media.asm
    ; should match data found in stage1's BPB for sanity checking
    bytesPerSector      dw BYTES_PER_SECTOR
    sectorsPerCluster   db FAT_SECTORS_PER_CLUSTER
    fatCount            db FAT_COUNT
    reservedSectors     dw TOTAL_RESERVED_SECTORS
    rootEntryCount      dw ROOT_ENTRIES
    sectorsPerFat       dw SECTORS_PER_FAT
    sectorsPerTrack     dw SECTORS_PER_TRACK
    heads               dw FLOPPY_HEADS
    totalSectors        dd TOTAL_SECTORS16
    fatStartLba         dd TOTAL_RESERVED_SECTORS
    rootStartLba        dd TOTAL_RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
    dataStartLba        dd TOTAL_RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT + ((ROOT_ENTRIES * 32 + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR)

    ; stage2 load/handoff info
    stage2LoadPhys      dd 0x00010000 ; physical address where stage2 will be loaded (segment 0x1000, offset 0)  
    stage2EntryCS       dw STAGE2_LOAD_SEGMENT
    stage2EntryIP       dw 0x0000
    stage2ErrorCode     dw 0          ; reserved for now, can be used to pass error codes to stage2 if needed
    reserved0           dw 0

halt:
    hlt
    jmp halt

stage2:
    cli
    mov ax, cs
    mov ds, ax
    cld

    mov si, initMsg
    call .print

    ; prepare register/variable assumptions
    mov [flpdrv], dl
    xor cx, cx

    ; prepare to search for kernel binary
    mov ax, [rootStartLba]
    mov [lbaPos], ax
    mov ax, FAT_LOAD_BUFFER_ESBX
    mov es, ax
    xor bx, bx
    jmp .read_root

.enterUnreal: ; == UNREAL MODE ENTRYPOINT ==
    ; Note: We enter this label assuming A20 is set up and that we've
    ; confirmed the existence of a 386+ system.
    ; ----
    ; We want access to BIOS subroutines for convenience, but we also
    ; want to load the kernel above 1MB, allowing us to reserve conven-
    ; tional memory for other purposes later.
    ;
    ; To do this, we briefly enter protected mode, load flat data 
    ; descriptors into segment-register caches, then return to real mode
    ; while keeping expanded memory limits cached in segment registers

    cli                 ; kill interrupts for now
    xor eax, eax        ; clear all of eax
    mov ax, cs          ; put the code segment in ax, so that we can...
    shl eax, 4          ; ...shift left (*16) to get the correct offset
    add eax, gdt        ; add the gdt pointer to it since it is the
                        ; offset within CS segment. GDTR needs full
                        ; linear base address, not segment:offset

    ; GDTR.base must be a linear/physical address, not a segment-relative
    ; offset. In flat real mode at this point, linear==physical.
    mov [gdtr+2], eax

    ; Load GDT descriptor (limit+base) prepared above
    lgdt [gdtr]

    ; Enable protected mode by setting CR0.PE (bit 0).
    mov eax, cr0        ; get CR0
    or eax, 1           ; set first bit
    mov cr0, eax        ; put it back. We are now in protected mode

    ; Far jump is required to load CS from GDT and flush prefetch queue.
    jmp CODE_SELECTOR:.pm16_entry

.pm16_entry: ; == PROTECTED-MODE STAGING ENTRY ==
    ; CS now uses CODE_SELECTOR descriptor. We only load FS/GS with flat 
    ; DATA_SELECTOR, NOT DS/ES, to keep DS/ES unchanged (16-bit conventional)
    ; for real-mode BIOS/string usage later on
    ;
    ; Reloading DS/ES here with flat selector can break assumptions in
    ; RM code that expects stage2 data labels via DS=CS semantics.
    mov ax, DATA_SELECTOR
    mov fs, ax
    mov gs, ax

    ; Clear CR0.PE to return to real mode.
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    ; Build a far pointer for RM return target:
    ; - offset = rm16_entry
    ; - segment = stage2EntryCS (expected 0x1000)
    mov ax, [stage2EntryCS]
    mov [cs:rm_far_ptr+2], ax
    jmp far [cs:rm_far_ptr]
    
.rm16_entry: ; == REAL-MODE RETURN POINT AFTER UNREAL SETUP ==
    ; At this point, CPU mode is real mode again (CR0.PE=0), FS/GS
    ; hidden descriptor caches remain expanded from PM load, and
    ; BIOS interrupts are callable again
    ;
    ; DS/ES may be interacted with and are intentionally kept 16-bit
    ; from here, but changing any other segment register forfeits
    ; those segments' access to upper memory.
    ; print newline and message to indicate we are beginning the
    ; kernel loading phase after unreal setup
    mov si, kernelLoadStartMsg
    call .print
    jmp .loadkernel

.read_root:
    mov ax, [lbaPos]    ; load LBA position to ax
    call lba2chs        ; Convert LBA to CHS in CH, DH, CL. DL clobbered.
    mov dl, [flpdrv]    ; Restore drive number to DL for BIOS calls
    mov ax, 0x0201      ; BIOS command: read 1 sector into ES:BX
    call .floppyread    ; Read the current sector of FAT root directory
    add bx, 0x200       ; Advance mem buffer by 1 sector (0x200 or 512 bytes)
    mov ax, [lbaPos]    ; load LBA position to ax
    inc ax              ; increment it
    mov [lbaPos], ax    ; copy it back
    mov si, dot         ; progress bar dot
    call .print         ; print it for each sector read
    mov ax, [rootStartLba]
    add ax, ROOT_DIR_SECTORS
    cmp [lbaPos], ax    ; have we read all sectors in the root directory? 
    jne .read_root       ; If not, loop until we are.
                        ; Fall through to root parser
.parse_root:
    push bx             ; preserve buffer offset for later use
    xor bx, bx          ; start at beginning of buffer again for parsing
    xor ax, ax          ; clear ax for use in parsing loop
.parse_root_loop:
    mov al, [es:bx]     ; load first byte of directory entry
    cmp al, 0           ; is it null? if so, we've hit the end of the root dir entries
    je .unexpected_root_end
    cmp al, 0xE5        ; E5 = deletion marker, so skip if we find one
    je .next_root_entry
    mov al, [es:bx+11]  ; check attribute byte for long filename entries
    cmp al, 0x0F        ; 0x0F means it's a long filename entry, which we also want to skip
    je .next_root_entry
    test al, 0x08
    jnz .next_root_entry ; Volume label entry, not a file, so skip if we find one
    test al, 0x10
    jnz .next_root_entry ; Subdirectory entry, not a file, so we skip
    ;mov si, FAT_LOAD_BUFFER_PHYS ; point SI to the start of the root buffer
    call .match_kernel      ; Check if this is the stage 2 file we want to load. If so, we'll jump to it and never come back here, so no need to clean up the stack after this call.
    jz .kernel_found        ; If it is the stage 2 file, jump to the code to handle that case
    ; old test: print filename
    ;add si, bx          ; add the current offset to get to the filename
    ;call .print         ; print the filename (will print garbage currently since we haven't implemented a proper print routine, but we should see progress if it's working at all)
    ;mov si, newline
    ;call .print         ; print a newline after the filename
.next_root_entry:
    add bx, 0x20        ; advance to next directory entry (32 bytes)
    cmp bx, ROOT_DIR_SECTORS*512 ; have we parsed all entries in the root directory buffer?
    jb .parse_root_loop ; if not, loop back to parse the next entry
.unexpected_root_end:
    pop bx              ; restore bx if we need it later (probably not at this point, but just in case)
    mov si, rootEndError
    call .print
    jmp halt

.kernel_found:          ; if jumped to here, kernel was found and we can stage loading it
    mov ax, [es:bx+FAT_OFFSET_START_CLUSTER]
    mov [kernelStartCluster], ax           ; store starting cluster

    mov ax, [es:bx+FAT_OFFSET_SIZE_BYTES]
    mov [kernelSizeBytes], ax              ; size low 16 bits (bytes)
    mov ax, [es:bx+FAT_OFFSET_SIZE_BYTES+2]
    mov [kernelSizeBytes+2], ax            ; size high 16 bits (bytes)

    ; Initialize remaining byte counter from discovered file size
    mov ax, [kernelSizeBytes]
    mov [kernelRemainingBytes], ax
    mov ax, [kernelSizeBytes+2]
    mov [kernelRemainingBytes+2], ax

    ; Initialize current cluster to starting cluster
    mov ax, [kernelStartCluster]
    mov [kernelCurCluster], ax

    ; We now know what the starting cluster and size of the kernel are
    ; Print a tmessage to indicate success
    pop bx
    mov si, foundMsg
    call .print

    jmp .initA20 ; this is a 32-bit kernel, so we need A20 gate set up

.loadkernel:
    ; this will be replaced with loader logic
    hlt
    jmp .loadkernel


%if 0
.parse_kernel:
    mov ax, KERNEL_LOAD_SEGMENT
    mov es, ax          ; Set ES to the segment we want to load stage 2 into
    xor bx, bx          ; Offset of 0
    mov ax, [kernelStartCluster]    ; Load up the starting cluster
    mov [kernelCurCluster], ax      ; Copy it into the current cluster variable
.kernel_load_loop:
    sub ax, 2                       ; Clusters offset from 2, so we subtract 2
    mov cx, FAT_SECTORS_PER_CLUSTER ; multiply by sectors-per-cluster for current profile
    mul cx
    add ax, DATA_START              ; Add the data start offset to get the final LBA value
    mov [lbaPos], ax                ; Store the LBA of the current cluster as AX will soon be clobbered
    call .read_lba_sector           ; Read the next cluster of stage 2 into memory
    add bx, 512*FAT_SECTORS_PER_CLUSTER ; Advance the offset in ES:BX by bytes read this cluster
    mov ax, [kernelRemainingBytes]  ; Load up the number of bytes we have left to read
    sub ax, 512*FAT_SECTORS_PER_CLUSTER ; Subtract the number of bytes we just advanced by
    mov [kernelRemainingBytes], ax  ; Store the updated number of bytes remaining
    cmp ax, 0
    jg .kernel_load_loop               ; If we still have more bytes to read, loop back and read the next cluster

    ; load loop done

    jmp halt            ; halt after parsing for now
%endif

.read_lba_sector:
    ; input: AX = LBA sector number, DL = drive number (AX clobbered)
    call lba2chs        ; Convert LBA to CHS in CH, DH, CL. DL clobbered.
    mov dl, [flpdrv]    ; Restore drive number to DL for BIOS calls
    mov ax, 0x0201      ; BIOS command: read 1 sector into ES:BX
    call .floppyread    ; Read the specified sector into ES:BX
    ret

.match_kernel:          ; == KERNEL MATCHING FUNCTION ==
                        ; Note: Can't use pusha/popa as we need the result of cmpsb preserved
    push si             ; preserve si,di,cx for string scanning/printing
    push di             
    push cx             
    mov si, kernelfn    ; load up the kernel 11-byte name
    mov di, bx          ; point it to where we expect the FAT-loaded filename to be in memory
    mov cx, 11          ; cap the search at 11-byte filename length
    repe cmpsb          ; compare DS:SI to ES:DI as strings
                        ; note: ZF=1 if matching, e.g. use jz if match
    pop cx              ; restore registers pushed earlier
    pop di
    pop si
    ret

%include "io.inc.asm"
%include "a20.inc.asm"
%include "lba2chs.inc.asm"

msg db 'S2 scaffold reached', 0
initMsg                     db 'stage2 finding kernel', 0
foundMsg                    db 'found', 13, 10, 0
kernelLoadStartMsg          db 'loading', 0
rootEndError                db 'unexpected end of root directory', 0
a20Error                    db 13,10,'A 386+ CPU with A20 line support is required to run unidos', 0
char                        db 'X', 0
dot                         db '.', 0
kernelSizeBytes             dd 0
kernelStartCluster          dd 0
kernelCurCluster            dd 0
kernelRemainingBytes        dd 0
kernelfn                    db 'KERNEL  BIN' ; 11-byte filename for kernel
flpdrv                      db 0
maxRetry                    db 0x03
newline                     db 13, 10, 0
errorMsg                    db 13, 10, 'error: ',0
fallbackMsg                 db 'fallback', 13, 10, 0
haltMsg                     db ', press any key to reset system',0
fdReadError                 db 'floppy read error',0
fdResetError                db 'floppy reset error',0
flpdx                       dw 0x0000
flpax                       dw 0x0000
flpretry                    db 0
lbaPos                      dw 0

align 8
gdt:
    dq 0x0000000000000000
    ; code: base=0x00010000, limit=0xFFFF, 16-bit, present
    dq 0x00009A010000FFFF
    ; data flat for unreal (base 0, 4GiB-1)
    dq 0x00CF92000000FFFF
gdt_end:

gdtr:
    ; == GDTR (for LGDT) ==
    ; 6-byte pseudo-descriptor:
    ;   [0-1] = GDT limit (size-1)
    ;   [2-5] = GDT base linear address
    ;
    ; Base is patched at runtime in .enterUnreal because stage2 can be
    ; relocated and we need the actual runtime linear address of gdt
    dw gdt_end - gdt - 1
    dd 0                       ; runtime-filled base

rm_far_ptr:
    ; == FAR POINTER USED TO RETURN TO REAL MODE CODE ==
    ; Layout expected by jmp far in 16-bit mode is 4 bytes:
    ;   [bytes 0-1] offset, [bytes 2-3] segment
    ;
    ; Offset is fixed to rm16_entry; segment is patched at runtime from
    ; stage2EntryCS so the same binary can return correctly regardless of
    ; load segment assumptions.
    dw stage2.rm16_entry
    dw 0  

times (BYTES_PER_SECTOR*STAGE2_RESERVED_SECTORS)-($-$$) db 0
