bits 16
org 0x0000

%include "media.asm"
%include "constants.inc.asm"

%ifndef STAGE2_RESERVED_SECTORS
%define STAGE2_RESERVED_SECTORS 7
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
%define KPARAMS_BUFFER_LINEAR   0x00000700
%define KPARAMS_MAX_LEN         255

%define FAT12_MIN_VALID     0x002
%define FAT12_RESERVED_LO   0xFF0
%define FAT12_BAD           0xFF7
%define FAT12_EOC_MIN       0xFF8


%ifndef KERNEL_CHECKSUM
    ; Unless we get insanely lucky, this will not match the kernel's actual
    ; CRC32 checksum. This value is intended to be replaced by the build 
    ; system by calculating the kernel's CRC32 and passing to NASM via -D
    %define KERNEL_CHECKSUM 0x24201103
%endif

%macro TEST_DWORD_ZERO 1
    mov ax, word [%1]
    or ax, word [%1+2]
%endmacro

%macro PRINT_DOT 0
    mov si, dot
    call .print
%endmacro

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
    stage2EntryCS       dw STAGE2_LOAD_SEGMENT
    kernelDestLinear    dd KERNEL_LOAD_LINEAR_ADDR
    kernelSizeBytes     dd 0
    kparamsLinearAddr   dd 0
    kparamsSizeBytes    dd 0

    ; system memory metadata
.e801:
    e801MemorySize      dd 0
.ah88Mem:
    ah88MemorySize      dd 0
.cmosMem:
    cmosMemorySize      dd 0
.e820:
    e820EntrySize       dw 24
    e820EntryCount      dw 0
    e820Length          dw 0
    e820Data            times (24 * 8) db 0
    
bootinfo_end:

; bootinfo constants
%define BOOTINFO_DEST_OFFSET    0x0500 ; linear/physical address
%define BOOTINFO_SIZE           (bootinfo_end - header)

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
    ; todo: if these are pre-defined with a constant start value,
    ; we can probably save some bytes and skip these
    mov [flpdrv], dl
    xor cx, cx
    mov byte [kernelFound], 0
    mov byte [kparamsFound], 0
    mov dword [kparamsFileSize], 0
    mov dword [kparamsSizeBytes], 0
    mov dword [kparamsLinearAddr], 0

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

.get_e820:
    pushad              ; push all GP registers
    push ds             ; push DS/ES
    push es

    ; default: no E820 data available
    and word [stage2Flags], 0xFFFC  ; by default, we want the lower 2 bits to be 0
    mov word [e820EntryCount], 0    ; init entry count and length to 0
    mov word [e820Length], 0

    xor ebx, ebx                    ; continuation token for E820 (must start at 0)
    mov di, e820Data                ; destination offset within CS/DS segment
    mov bp, 8                       ; max entries that fit in bootinfo buffer

.e820_next:
    mov ax, cs                      ; CS for ES:DI
    mov es, ax                      ; E820 writes to ES:DI
    mov dword [es:di+20], 1         ; request valid extended attrs when ECX=24

    mov eax, 0xE820                 ; load command
    mov edx, 0x534D4150             ; 'SMAP' constant--required for E820 command
    mov ecx, 24                     ; request ACPI 3.0-sized descriptor
    int 0x15                        ; call bios
    jc .e820_done                   ; unsupported or failed; keep any entries gathered so far

    cmp eax, 0x534D4150             ; if EDX returns 'SMAP' we are done
    jne .e820_done

    cmp cx, 20                      ; 20-byte legacy minimum
    jb .e820_done                   

    add di, 24
    inc word [e820EntryCount]       ; increment count for each entry
    add word [e820Length], 24       ; keep track of total length by adding size of each

    dec bp                          ; move base pointer until 0
    jz .e820_done                   ; if 0, we're done

    test ebx, ebx                   ; EBX==0 means end of map
    jnz .e820_next

.e820_done:
    cmp word [e820EntryCount], 0    ; if we got no entries, we don't have e820 data
    je .e820_exit                   ; skip if so
    or word [stage2Flags], 0x0001   ; otherwise, lowest 2 bits: 01: E820 map present

.e820_exit:
    pop es          ; restore regs
    pop ds
    popad

    mov ax, [stage2Flags]   ; check if we got E820 data for skipping next phases
    test ax, 0x0001 ; did we get E820 data?
    jnz .resumeEnterUnreal ; if so, we can skip E801/AH88 methods

