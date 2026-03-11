cmake_minimum_required(VERSION 3.20)
project(unidos_bootstrap LANGUAGES NONE)

set(UNIDOS_OUT_DIR ${CMAKE_BINARY_DIR}/out)
file(MAKE_DIRECTORY ${UNIDOS_OUT_DIR})

find_program(NASM_EXECUTABLE nasm REQUIRED)
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
set(KERNEL_SRC ${CMAKE_SOURCE_DIR}/kernel/kernel_stub.asm)

set(STAGE1_BIN ${UNIDOS_OUT_DIR}/stage1.bin)
set(STAGE2_BIN ${UNIDOS_OUT_DIR}/stage2.bin)
set(KERNEL_BIN ${UNIDOS_OUT_DIR}/kernel.bin)
set(FLOPPY_IMG ${UNIDOS_OUT_DIR}/floppy.img)
set(NASM_BOOT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/boot/)
set(NASM_STAGE2_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/boot/stage2/)

# Boot layout defaults:
# - LBA 0: stage1 boot sector
# - LBA 1..N: raw stage2 loader stored in reserved FAT sectors
set(STAGE2_RESERVED_SECTORS 4 CACHE STRING "Number of reserved sectors used to store raw stage2 after the boot sector")

add_custom_command(
  OUTPUT ${STAGE1_BIN}
  COMMAND ${NASM_EXECUTABLE} -i${NASM_BOOT_INCLUDE_DIR} -DSTAGE2_RESERVED_SECTORS=${STAGE2_RESERVED_SECTORS} -f bin -o ${STAGE1_BIN} ${STAGE1_SRC}
  DEPENDS ${STAGE1_SRC}
  COMMENT "Assembling stage1 boot sector"
  VERBATIM
)

add_custom_command(
  OUTPUT ${STAGE2_BIN}
  COMMAND ${NASM_EXECUTABLE} -i${NASM_BOOT_INCLUDE_DIR} -i${NASM_STAGE2_INCLUDE_DIR} -DSTAGE2_RESERVED_SECTORS=${STAGE2_RESERVED_SECTORS} -f bin -o ${STAGE2_BIN} ${STAGE2_SRC}
  DEPENDS ${STAGE2_SRC}
  COMMENT "Assembling stage2 bootstrap"
  VERBATIM
)

add_custom_command(
  OUTPUT ${KERNEL_BIN}
  COMMAND ${NASM_EXECUTABLE} -f bin -o ${KERNEL_BIN} ${KERNEL_SRC}
  DEPENDS ${KERNEL_SRC}
  COMMENT "Assembling kernel placeholder"
  VERBATIM
)

add_custom_target(artifacts DEPENDS ${STAGE1_BIN} ${STAGE2_BIN} ${KERNEL_BIN})

add_custom_target(image ALL
  COMMAND ${CMAKE_COMMAND} -E rm -f ${FLOPPY_IMG}
  COMMAND ${CMAKE_COMMAND} -E env
          STAGE1_BIN=${STAGE1_BIN}
          STAGE2_BIN=${STAGE2_BIN}
          KERNEL_BIN=${KERNEL_BIN}
          IMG_OUT=${FLOPPY_IMG}
      STAGE2_RESERVED_SECTORS=${STAGE2_RESERVED_SECTORS}
          /bin/sh ${CMAKE_SOURCE_DIR}/tools/make_floppy_image.sh
  DEPENDS artifacts ${CMAKE_SOURCE_DIR}/tools/make_floppy_image.sh
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
