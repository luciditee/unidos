
%ifndef PM32_STACK_TOP
%define PM32_STACK_TOP     0x0009FC00
kernel_stack_top equ PM32_STACK_TOP
%endif

%ifndef GDT_CONSTANTS
%define GDT_SEL_KCODE 0x08
%define GDT_SEL_KDATA 0x10
%define GDT_SEL_UCODE 0x18
%define GDT_SEL_UDATA 0x20
%define GDT_SEL_TSS   0x28

%define GDT_CONSTANTS
%endif