.e801_entry:
    pushfd
    pushad

    xor eax, eax        ; clear eax and ebx so the higher word is zeroed
    xor ebx, ebx

    mov ax, 0xE801      ; set e801 command
    int 0x15            ; call BIOS
    jc .e801_exit       ; if carry flag set, command failed

    add ax, 0x400       ; ax will contain the quantity of memory up to 16MB 
                        ; but disregards the first 1MB, so we add 1MB back.
                        ; we're limited to 16MB with this method, so we need
                        ; to check the 64k block math as well
    shl ebx, 6          ; bx contains memoryabove 16MB in 64k blocks, so we * 64
                        ; to get memory in kilobytes (done as dword so we can
                        ; safely overflow out of bx)
    add eax, ebx        ; add the two values together to get total memory in KB
    test eax, eax       ; did we get a zero? some BIOSes report with CX/DX instead
                        ; so we repeat the above process with CX/DX if so
    jnz .e801_save      ; otherwise, go ahead and save the value
                        ; fallthrough to CX/DX case

    xor eax, eax        ; clear EAX/EBX for CX/DX case
    xor ebx, ebx
    mov ax, cx          ; copy CX/DX to AX/BX to make this method workable
    mov bx, dx          
    add ax, 0x400       ; offset by 1MB since we only got memory above 1MB with int15h
    shl ebx, 6          ; convert 64k blocks to KB in EBX for the same reason as above
    add eax, ebx        ; add together for total KB

    test eax, eax       ; check for 0 value
    jz .e801_exit       ; if we still got zero, we failed to get memory info with this method, so exit
.e801_save:
    mov [e801MemorySize], eax   
    mov ax, [stage2Flags]       ; set flag
    or ax, 0x0002               ; 10: E801 data present
    mov [stage2Flags], ax
    
.e801_exit:
    popad
    popfd
    
    mov ax, [stage2Flags]
    test ax, 0x0003     ; have we found memory info from either E820 or E801?
    jnz .resumeEnterUnreal ; if so, we can skip the AH=0x88 method

.ah88_entry:
    pushad
    pushfd

    xor eax, eax
    mov ah, 0x88
    int 0x15
    jc .ah88_exit
    movzx eax, ax        ; ax contains memory in KB, but we want to store as dword, 
                         ; so zero-extend into eax
    add eax, 0x400       ; like E801, this returns memory above 1MB in KB, 
                         ; but disregards the first 1MB, so we add it back
    mov [ah88MemorySize], eax
    mov ax, [stage2Flags]    
    or ax, 0x0003      ; 11: memory info present from ah88
    mov [stage2Flags], ax
.ah88_exit:
    popfd
    popad

    mov ax, [stage2Flags]
    test ax, 0x0003     ; check if we got memory info from E820 or E801 methods
    jnz .resumeEnterUnreal ; if we got any memory info from the above methods, we can skip synthetic method and proceed with unreal mode setup
                        ; fallthrough to CMOS method if not found
.cmosMemoryTest:
    ; As a last resort, we try to get memory info from the CMOS, which is extremely 
    ; unreliable and often under-reports available memory, but it's better than nothing
    ; on really old systems that don't support the above methods. We won't set any 
    ; flags for this method since it's not really a "bootinfo method" per se, just a 
    ; last-ditch effort to get some kind of memory info before giving up and using 
    ; synthetic defaults.
    push dx
    xor eax, eax

    %if 0
    ; Can be used as a sanity check, but is not necessary
    mov al, 0x15        ; CMOS base memory size low bytes
    out 0x70, al
    in al, 0x71
    mov dl, al

    mov al, 0x16        ; CMOS base memory size high bytes
    out 0x70, al
    in al, 0x71
    mov dh, al
    %else
    ; Otherwise, we can just skip the base memory size check
    ; and instead assume a default of 1024K for conventional/BIOS
    ; memory
    mov dx, 0x400       ; assume base memory is 640K + 384K for BIOS
    %endif

    push dx             ; dx now hold base memory size, push it

    mov al, 0x17        ; CMOS extended memory size low bytes
    out 0x70, al
    in al, 0x71
    mov dl, al 

    mov al, 0x18        ; CMOS extended memory size high bytes
    out 0x70, al
    in al, 0x71
    mov dh, al

    pop ax              ; restore old DX value into AX
    add ax, dx          ; add base and extended memory together for total memory in KB

    cmp dx, 0           ; however, there's a chance we didn't get any extended info, so we
                        ; try the alternate port
    jnz .cmos_done       ; if we got some value in dx, we can skip the alternate port method
    
    mov al, 0x30        ; alt port for extended memory size low byte
    out 0x70, al
    in al, 0x71
    mov dl, al
    mov al, 0x31        ; alt port for extended memory size high byte
    out 0x70, al
    in al, 0x71
    mov dh, al
    add ax, dx          ; add alternate extended memory to base memory in ax for total memory in KB
