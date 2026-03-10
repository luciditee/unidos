bits 16
org 0x0000

start:
    cli
.halt:
    hlt
    jmp .halt
