cmake_minimum_required(VERSION 3.20)
project(unidos_bootstrap LANGUAGES C)

set(UNIDOS_DISPLAYNAME "UNIDOS")
set(UNIDOS_VERSION_MAJOR 0)
set(UNIDOS_VERSION_MINOR 1)
set(UNIDOS_VERSION_PATCH 0)
set(UNIDOS_VERSION "\"${UNIDOS_DISPLAYNAME} ${UNIDOS_VERSION_MAJOR}.${UNIDOS_VERSION_MINOR}.${UNIDOS_VERSION_PATCH}\"")

set(UNIDOS_OUT_DIR ${CMAKE_BINARY_DIR}/out)
file(MAKE_DIRECTORY ${UNIDOS_OUT_DIR})

# Kernel address-space constants (kept explicit for paging/higher-half work).
set(KERNEL_PHYS_BASE 0x00100000 CACHE STRING "Physical load base address for the kernel image")
set(KERNEL_VIRT_BASE 0xC0000000 CACHE STRING "Virtual base address reserved for kernel mapping")
set(KERNEL_VIRT_OFFSET 0xBFF00000 CACHE STRING "Kernel virtual-physical offset (KERNEL_VIRT_BASE - KERNEL_PHYS_BASE)")

find_program(NASM_EXECUTABLE nasm REQUIRED)
find_program(PYTHON3_EXECUTABLE python3 REQUIRED)
find_program(LD_EXECUTABLE ld REQUIRED)
find_program(OBJCOPY_EXECUTABLE objcopy REQUIRED)
find_program(MKFS_FAT_EXECUTABLE mkfs.fat)
find_program(MCOPY_EXECUTABLE mcopy)
find_program(QEMU_I386_EXECUTABLE qemu-system-i386)
find_program(BOCHS_EXECUTABLE bochs)

if(NOT MKFS_FAT_EXECUTABLE)
  message(WARNING "mkfs.fat not found; image generation will fail until dosfstools is installed.")
endif()
if(NOT MCOPY_EXECUTABLE)
  message(WARNING "mcopy not found; image generation will fail until mtools is installed.")
endif()

set(STAGE1_SRC ${CMAKE_SOURCE_DIR}/boot/stage1/stage1.asm)
set(STAGE2_SRC ${CMAKE_SOURCE_DIR}/boot/stage2/stage2.asm)

# Explicit kernel source lists. Add new files here.
set(KERNEL_ASM_SOURCES
  ${CMAKE_SOURCE_DIR}/kernel/asmcore/kernel_entry.asm
  ${CMAKE_SOURCE_DIR}/kernel/asmcore/c_abi.asm
  ${CMAKE_SOURCE_DIR}/kernel/asmcore/usermode_test.asm
  ${CMAKE_SOURCE_DIR}/kernel/asmcore/usermode_entry.asm
)

set(KERNEL_C_SOURCES
  ${CMAKE_SOURCE_DIR}/kernel/kmain.c
  ${CMAKE_SOURCE_DIR}/kernel/isr.c  
  ${CMAKE_SOURCE_DIR}/kernel/pit.c
  ${CMAKE_SOURCE_DIR}/kernel/pic.c
  ${CMAKE_SOURCE_DIR}/kernel/kb.c
  ${CMAKE_SOURCE_DIR}/kernel/sched.c
  ${CMAKE_SOURCE_DIR}/kernel/bootinfo.c
  ${CMAKE_SOURCE_DIR}/kernel/kmem.c
  ${CMAKE_SOURCE_DIR}/kernel/paging.c
  ${CMAKE_SOURCE_DIR}/kernel/kremap.c
  ${CMAKE_SOURCE_DIR}/kernel/panic.c
  ${CMAKE_SOURCE_DIR}/kernel/syscall.c
  ${CMAKE_SOURCE_DIR}/kernel/errno.c
  ${CMAKE_SOURCE_DIR}/kernel/exception.c
  ${CMAKE_SOURCE_DIR}/kernel/uaccess.c
  ${CMAKE_SOURCE_DIR}/kernel/sys/exit.c
  ${CMAKE_SOURCE_DIR}/kernel/sys/write.c
  ${CMAKE_SOURCE_DIR}/kernel/sys/yield.c
)

set(KERNEL_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/kernel/include
)

set(KERNEL_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/linker.ld)
set(KPARAMS_DEFAULT_SRC ${CMAKE_SOURCE_DIR}/kernel/kparams.default.dat)
set(USERMODE_TEST_SRC ${CMAKE_SOURCE_DIR}/user/syscalltest.c)
set(USERMODE_TEST_LINK_ADDR 0x40000000 CACHE STRING "Virtual load address used when linking the embedded usermode syscall test program")