.cmos_done:
    mov [cmosMemorySize], ax ; store CMOS memory size in bootinfo for potential use by kernel
    pop dx

%if 0   ; uncomment to prevent systems without E820/E801/AH88 memory info from proceeding
        ; (otherwise, kernel will use synthetic memory info determined after stage2)
    mov ax, [stage2Flags]
    test ax, 0x0003     ; did any memory method succeed in getting us memory info?
    jnz .resumeEnterUnreal  ; if so, proceed with the rest of unreal mode setup
    mov si, memError        ; otherwise, print error
    call .print
    mov si, haltMsg
    call .print
    jmp .wait_key_reset
%endif

.resumeEnterUnreal:
    ; set up temporary gdt we'll need when temporarily entering pmode
    xor eax, eax        ; clear all of eax
    mov ax, cs          ; put the code segment in ax, so that we can...
    shl eax, 4          ; ...shift left (*16) to get the correct offset
    add eax, gdt        ; add the gdt pointer to it since it is the
                        ; offset within CS segment. GDTR needs full
                        ; linear base address, not segment:offset

    ; GDTR.base must be a linear/physical address, not a segment-relative
    ; offset. In real mode with no paging, linear==physical.
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
    call .crc32_finalize    ; verify kernel integrity
    call .load_kparams_if_present ; optional kparams load for kernel cmdline
    call .publish_bootinfo  ; kernel expects bootinfo to be copied somewhere

    ; Print checkmark (sqrt character) to indicate load success
    mov si, checkmark
    call .print
    mov si, newline
    call .print
    
    jmp .enterPMHandoff

.publish_bootinfo:
    push ax             ; preserve register state
    push cx
    push ds
    push si
    push di
    push es

    mov ax, cs          ; we're using labels to note offsets, so we need CS/DS to be
    mov ds, ax          ; known and equivalent

    mov word [totalSize], BOOTINFO_SIZE ; patch dynamic bootinfo fields
    mov al, [flpdrv]                    ; copy boot drive into bootinfo
    mov [bootDrive], al                 
    mov eax, [kernelCrcRuntime]         ; copy calculated crc32 into bootinfo
    xor eax, 0xFFFFFFFF                 ; invert bits to get final CRC32 value
    mov [checksum32], eax

    xor ax, ax                          ; clear ax
    mov es, ax                          ; set ES=0 for absolute addressing of bootinfo destination
    mov di, BOOTINFO_DEST_OFFSET        ; set destination index to absolute address we want to copy to
    mov si, header                      ; set source index to start of bootinfo header
    mov cx, BOOTINFO_SIZE               ; set number of bytes to copy
    cld                                 ; clear direction flag for sanity
    rep movsb                           ; do the copy until CX=0

    pop es              ; restore registers and return
    pop di
    pop si
    pop ds
    pop cx
    pop ax
    ret

.load_fat12_table:
    push ax                 ; preserve register state
    push bx
    push cx
    push es

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
    mov dword [kernelLoadCursor], KERNEL_LOAD_LINEAR_ADDR ; initialize kernelLoadCursor
    mov dword [kernelCrcRuntime], 0xFFFFFFFF    ; crc32 initial value is always uint32_max
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
    PRINT_DOT
    loop .read_cluster_sectors

    call .copy_cluster_to_himem ; copy data from cluster to final destination in xms

    TEST_DWORD_ZERO kernelRemainingBytes    ; have we loaded the whole kernel?
    jz .kernel_load_done                    ; if so, exit

    mov ax, [kernelCurCluster]
    call .fat12_next_cluster

    ; error handling
    cmp ax, FAT12_MIN_VALID
    jb  .kernel_load_error        ; a cluster value below 2 is invalid, so we fail on that

    cmp ax, FAT12_RESERVED_LO
    jb  .have_next_cluster        ; a cluster in range 2..0xFEF is valid, so we go to next in chain
    cmp ax, FAT12_BAD
    je  .kernel_load_error        ; 0xFF7 is a bad cluster marker
    cmp ax, FAT12_EOC_MIN
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

.kernel_load_done:                  ; no more clusters to read
    ret
