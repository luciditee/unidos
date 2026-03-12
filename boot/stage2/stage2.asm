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

%define ROOT_BUFFER_SEGMENT     0x6000
%define FAT_BUFFER_SEG          0x6200
%define BOUNCE_BUFFER_SEG       0x6400
%define KERNEL_LOAD_LINEAR_ADDR 0x00100000 ; 1MB, where we will load the kernel in memory

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

    ; kernel load metadata
    kernelDestLinear    dd KERNEL_LOAD_LINEAR_ADDR
    stage2EntryCS       dw STAGE2_LOAD_SEGMENT

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
    ; from here, but changing FS/GS forfeits their unreal cached limits,
    ; so we should *only* alter DS/ES whilst in unreal mode
    
    ; print message to indicate we are beginning the kernel loading 
    ; phase after unreal setup
    mov si, kernelLoadStartMsg
    call .print

    call .load_fat12_table  ; can't parse the kernel without reading FAT first
    call .parse_kernel      ; this will load the kernel and then return if successful, 
                            ; or print an error and halt if not

    ; Debug printout for now (next phase is to setup the kernel's expectations)
    mov si, char
    call .print
    mov si, newline
    call .print
    
    jmp .enterPMHandoff

.load_fat12_table:
    push ax                 ; preserve register state
    push bx
    push cx
    push es

    mov [kernelStartCluster+2], 0 ; a FAT12 cluster table is a word, not a dword, so we discard upper half

    mov ax, FAT_BUFFER_SEG  ; ES should point to where we want the FAT table loaded in memory
    mov es, ax
    xor bx, bx

    mov ax, [fatStartLba]   ; load the first LBA sector of the FAT table to ax
    mov cx, [sectorsPerFat] ; load cx with number of sectors per FAT, to be used for iteration tracking
                            ; fallthrough to read loop
.fat_read_loop:
    push ax                 ; preserve LBA sector
    push cx                 ; preserve remaining sector count
    call .read_lba_sector
    pop cx                  ; restore remaining sector count
    pop ax                  ; restore LBA sector

    add bx, BYTES_PER_SECTOR    ; advance buffer offset for next read
    inc ax                      ; increment LBA sector for next read
    loop .fat_read_loop         ; loop until cx=0, meaning we've read all FAT sectors

    pop es              ; restore registers and return
    pop cx
    pop bx
    pop ax
    ret

.parse_kernel: ; == KERNEL PARSING ENTRYPOINT ==
    mov dword [kernelDestLinear], KERNEL_LOAD_LINEAR_ADDR ; initialize kernelDestLinear
                        ; fallthrough to kernel image reading loop 
.kernel_loop:
    mov ax, [kernelRemainingBytes]
    or ax, [kernelRemainingBytes+2]
    jz .kernel_load_done

    ; note: LBA = dataStartLba + (cluster-2) * sectorsPerCluster
    mov ax, [kernelCurCluster]
    sub ax, 2

    xor dx, dx
    xor cx, cx
    mov cl, [sectorsPerCluster]
    mul cx                  ; populate ax with the right sector value for the current cluster 
                            ; following the formula ax = (cluster-2) * sectorsPerCluster
    add ax, [dataStartLba]  ; add dataStartLba to get the final LBA sector number to read for the current cluster
    ; note: in the current CHS approach we ignore DX/high LBA, but for larger disks this will need addressing

    push ax                     ; preserve LBA sector for next read
    push cx                     ; preserve sectors per cluster
    mov ax, BOUNCE_BUFFER_SEG   ; populate es with our bounce buffer
    mov es, ax                  
    xor bx, bx                  ; no offset within bounce buffer (yet)
    pop cx                      ; restore sectors per cluster/LBA sector
    pop ax
                            ; fallthrough to read cluster sectors function
.read_cluster_sectors:
    push ax                     ; preserve LBA sector for next read
    push cx                     ; preserve sectors per cluster for loop tracking
    call .read_lba_sector       ; read the current sector of the cluster into the bounce buffer
    pop cx                      ; restore sectors per cluster for loop tracking
    pop ax                      ; restore LBA sector for next iteration or post-read processing
    add bx, BYTES_PER_SECTOR
    inc ax
    mov si, dot                 ; progress bar dot
    call .print                 ; print it for each sector read
    loop .read_cluster_sectors

    call .copy_cluster_to_himem ; copy data from cluster to final destination in xms

    mov ax, [kernelRemainingBytes]  ; check if we've loaded the entire kernel
    or ax, [kernelRemainingBytes+2] ; use bitwise or to check both low and high word...
    jz .kernel_load_done    ; ...since we only care about it being 0 for this jump

    mov ax, [kernelCurCluster]
    call .fat12_next_cluster

    ; error handling
    cmp ax, 2
    jb  .kernel_load_error        ; a cluster value below 0 is invalid, so we fail on that

    cmp ax, 0xFF0
    jb  .have_next_cluster        ; a cluster in range 2..0xFEF is valid, so we go to next in chain
    cmp ax, 0xFF7
    je  .kernel_load_error        ; 0xFF7 is a bad cluster marker
    cmp ax, 0xFF8
    jb  .kernel_load_error        ; 0xFF0..0xFF6 is a reserved cluster marker

    ; if here, EOC check passed, there is no other cluster
    mov dx, [kernelRemainingBytes]
    or dx, [kernelRemainingBytes+2]
    jz .kernel_load_done
    jmp .kernel_size_mismatch   ; if we still have remaining bytes but no next cluster, the file size doesn't 
                                ; match the cluster chain length, so we error out

.have_next_cluster:
    mov [kernelCurCluster], ax   ; update current cluster to next cluster from FAT
    mov word [kernelCurCluster+2], 0 ; zero out high word since FAT12 cluster numbers are 16-bit
    jmp .kernel_loop             ; loop back to read the next cluster's worth of sectors and copy it to memory

.kernel_load_done:
    ret
.kernel_load_error:
    mov si, fdReadError
    call .print
    mov si, haltMsg
    call .print
    jmp .wait_key_reset
.kernel_size_mismatch:
    mov si, kernelPrematureEOFError
    call .print
    mov si, haltMsg
    call .print
    jmp .wait_key_reset

.copy_cluster_to_himem: ; == BOUNCE BUFFER TO HIMEM COPY PROCEDURE ==
    push ax         ; we'll be clobbering quite a few registers, so we should
    push bx         ; preserve them since the caller needs them
    push cx
    push dx
    push si
    push di
    push es

    ; determine bytes remaining to copy
    xor ax, ax
    mov al, [sectorsPerCluster]
    shl ax, 9           ; multiply by 512 to get bytes per cluster
                        ; TODO: this should probably reference BYTES_PER_SECTOR
    mov dx, ax

    mov ax, [kernelRemainingBytes+2]
    or ax, ax               ; execution no-op, but sets flags based on high dword...
    jne .use_cluster_bytes  ; if it's not zero, we have at least one cluster's worth of bytes 
                            ; left to copy, so we can skip the next part 
    mov ax, [kernelRemainingBytes] ; otherwise, we should check how many bytes remain
    cmp ax, dx              ; and see if it's less than a full cluster's worth
    jbe .use_remaining_low
                            ; fallthrough to cluster bytes copy entry
.use_cluster_bytes:
    mov cx, dx              ; if we have at least a cluster's worth of bytes, we copy the full cluster
    jmp .copy_start         ; init copy
.use_remaining_low:
    mov cx, ax              ; if we have less than a cluster's worth of bytes, we copy the remaining byte count instead
                            ; fallthrough to copy
.copy_start:
    mov ax, BOUNCE_BUFFER_SEG
    mov es, ax
    xor si, si              ; start of bounce buffer
    mov edi, [kernelDestLinear]
    cld
.copy_loop:
    mov al, [es:si]         ; load byte from bounce buffer
    mov [fs:edi], al        ; copy it to the final destination in high memory using FS segment
    inc si                  ; advance source index
    inc edi                 ; advance destination index
    loop .copy_loop         ; keep going until CX=0, with CX indicating either a full cluster's quantity
                            ; of bytes, or just the bytes remaining in the file.

    movzx eax, si                   ; si contains the number of bytes we just copied
    add [kernelDestLinear], eax     ; update kernelDestLinear with that number
    sub [kernelRemainingBytes], si  ; remaining -= copied
    sbb word [kernelRemainingBytes+2], 0 ; handle borrow for high dword

    pop es  ; restore all the registers we pushed before returning
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.fat12_next_cluster:    ; == FAT12 CLUSTER CHAIN NAVIGATION PROCEDURE ==
                        ; input: AX = current cluster
                        ; output: AX = next cluster (12 bit for FAT12)
    push bx
    push dx
    push es

    ; Get fat12 entry byte offset following formula bx = n + n/2
    mov bx, ax
    mov dx, bx
    shr dx, 1
    add bx, dx

    mov dx, ax              ; preserve parity for n in fat12 entry calculation
    mov ax, FAT_BUFFER_SEG  ; point ES to fat table buffer
    mov es, ax
    mov ax, [es:bx]         ; load the FAT entry's two bytes into ax for parsing

    test dx, 1              ; check parity of n to determine if we want the high 12 bits or 
                            ; low 12 bits of the FAT entry word
    jz .even_entry
    shr ax, 4               ; for odd n, we want the high 12 bits
    and ax, 0x0FFF          ; mask out the upper 4 bits that we don't want
    jmp .next_done
.even_entry:
    and ax, 0x0FFF          ; for even n, we want the low 12 bits, so we just mask out the upper 4 bits
                            ; fallthrough to exit case
.next_done: 
    pop es                  ; restore registers with AX now containing next cluster
    pop dx                  ; (or FAT12_EOC_MIN/FAT12_BAD if we hit those cases)
    pop bx
    ret


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
    mov [kernelStartCluster], ax            ; store starting cluster
    mov word [kernelCurCluster+2], 0        ; zero out high dword since FAT12 cluster numbers are 16-bit

    mov ax, [es:bx+FAT_OFFSET_SIZE_BYTES]
    mov [kernelSizeBytes], ax               ; size low 16 bits (bytes)
    mov ax, [es:bx+FAT_OFFSET_SIZE_BYTES+2]
    mov [kernelSizeBytes+2], ax             ; size high 16 bits (bytes)

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

.enterPMHandoff:
    cli
    cld

    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, pm32_gdt
    mov [pm32_gdtr+2], eax
    lgdt [pm32_gdtr]

    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, .pm32Entry
    mov [cs:pm32_far_ptr+0], eax         ; 32-bit offset
    mov word [cs:pm32_far_ptr+4], PM32_CODE_SELECTOR

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp dword far [cs:pm32_far_ptr]

bits 32
.pm32Entry:
    mov ax, PM32_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov esp, PM32_STACK_TOP
    cld
    cli

    xor ebx, ebx
    mov bl, byte [flpdrv]
    mov eax, KERNEL_LOAD_LINEAR_ADDR
    jmp eax

bits 16

%include "io.inc.asm"
%include "a20.inc.asm"
%include "lba2chs.inc.asm"

msg db 'S2 scaffold reached', 0
initMsg                     db 'stage2 finding kernel', 0
foundMsg                    db 'found', 13, 10, 0
kernelLoadStartMsg          db 'loading', 0
rootEndError                db 'unexpected end of root directory', 0
kernelPrematureEOFError     db 'kernel image EOF reached!',13,10,'expected more data based on file size', 0
a20Error                    db 13,10,'A 386+ CPU with A20 line support is required to run unidos', 0
char                        db 0xFB, 0
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
pm32_gdt:
    dq 0x0000000000000000      ; null
    dq 0x00CF9A000000FFFF      ; 32-bit flat code
    dq 0x00CF92000000FFFF      ; 32-bit flat data
pm32_gdt_end:

pm32_gdtr:
    dw pm32_gdt_end - pm32_gdt - 1
    dd 0

pm32_far_ptr:
    dd 0
    dw 0

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