set(STAGE1_BIN ${UNIDOS_OUT_DIR}/stage1.bin)
set(STAGE2_BIN ${UNIDOS_OUT_DIR}/stage2.bin)
set(KERNEL_ELF ${UNIDOS_OUT_DIR}/kernel.elf)
set(KERNEL_BIN ${UNIDOS_OUT_DIR}/kernel.bin)
set(KERNEL_OBJ_DIR ${UNIDOS_OUT_DIR}/kernel_obj)
set(USERMODE_TEST_OBJ ${UNIDOS_OUT_DIR}/usermode_test.o)
set(USERMODE_TEST_ELF ${UNIDOS_OUT_DIR}/usermode_test.elf)
set(USERMODE_TEST_BIN ${UNIDOS_OUT_DIR}/usermode_test.bin)
set(FLOPPY_IMG ${UNIDOS_OUT_DIR}/floppy.img)
set(NASM_BOOT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/boot/)
set(NASM_KERNEL_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/kernel/asmcore/
  ${CMAKE_SOURCE_DIR}/kernel/prekinit/
)
set(NASM_STAGE2_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/boot/stage2/)

# Boot layout defaults:
# - LBA 0: stage1 boot sector
# - LBA 1..N: raw stage2 loader stored in reserved FAT sectors
set(STAGE2_RESERVED_SECTORS 6 CACHE STRING "Number of reserved sectors used to store raw stage2 after the boot sector")
option(NOKPARAMS "Skip injecting kernel/kparams.default.dat as KPARAMS.DAT into the floppy image" OFF)

set(INJECT_DEFAULT_KPARAMS 1)
if(NOKPARAMS)
  set(INJECT_DEFAULT_KPARAMS 0)
endif()

if(INJECT_DEFAULT_KPARAMS AND NOT EXISTS ${KPARAMS_DEFAULT_SRC})
  message(FATAL_ERROR "Default kparams file not found: ${KPARAMS_DEFAULT_SRC}. Create it or configure with -DNOKPARAMS=ON.")
endif()

add_custom_command(
  OUTPUT ${STAGE1_BIN}
  COMMAND ${NASM_EXECUTABLE} -i${NASM_BOOT_INCLUDE_DIR} -DSTAGE2_RESERVED_SECTORS=${STAGE2_RESERVED_SECTORS} -f bin -o ${STAGE1_BIN} ${STAGE1_SRC}
  DEPENDS ${STAGE1_SRC}
  COMMENT "Assembling stage1 boot sector"
  VERBATIM
)

add_custom_command(
  OUTPUT ${STAGE2_BIN}
  COMMAND ${CMAKE_COMMAND} -E env
          NASM_EXECUTABLE=${NASM_EXECUTABLE}
          PYTHON3_EXECUTABLE=${PYTHON3_EXECUTABLE}
          KERNEL_BIN=${KERNEL_BIN}
          STAGE2_SRC=${STAGE2_SRC}
          STAGE2_BIN=${STAGE2_BIN}
          NASM_BOOT_INCLUDE_DIR=${NASM_BOOT_INCLUDE_DIR}
          NASM_STAGE2_INCLUDE_DIR=${NASM_STAGE2_INCLUDE_DIR}
          STAGE2_RESERVED_SECTORS=${STAGE2_RESERVED_SECTORS}
          /bin/sh -c "set -eu; CRC32=$($PYTHON3_EXECUTABLE -c 'import sys,zlib;data=open(sys.argv[1],\"rb\").read();print(f\"0x{zlib.crc32(data)&0xffffffff:08X}\")' \"$KERNEL_BIN\"); echo \"Kernel CRC32: $CRC32\"; exec \"$NASM_EXECUTABLE\" -i\"$NASM_BOOT_INCLUDE_DIR\" -i\"$NASM_STAGE2_INCLUDE_DIR\" -DSTAGE2_RESERVED_SECTORS=$STAGE2_RESERVED_SECTORS -DKERNEL_CHECKSUM=$CRC32 -f bin -o \"$STAGE2_BIN\" \"$STAGE2_SRC\""
  DEPENDS ${STAGE2_SRC} ${KERNEL_BIN}
  COMMENT "Assembling stage2 bootstrap"
  VERBATIM
)

set(KERNEL_OBJECTS)

set(NASM_KERNEL_INCLUDE_FLAGS)
foreach(NASM_KERNEL_INCLUDE_DIR IN LISTS NASM_KERNEL_INCLUDE_DIRS)
  list(APPEND NASM_KERNEL_INCLUDE_FLAGS -i${NASM_KERNEL_INCLUDE_DIR})
