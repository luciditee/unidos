OUTPUT_FORMAT(elf32-i386)
ENTRY(start)

/*
 * Kernel address constants can be overridden from the build via ld --defsym.
 * Defaults here keep current behavior while exposing stable symbols for
 * paging/higher-half transition work.
 */
PROVIDE(KERNEL_PHYS_BASE = 0x00100000);
PROVIDE(KERNEL_VIRT_BASE = 0xC0000000);
PROVIDE(KERNEL_VIRT_OFFSET = KERNEL_VIRT_BASE - KERNEL_PHYS_BASE);

SECTIONS
{
  . = KERNEL_PHYS_BASE;

  .text : ALIGN(16)
  {
    __kernel_start = .;
    KEEP(*(.text.start))
    *(.text .text.*)
  }

  .rodata : ALIGN(16)
  {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(16)
  {
    *(.data .data.*)
  }

  .bss : ALIGN(16)
  {
    __bss_start = .;
    *(COMMON)
    *(.bss .bss.*)
    __kernel_end = .;
  }

  /* TODO: Putting this here so it's written somewhere -- stage2 doesn't zero 
     the region of memory where .bss ought to go. There's a possibility of
     garbage data in cases where we don't explicitly initialize to 0 in the
     kernel image. This can likely be ignored for now but could cause headaches
     later */

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}
