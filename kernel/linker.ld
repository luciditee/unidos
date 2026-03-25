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
ASSERT(KERNEL_VIRT_OFFSET == (KERNEL_VIRT_BASE - KERNEL_PHYS_BASE), "KERNEL_VIRT_OFFSET mismatch");

SECTIONS
{
  /*
   * 1) Low trampoline, identity-addressed, entered directly from stage2.
   *    Keep only startup code in .text.start.
   */
  . = KERNEL_PHYS_BASE;
  .text.start ALIGN(16) : AT(KERNEL_PHYS_BASE)
  {
    __trampoline_start = .;
    KEEP(*(.text.start))
    KEEP(*(.text.start.*))
  }

  .rodata.start ALIGN(16) : AT(ADDR(.rodata.start))
  {
    KEEP(*(.rodata.start))
    KEEP(*(.rodata.start.*))
  }

  .data.start ALIGN(16) : AT(ADDR(.data.start))
  {
    KEEP(*(.data.start))
    KEEP(*(.data.start.*))
  }

  .bss.start ALIGN(16) (NOLOAD) : AT(ADDR(.bss.start))
  {
    KEEP(*(.bss.start))
    KEEP(*(.bss.start.*))
  }

  __trampoline_end = ALIGN(., 16);

  /*
   * 2) Kernel proper, linked in higher-half virtual space, loaded physically
   *    just after trampoline.
   */
  __kernel_start_lma = ALIGN(__trampoline_end, 16);

  . = KERNEL_VIRT_BASE;
  __kernel_start_vma = .;

  .text ALIGN(16) : AT(__kernel_start_lma + (ADDR(.text) - __kernel_start_vma))
  {
    *(.text .text.*)
  }

  .rodata ALIGN(16) : AT(__kernel_start_lma + (ADDR(.rodata) - __kernel_start_vma))
  {
    *(.rodata .rodata.*)
  }

  .data ALIGN(16) : AT(__kernel_start_lma + (ADDR(.data) - __kernel_start_vma))
  {
    *(.data .data.*)
  }

  .bss ALIGN(16) (NOLOAD) : AT(__kernel_start_lma + (ADDR(.bss) - __kernel_start_vma))
  {
    __bss_start_vma = .;
    *(COMMON)
    *(.bss .bss.*)
    __bss_end_vma = .;
  }

  __kernel_end_vma = .;

  /* Physical aliases of high-half symbols (derived from actual high segment LMA). */
  __bss_start_lma = __kernel_start_lma + (__bss_start_vma - __kernel_start_vma);
  __bss_end_lma = __kernel_start_lma + (__bss_end_vma - __kernel_start_vma);
  __kernel_end_lma = __kernel_start_lma + (__kernel_end_vma - __kernel_start_vma);

  /* Backward-compatible names currently expected by C/ASM bootstrap paths */
  __kernel_start = __kernel_start_lma;
  __kernel_end = __kernel_end_lma;
  __bss_start = __bss_start_lma;

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}