.kernel_load_error:                 
    mov si, fdReadError             ; a number of things can go wrong, usually bad sectors or exhausting retries
    call .print                     ; show error message/reset message
    mov si, haltMsg
    call .print
    jmp .wait_key_reset             
.kernel_size_mismatch:
    mov si, kernelPrematureEOFError ; if we stopped traversing clusters early, we can't be certain the kernel loaded
    call .print                     ; so here, we also print an error/reset message
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
    or ax, ax               ; execution no-op, but sets flags based on high word...
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
    mov edi, [kernelLoadCursor]
    cld
.copy_loop:
    mov al, [es:si]         ; load byte from bounce buffer
    mov [fs:edi], al        ; copy it to the final destination in high memory using FS segment

    ; CRC32 calculation on-the-fly as we copy bytes to himem
    mov dl, al                  ; feed the byte into the CRC32 calculation for runtime integrity verification   
    mov eax, [kernelCrcRuntime] ; get previous crc value
    xor al, dl                  ; xor old crc value against most recent byte

%rep 2
    ; 2 rounds of this loop per byte copied to process low and high nibble
    mov edx, eax                ; isolate low nibble for table lookup
    and edx, 0x0F               ; mask to get index for low nibble
    shr eax, 4                  ; shift eax right to bring high nibble into low nibble position
    xor eax, [crc32Table+edx*4] ; xor against crc32 table value
%endrep

    mov [kernelCrcRuntime], eax ; store updated crc value back for next round

    inc si                  ; advance source index
    inc edi                 ; advance destination index
    loop .copy_loop         ; keep going until CX=0, with CX indicating either a full cluster's quantity
                            ; of bytes, or just the bytes remaining in the file.

    movzx eax, si                   ; si contains the number of bytes we just copied
    add [kernelLoadCursor], eax     ; update kernelLoadCursor with that number
    sub [kernelRemainingBytes], si  ; remaining -= copied
    sbb word [kernelRemainingBytes+2], 0 ; handle borrow for high word

    pop es  ; restore all the registers we pushed before returning
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.crc32_finalize:
    call .should_skip_crc32             ; did user hold F8 key to skip?
    jc .crc32_skipped

    mov eax, [kernelCrcRuntime]         ; get the calculated CRC32 value
    xor eax, 0xFFFFFFFF                 ; invert all the bits to get final value
    cmp eax, [kernelCrc32]              ; compare it to the value emitted at compile time
    je .crc32_ok                        ; indicate success by returning
    mov si, kernelCrcMismatchError      ; if here, we mismatched, so kernel may be bad
    call .print                         ; display error message

    mov si, haltMsg                     ; prompt user to reset
    call .print                 
    jmp .wait_key_reset

.crc32_skipped:
    mov si, kernelCrcSkipMsg            ; if here, user chose to skip crc32 check, so indicate that
    call .print
    push eax
    mov ax, [stage2Flags]
    or ax, 0x0004 ; set bit2 to indicate we skipped crc32 check
    mov [stage2Flags], ax
    pop eax
.crc32_ok:
    ret

.load_kparams_if_present:
    push ax     ; preserve registers since we'll be clobbering a lot
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    ; initialize variables to default "not found" state
    mov dword [kparamsSizeBytes], 0 
    mov dword [kparamsLinearAddr], 0
    mov word [kparamsLoadedLen], 0

    ; check if we had previously found a kparams file in root dir
    cmp byte [kparamsFound], 1
    jne .kparams_none

    ; remaining = min(kparams file size, KPARAMS_MAX_LEN)
    mov ax, [kparamsFileSize+2] ; check high word of file size first since if it's nonzero, it's definitely > KPARAMS_MAX_LEN
    or ax, ax                   ; execution no-op, but sets flags based on high word...
    jne .kparams_clamp          ; ...because jne consumes flags, so if high word is nonzero, we jump
                                ; to clamp remaining to KPARAMS_MAX_LEN
    mov ax, [kparamsFileSize]   ; check if the file size is below or equal to max
    cmp ax, KPARAMS_MAX_LEN
    jbe .kparams_set_remaining  ; if file size is already below max, no need to clamp
.kparams_clamp:
    mov ax, KPARAMS_MAX_LEN     ; otherwise, we set remaining to the max allowed value to avoid overflow issues
