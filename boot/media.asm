
%define FAT_COUNT 2
%define BYTES_PER_SECTOR 512
%define FAT_SECTORS_PER_CLUSTER 1
%define FAT_OFFSET_START_CLUSTER 26
%define FAT_OFFSET_SIZE_BYTES 28
%define MAGIC_4CHAR 'BTP1'

%ifndef STAGE2_RESERVED_SECTORS
%define STAGE2_RESERVED_SECTORS 7
%endif

%ifndef STAGE2_HEADER_OFFSET
%define STAGE2_HEADER_OFFSET 0x0020
%endif

%ifdef FLOPPY_720K
; Smallest supported size is DS/DD 3.5" 720KiB floppy
%define ROOT_ENTRIES        112
%define TOTAL_SECTORS16     1440
%define MEDIA_DESCRIPTOR    0xF9
%define SECTORS_PER_FAT     3
%define SECTORS_PER_TRACK   9
%define FLOPPY_HEADS        2
%define ROOT_DIR_SECTORS    7
%define ROOT_START          7
%define DATA_START          0x0E

%elifdef FLOPPY_1200K
; 1.2MB 5.25" floppy BPB
%define ROOT_ENTRIES        224
%define TOTAL_SECTORS16     2400
%define MEDIA_DESCRIPTOR    0xF9
%define SECTORS_PER_FAT     7
%define SECTORS_PER_TRACK   15
%define FLOPPY_HEADS        2
%define ROOT_DIR_SECTORS    14
%define ROOT_START          15
%define DATA_START          0x1D

%else
; Default to a 1440KiB floppy BPB if no size specified
%define ROOT_ENTRIES        224
%define TOTAL_SECTORS16     2880
%define MEDIA_DESCRIPTOR    0xF0
%define SECTORS_PER_FAT     9
%define SECTORS_PER_TRACK   18
%define FLOPPY_HEADS        2
%define ROOT_DIR_SECTORS    14
%define ROOT_START          19
%define DATA_START          0x21

%endif