endforeach()
list(APPEND NASM_KERNEL_INCLUDE_FLAGS -i${UNIDOS_OUT_DIR}/)

add_custom_command(
  OUTPUT ${USERMODE_TEST_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${UNIDOS_OUT_DIR}
  COMMAND ${CMAKE_C_COMPILER}
          -m32
          -ffreestanding
          -fno-stack-protector
          -fno-pie
          -march=i386
          -nostdlib
          -nostartfiles
          -Wall
          -Wextra
          -MMD
          -MF ${USERMODE_TEST_OBJ}.d
          -c ${USERMODE_TEST_SRC}
          -o ${USERMODE_TEST_OBJ}
  DEPFILE ${USERMODE_TEST_OBJ}.d
  DEPENDS ${USERMODE_TEST_SRC}
  COMMENT "Compiling syscalltest.c"
  VERBATIM
)

add_custom_command(
  OUTPUT ${USERMODE_TEST_ELF}
  COMMAND ${LD_EXECUTABLE}
          -m elf_i386
          -nostdlib
          -N
          -Ttext ${USERMODE_TEST_LINK_ADDR}
          -e _start
          -o ${USERMODE_TEST_ELF}
          ${USERMODE_TEST_OBJ}
  DEPENDS ${USERMODE_TEST_OBJ}
  COMMENT "Linking usermode syscall test ELF"
  VERBATIM
)

add_custom_command(
  OUTPUT ${USERMODE_TEST_BIN}
  COMMAND ${OBJCOPY_EXECUTABLE} -O binary ${USERMODE_TEST_ELF} ${USERMODE_TEST_BIN}
  DEPENDS ${USERMODE_TEST_ELF}
  COMMENT "Generating usermode syscall test binary"
  VERBATIM
)

foreach(KERNEL_ASM_SOURCE IN LISTS KERNEL_ASM_SOURCES)
  get_filename_component(KERNEL_ASM_NAME_WE ${KERNEL_ASM_SOURCE} NAME_WE)
  set(KERNEL_ASM_OBJ ${KERNEL_OBJ_DIR}/${KERNEL_ASM_NAME_WE}.o)
  set(KERNEL_ASM_DEPFILE ${KERNEL_OBJ_DIR}/${KERNEL_ASM_NAME_WE}.d)
  set(KERNEL_ASM_EXTRA_DEPS)
  if(KERNEL_ASM_NAME_WE STREQUAL "usermode_test")
    set(KERNEL_ASM_EXTRA_DEPS ${USERMODE_TEST_BIN})
  endif()

  add_custom_command(
    OUTPUT ${KERNEL_ASM_OBJ}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KERNEL_OBJ_DIR}
    COMMAND ${NASM_EXECUTABLE}
            -DUNIDOS_VERSION=${UNIDOS_VERSION}
            -DKERNEL_PHYS_BASE=${KERNEL_PHYS_BASE}
            -DKERNEL_VIRT_BASE=${KERNEL_VIRT_BASE}
            -DKERNEL_VIRT_OFFSET=${KERNEL_VIRT_OFFSET}
            ${NASM_KERNEL_INCLUDE_FLAGS}
            -f elf32
            -MD ${KERNEL_ASM_DEPFILE}
            -o ${KERNEL_ASM_OBJ}
            ${KERNEL_ASM_SOURCE}
    DEPFILE ${KERNEL_ASM_DEPFILE}
    DEPENDS ${KERNEL_ASM_SOURCE} ${KERNEL_ASM_EXTRA_DEPS}
    COMMENT "Assembling ${KERNEL_ASM_NAME_WE}.asm"
    VERBATIM
  )

  list(APPEND KERNEL_OBJECTS ${KERNEL_ASM_OBJ})
endforeach()

set(KERNEL_C_INCLUDE_FLAGS)
foreach(KERNEL_INCLUDE_DIR IN LISTS KERNEL_INCLUDE_DIRS)
  list(APPEND KERNEL_C_INCLUDE_FLAGS -I${KERNEL_INCLUDE_DIR})
endforeach()