.kparams_set_remaining:
    mov [kparamsRemainingBytes], ax         ; set low word of remaining to file size or max, whichever is smaller
    mov word [kparamsRemainingBytes+2], 0   ; ignore high word because fat12 won't use it

    mov ax, [kparamsStartCluster]           ; set current cluster to starting cluster
    mov [kparamsCurCluster], ax             ; set current cluster to starting cluster
    mov word [kparamsCurCluster+2], 0       ; again, ignore high word vis-a-vis fat12

    mov di, KPARAMS_BUFFER_LINEAR   ; destination index should be our absolute/linear buffer address
    mov ax, [kparamsRemainingBytes] ; check if we have any bytes to copy now
    or ax, [kparamsRemainingBytes+2]; if there's any 1's in the low or high word, there's bytes to copy
    jz .kparams_finalize            ; if not, we can skip straight to finalization since there's nothing to copy

.kparams_cluster_loop:
    ; LBA = dataStartLba + (cluster-2) * sectorsPerCluster
    mov ax, [kparamsCurCluster]     ; get the current cluster number
    sub ax, 2                       ; adjust for FAT12 cluster numbering being >2

    xor dx, dx                      ; we're going to need dx and cx later
    xor cx, cx                      ; zero cx before we load sectorsPerCluster into cl, since it's only a byte value and we want to avoid garbage in high byte
    mov cl, [sectorsPerCluster]     ; cl is for counting how many sectors left to scan
    mul cx                          ; ax *= sectorsPerCluster
    add ax, [dataStartLba]          ; add the starting LBA to get the true LBA we want to read from

    ; read current cluster into bounce buffer
    push ax                         ; preserve LBA for read function
    ;push cx                         ; preserve sectors per cluster for read function
    mov ax, BOUNCE_BUFFER_SEG       ; set up ES for bounce buffer segment
    mov es, ax
    xor bx, bx                      ; no offset at this time
    ;pop cx                          ; restore cx (todo: probably can remove this push/pop pair)
    pop ax                          ; restore LBA

.kparams_read_cluster:
    push ax                         ; preserve LBA for read function
    push cx                         ; preserve sectors per cluster for read function
    call .read_lba_sector           ; LBA sector read uses lba2chs which clobbers ax/cx
    pop cx                          ; restore sectors per cluster
    pop ax                          ; restore LBA for next iteration
    add bx, BYTES_PER_SECTOR        ; increment buffer offset by one sector's worth
    inc ax                          ; increment LBA to keep lba/sector read in sync
    loop .kparams_read_cluster      ; repeat until cx=0

    ; bytes_to_copy = min(remaining, cluster_bytes)
    xor ax, ax                      ; clear ax
    mov al, [sectorsPerCluster]     ; load sectors per cluster
    shl ax, 9                       ; multiply by 512 (bytes per sector) (TODO: should probably tie this to BYTES_PER_SECTOR constant)
    mov dx, ax                      ; copy sectors per cluster to dx

    mov cx, [kparamsRemainingBytes] ; load remaining bytes to cx for comparison
    cmp cx, dx                      ; if remaining bytes is less than a full cluster, we should only copy the remaining byte count
    jbe .kparams_have_count         
    mov cx, dx                      ; otherwise, we have at least a cluster's worth of bytes left, so we copy the whole cluster
.kparams_have_count:
    push cx                         ; preserve byte count for copy function

    ; movsb copies DS:SI -> ES:DI
    mov ax, BOUNCE_BUFFER_SEG       ; data segment should point to bounce buffer
    mov ds, ax                      
    xor si, si                      ; clear source index

    xor ax, ax                      ; clear destination segment for linear addressing
    mov es, ax                      ; destination is linear 0x0000:DI (kparams buffer)
    cld
    rep movsb                       ; copy cluster data from bounce buffer to final buffer

    ; IMPORTANT: restore stage2 data segment before metadata updates
    mov ax, cs                      ; restore data segment
    mov ds, ax

    pop ax                          ; restore copy byte count
    add [kparamsLoadedLen], ax      ; update loaded length by how many bytes we just copied
    sub [kparamsRemainingBytes], ax ; remove that amount from low word
    sbb word [kparamsRemainingBytes+2], 0   ; doesn't do anything in fat12, but kept here for fat16/32 support later

    mov cx, [kparamsRemainingBytes] ; if low or high word contains any 1's, we still have bytes to load
    or cx, [kparamsRemainingBytes+2]
    jz .kparams_finalize            ; if not, we can finish up

    mov ax, [kparamsCurCluster]     ; repopulate the current cluster into the cluster traversal code
    call .fat12_next_cluster        ; advance to the next cluster

    cmp ax, FAT12_MIN_VALID         ; if the next cluster value is below 2, it's invalid, so we disable kparams
    jb  .kparams_disable

    cmp ax, FAT12_RESERVED_LO       ; if the next cluster value is in the reserved range, it's invalid
    jb  .kparams_have_next          ;
    cmp ax, FAT12_BAD               ; if the next cluster value is 0xFF7, it's a bad cluster, so we can't trust kparams
    je  .kparams_disable            ;
    cmp ax, FAT12_EOC_MIN           ; if the next cluster value is below the EOC minimum, it's invalid
    jb  .kparams_disable

    ; EOC reached before desired byte count. Keep what we loaded.
    jmp .kparams_finalize
