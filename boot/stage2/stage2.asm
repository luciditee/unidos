bits 16
org 0x0000

%include "media.asm"

%ifndef STAGE2_RESERVED_SECTORS
%define STAGE2_RESERVED_SECTORS 4
%endif

%define TOTAL_RESERVED_SECTORS (1 + STAGE2_RESERVED_SECTORS)

; Skip the header region
entry:
    jmp start
    nop

times STAGE2_HEADER_OFFSET-($-$$) db 0

; Header region
header:
    magic db MAGIC_4CHAR
    versionMajor db 1
    versionMinor db 0

    ; bootinfo constants
    totalSize           dw 0 ; will be filled in by stage1
    stage2Flags         dw 0 ; reserved for now
    checksum32          dd 0
    bootDrive           db 0 ; contents of DL register before stage1 handoff
    mediaType           db 0 ; for now, always 0x01 meaning FAT12 floppy

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
    dataStartLba        dd TOTAL_RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT + ROOT_ENTRIES * 32 / BYTES_PER_SECTOR

    ; stage2 load/handoff info
    stage2LoadPhys      dd 0x00010000 ; physical address where stage2 will be loaded (segment 0x1000, offset 0)  
    stage2EntryCS       dw 0x1000
    stage2EntryIP       dw 0x0000
    stage2ErrorCode     dw 0          ; reserved for now, can be used to pass error codes to stage2 if needed
    reserved0           dw 0

%include "lba2chs.asm"

start:
    cli
    mov ax, cs
    mov ds, ax

    mov si, msg
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x0A
    int 0x10
    jmp .print

.halt:
    hlt
    jmp .halt

msg db 'S2 scaffold reached', 0

times (BYTES_PER_SECTOR*STAGE2_RESERVED_SECTORS)-($-$$) db 0