foreach(KERNEL_C_SOURCE IN LISTS KERNEL_C_SOURCES)
  get_filename_component(KERNEL_C_NAME_WE ${KERNEL_C_SOURCE} NAME_WE)
  set(KERNEL_C_OBJ ${KERNEL_OBJ_DIR}/${KERNEL_C_NAME_WE}.o)
  set(KERNEL_C_DEPFILE ${KERNEL_OBJ_DIR}/${KERNEL_C_NAME_WE}.d)

  add_custom_command(
    OUTPUT ${KERNEL_C_OBJ}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KERNEL_OBJ_DIR}
    COMMAND ${CMAKE_C_COMPILER}
            -m32
            -ffreestanding
            -fno-stack-protector
            -fno-pie
            -march=i386
            -nostdlib
            -nostartfiles
            -Wall
            -Wextra
            ${KERNEL_C_INCLUDE_FLAGS}
            -DUNIDOS_VERSION=${UNIDOS_VERSION}
            -DSTAGE2_RESERVED_SECTORS=${STAGE2_RESERVED_SECTORS}
            -DKERNEL_PHYS_BASE=${KERNEL_PHYS_BASE}
            -DKERNEL_VIRT_BASE=${KERNEL_VIRT_BASE}
            -DKERNEL_VIRT_OFFSET=${KERNEL_VIRT_OFFSET}
            -MMD
            -MF ${KERNEL_C_DEPFILE}
            -c ${KERNEL_C_SOURCE}
            -o ${KERNEL_C_OBJ}
    DEPFILE ${KERNEL_C_DEPFILE}
    DEPENDS ${KERNEL_C_SOURCE}
    COMMENT "Compiling ${KERNEL_C_NAME_WE}.c"
    VERBATIM
  )

  list(APPEND KERNEL_OBJECTS ${KERNEL_C_OBJ})
endforeach()

add_custom_command(
  OUTPUT ${KERNEL_ELF}
  COMMAND ${LD_EXECUTABLE}
          -m elf_i386
          -nostdlib
          --defsym=KERNEL_PHYS_BASE=${KERNEL_PHYS_BASE}
          --defsym=KERNEL_VIRT_BASE=${KERNEL_VIRT_BASE}
          --defsym=KERNEL_VIRT_OFFSET=${KERNEL_VIRT_OFFSET}
          -T ${KERNEL_LINKER_SCRIPT}
          -o ${KERNEL_ELF}
          ${KERNEL_OBJECTS}
  DEPENDS ${KERNEL_OBJECTS} ${KERNEL_LINKER_SCRIPT}
  COMMENT "Linking kernel ELF"
  VERBATIM
)

add_custom_command(
  OUTPUT ${KERNEL_BIN}
  COMMAND ${OBJCOPY_EXECUTABLE} -O binary ${KERNEL_ELF} ${KERNEL_BIN}
  DEPENDS ${KERNEL_ELF}
  COMMENT "Generating flat kernel binary"
  VERBATIM
)

add_custom_target(artifacts DEPENDS ${STAGE1_BIN} ${STAGE2_BIN} ${KERNEL_BIN})

set(IMAGE_EXTRA_DEPS)
if(INJECT_DEFAULT_KPARAMS)
  list(APPEND IMAGE_EXTRA_DEPS ${KPARAMS_DEFAULT_SRC})
endif()

add_custom_target(image ALL
  COMMAND ${CMAKE_COMMAND} -E rm -f ${FLOPPY_IMG}
  COMMAND ${CMAKE_COMMAND} -E env
          STAGE1_BIN=${STAGE1_BIN}
          STAGE2_BIN=${STAGE2_BIN}
          KERNEL_BIN=${KERNEL_BIN}
          KPARAMS_DEFAULT_SRC=${KPARAMS_DEFAULT_SRC}
          NOKPARAMS=${NOKPARAMS}
          IMG_OUT=${FLOPPY_IMG}
      STAGE2_RESERVED_SECTORS=${STAGE2_RESERVED_SECTORS}
          /bin/sh ${CMAKE_SOURCE_DIR}/tools/make_floppy_image.sh
  DEPENDS artifacts ${CMAKE_SOURCE_DIR}/tools/make_floppy_image.sh ${IMAGE_EXTRA_DEPS}
  BYPRODUCTS ${FLOPPY_IMG}
  COMMENT "Rebuilding FAT12 floppy image"
  VERBATIM
)

if(QEMU_I386_EXECUTABLE)
  add_custom_target(run-qemu
    COMMAND ${QEMU_I386_EXECUTABLE} -fda ${FLOPPY_IMG} -boot a -m 16M -serial stdio -monitor none -no-reboot -no-shutdown
    COMMENT "Run floppy image in QEMU"
    VERBATIM
  )

  add_custom_target(debug-qemu
    COMMAND ${QEMU_I386_EXECUTABLE} -fda ${FLOPPY_IMG} -boot a -m 16M -serial stdio -monitor none -S -s
    COMMENT "Run QEMU with GDB stub (:1234)"
    VERBATIM
  )
endif()

if(BOCHS_EXECUTABLE)
  add_custom_target(run-bochs
    COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/tools/run_bochs.sh ${FLOPPY_IMG}
    DEPENDS image ${CMAKE_SOURCE_DIR}/tools/run_bochs.sh
    COMMENT "Run floppy image in Bochs"
    VERBATIM
  )
endif()