.kparams_have_next:
    mov [kparamsCurCluster], ax         ; update current cluster to next cluster from FAT
    mov word [kparamsCurCluster+2], 0   ; drop high word in fat12
    jmp .kparams_cluster_loop           ; continue/repeat cluster load routine

.kparams_finalize:
    xor ax, ax                  ; if we got here, we should finalize whatever was loaded and null-terminate
    mov es, ax                  ; set ES=0 to write null-terminator at absolute lowest buffer address
    mov byte [es:di], 0         ; no offset

    ; now write metadata in stage2 segment
    mov ax, cs                  ; copy metadata to stage2
    mov ds, ax                  ; CS = DS for this copy
    mov dword [kparamsLinearAddr], KPARAMS_BUFFER_LINEAR    ; put the linear address for kparams into scratch
    mov ax, [kparamsLoadedLen]          ; put the size of kparams we loaded into ax
    mov [kparamsSizeBytes], ax          ; copy it to scratch
    mov word [kparamsSizeBytes+2], 0    ; ignore high word in fat12
    jmp .kparams_done                   ; jump to common exit for both success and "not found" cases

.kparams_disable:
    mov dword [kparamsLinearAddr], 0    ; nullptr for disabled kparams
    mov dword [kparamsSizeBytes], 0     ; size 0 for no kparams

.kparams_none:
    xor ax, ax                          ; zero out first byte of kparams buffer if not found
    mov ds, ax                          ;
    mov byte [KPARAMS_BUFFER_LINEAR], 0 ; 

.kparams_done:
    pop es          ; restore register state and return
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.should_skip_crc32:
    push ax             ; preserve ax
.scan_f8:
    xor ax, ax          ; zero it out
    mov ah, 1           ; BIOS command AH=1 -- check for keystroke
    int 0x16            ; if ZF=1 after int16, no keystroke pending
    jz .no_skip         ; no keystroke, escape skip check
                        ; if past jz, key is pressed and needs dequeueing
    mov ah, 0           ; command AH=0 -- read keystroke (nonblocking, already checked key event availability by now)
    int 0x16            ; int16 retrieves keystroke into AX

    cmp al, 0           ; if AL=0, we know it's an extended/non-ASCII key
    jne .scan_f8        ; if not zero, it's a normal key (jump to start and retry)
    cmp ah, 0x42        ; check for AH=0x42, aka the F8 key
    je .skip            ; jump to skip if F8 was enqueued
    jmp .scan_f8        ; otherwise retry (in practice, releasing an unknown key unblocks
                        ; the scan and the CRC check will still run due to users
                        ; being unlikely to press F8 mere (milli|micro)seconds later)
.skip:
    stc                 ; set carry flag to indicate skip
    pop ax              ; restore register state
    ret
.no_skip:
    clc                 ; unset carry flag to indicate no skip
    pop ax              ; pop ax
    ret                 

.fat12_next_cluster:    ; == FAT12 CLUSTER CHAIN NAVIGATION PROCEDURE ==
                        ; input: AX = current cluster
                        ; output: AX = next cluster value (12 bit for FAT12)
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
    PRINT_DOT           ; print progress dot for each sector read
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
    je .root_scan_done
    cmp al, 0xE5        ; E5 = deletion marker, so skip if we find one
    je .next_root_entry
    mov al, [es:bx+11]  ; check attribute byte for long filename entries
    cmp al, 0x0F        ; 0x0F means it's a long filename entry, which we also want to skip
    je .next_root_entry
    test al, 0x08
    jnz .next_root_entry ; Volume label entry, not a file, so skip if we find one
    test al, 0x10
    jnz .next_root_entry ; Subdirectory entry, not a file, so we skip
    call .match_kernel
    jnz .check_kparams   ; differentiate between kparams/kernel/other files
    cmp byte [kernelFound], 1   
    je .check_kparams           
    call .capture_kernel_entry  ; get the entrypoint of the kernel for loading later

