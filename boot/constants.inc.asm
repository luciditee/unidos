
; Memory map constants
%ifndef STAGE2_LOAD_SEGMENT
%define STAGE2_LOAD_SEGMENT 0x1000
%endif

; Protected mode constants
%define PM32_CODE_SELECTOR 0x08
%define PM32_DATA_SELECTOR 0x10
%define PM32_STACK_TOP     0x0009FC00