.check_kparams:
    call .match_kparams         ; match against the filename of the kparams file
    jnz .next_root_entry        ; if it doesn't match, skip to the next entry
    cmp byte [kparamsFound], 1  ; did we find kparams?
    je .next_root_entry         ; skip to next entry if already found
    call .capture_kparams_entry ; otherwise, capture kparams initial cluster

    ; old test: print filename
    ;add si, bx          ; add the current offset to get to the filename
    ;call .print         ; print the filename (will print garbage currently since we haven't implemented a proper print routine, but we should see progress if it's working at all)
    ;mov si, newline
    ;call .print         ; print a newline after the filename
.next_root_entry:
    add bx, 0x20        ; advance to next directory entry (32 bytes)
    cmp bx, ROOT_DIR_SECTORS*512 ; have we parsed all entries in the root directory buffer?
    jb .parse_root_loop ; if not, loop back to parse the next entry
.root_scan_done:
    pop bx                      ; restore offset
    cmp byte [kernelFound], 1   ; did we find the kernel?
    je .kernel_found            ; if so, proceed with kernel load prereqs
    jmp .root_not_found         ; if not, that's a fatal error

.unexpected_root_end:
    pop bx              ; restore bx if we need it later (probably not at this point, but just in case)
.root_not_found:
    mov si, rootEndError        ; show root end error message
    call .print                 ;
    mov si, haltMsg             ; show halt message
    call .print                 ;
    jmp .wait_key_reset         ; "press any key to reset"

.capture_kernel_entry:
    mov ax, [es:bx+FAT_OFFSET_START_CLUSTER]    ; record the starting cluster of the kernel image
    mov [kernelStartCluster], ax                ;
    mov word [kernelStartCluster+2], 0          ;

    mov ax, [es:bx+FAT_OFFSET_SIZE_BYTES]       ; record byte-size of the kernel image
    mov [kernelSizeBytes], ax                   ;
    mov ax, [es:bx+FAT_OFFSET_SIZE_BYTES+2]     ;
    mov [kernelSizeBytes+2], ax

    mov ax, [kernelSizeBytes]                   ; initialize remaining byte count
    mov [kernelRemainingBytes], ax              ;
    mov ax, [kernelSizeBytes+2]                 ;
    mov [kernelRemainingBytes+2], ax            ;

    mov ax, [kernelStartCluster]                ; init current cluster counter with starting cluster value
    mov [kernelCurCluster], ax                  ;
    mov word [kernelCurCluster+2], 0            ;

    mov byte [kernelFound], 1                   ; set kernel found flag and return
    ret                                         

.capture_kparams_entry:
    mov ax, [es:bx+FAT_OFFSET_START_CLUSTER]    ; record the starting cluster of the kparams file
    mov [kparamsStartCluster], ax               ;
    mov word [kparamsStartCluster+2], 0         ; 

    mov ax, [es:bx+FAT_OFFSET_SIZE_BYTES]       ; record starting byte-size of the kparams file
    mov [kparamsFileSize], ax                   ;
    mov ax, [es:bx+FAT_OFFSET_SIZE_BYTES+2]     ; 
    mov [kparamsFileSize+2], ax                 ;

    mov byte [kparamsFound], 1                  ; set kparams found flag and return
    ret

.kernel_found:
    mov si, foundMsg    ; indicate we've found the kernel image
    call .print         ; 

    jmp .initA20        ; this is a 32-bit kernel, so we need A20 gate set up



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

.match_kparams:
    push si             ; simple string comparison function again, but for
    push di             ; kparams filename (kparamsfn) this time
    push cx             
    mov si, kparamsfn
    mov di, bx
    mov cx, 11          ; fat12 filenames capped at 11 chars
    repe cmpsb          ; compare DS:SI to ES:DI as strings
                        ; note: ZF=1 if matching, e.g. use jz if match
    pop cx
    pop di
    pop si
    ret

.enterPMHandoff:        ; if we found kparams, there's a bit of extra work to do first
    cli                 ; interrupts should already be off, but we want to be sure
    cld                 ; direction flag should be cleared as well 

    xor edx, edx                ; At handoff, DL will contain the boot drive number
    mov dl, byte [flpdrv]       ; stage1 told us our boot drive number, and we need to do this
                                ; before passing new segment selectors

    xor esi, esi                ; init esi to 0 for handoff
    xor ecx, ecx                ; ecx will contain byte length of kparams
    mov cx, [kparamsSizeBytes]  ;
    or cx, cx                   ; if kparams size is zero, we can skip populating esi
    jz .no_kparams_handoff      ;
    mov esi, KPARAMS_BUFFER_LINEAR  ; otherwise, esi=kparams linear address, ecx=kparams length
                                ; fallthrough to the rest of handoff
.no_kparams_handoff:
    xor eax, eax            ; clear eax for future use
    mov ax, cs              ; get current code segment, which is where our GDT is located
                            ; since we're in real mode with unreal mode limits in FS/GS
    shl eax, 4              ; convert from segment to linear (physical) address (*16)
    add eax, pm32_gdt       ; add offset of our protected mode GDT to get the linear address of the GDT
    mov [pm32_gdtr+2], eax  ; patch the linear address of the PM GDT into the PM GDTR structure for 32-bit entry
    lgdt [pm32_gdtr]        ; load our new flat GDT

    xor eax, eax            ; clear eax again
    mov ax, cs              ; get code segment again, same reason as before
    shl eax, 4              ; segment to linear/physical address conversion
    add eax, .pm32Entry     ; add offset of our protected mode entry point to get its linear address
    mov [cs:pm32_far_ptr+0], eax                        ; 32-bit offset
    mov word [cs:pm32_far_ptr+4], PM32_CODE_SELECTOR    ; set segment selector for protected mode entry

    mov eax, cr0            ; enter protected mode
    or eax, 1
    mov cr0, eax

    jmp dword far [cs:pm32_far_ptr] ; jump to entrypoint

bits 32
.pm32Entry:
    mov ax, PM32_DATA_SELECTOR  ; all segments should be the same in a flat memory model, so we use DS
    mov ds, ax                  ; set all the segments to be equal
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov esp, PM32_STACK_TOP     ; initialize the stack pointer
    cld                         ; once more, ensure direction flag is cleared
    cli                         ; ditto for interrupts


    mov eax, KERNEL_LOAD_LINEAR_ADDR    ; EAX will contain the linear address of the loaded kernel
    jmp eax                     ; jump to the kernel, ending stage2

bits 16

%include "io.inc.asm"
%include "a20.inc.asm"
%include "lba2chs.inc.asm"

initMsg                     db 'Stage2 finding kernel', 0
foundMsg                    db 'done', 13, 10, 0
kernelLoadStartMsg          db 'Loading', 0
rootEndError                db 'unexpected end of root', 0
kernelPrematureEOFError     db 'EOF reached!',13,10,'Expected more data', 0
kernelCrcMismatchError      db 'crc32 fail',13,10,'Invalid kernel',0
kernelCrcSkipMsg            db 'skipping crc32',0
kernelCrcRuntime            dd 0
a20Error                    db 13,10,'A 386+ CPU with A20 line support is required', 0
checkmark                   db 0xFB, 0
dot                         db '.', 0
kernelLoadCursor            dd 0
kernelStartCluster          dd 0
kernelCurCluster            dd 0
kernelRemainingBytes        dd 0
kernelfn                    db 'KERNEL  BIN' ; 11-byte filename for kernel
flpdrv                      db 0
maxRetry                    db 0x03
newline                     db 13, 10, 0
errorMsg                    db 13, 10, 'error: ',0
fallbackMsg                 db 'fallback', 13, 10, 0
haltMsg                     db ', press any key to reset',0
fdReadError                 db 'read error',0
fdResetError                db 'reset error',0
flpdx                       dw 0x0000
flpax                       dw 0x0000
flpretry                    db 0
lbaPos                      dw 0
kernelFound                 db 0
kparamsFound                db 0
kparamsStartCluster         dd 0
kparamsCurCluster           dd 0
kparamsFileSize             dd 0
kparamsLoadedLen            dw 0
kparamsfn                   db 'KPARAMS DAT'
kparamsRemainingBytes       dd 0

align 8

; flat protected mode gdt (to be replaced by kernel)
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

; unreal mode gdt
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

; CRC32 table for polynomial 0xEDB88320, used for runtime kernel 
; integrity verification.
align 4
crc32Table:
    dd 0x00000000,0x1DB71064,0x3B6E20C8,0x26D930AC
    dd 0x76DC4190,0x6B6B51F4,0x4DB26158,0x5005713C
    dd 0xEDB88320,0xF00F9344,0xD6D6A3E8,0xCB61B38C
    dd 0x9B64C2B0,0x86D3D2D4,0xA00AE278,0xBDBDF21C

times (BYTES_PER_SECTOR*STAGE2_RESERVED_SECTORS)-4-($-$$) db 0
kernelCrc32 dd KERNEL_CHECKSUM  ; replaced by build system
