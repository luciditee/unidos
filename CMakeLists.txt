cmake_minimum_required(VERSION 3.20)
project(unidos_bootstrap LANGUAGES NONE)

set(UNIDOS_OUT_DIR ${CMAKE_BINARY_DIR}/out)
file(MAKE_DIRECTORY ${UNIDOS_OUT_DIR})

find_program(NASM_EXECUTABLE nasm REQUIRED)
find_program(PYTHON3_EXECUTABLE python3 REQUIRED)
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
set(KERNEL_SRC ${CMAKE_SOURCE_DIR}/kernel/kernel_entry.asm)
set(KPARAMS_DEFAULT_SRC ${CMAKE_SOURCE_DIR}/kernel/kparams.default.dat)

set(STAGE1_BIN ${UNIDOS_OUT_DIR}/stage1.bin)
set(STAGE2_BIN ${UNIDOS_OUT_DIR}/stage2.bin)
set(KERNEL_BIN ${UNIDOS_OUT_DIR}/kernel.bin)
set(KERNEL_DEPFILE ${UNIDOS_OUT_DIR}/kernel.d)
set(FLOPPY_IMG ${UNIDOS_OUT_DIR}/floppy.img)
set(NASM_BOOT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/boot/)
set(NASM_KERNEL_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/kernel/)
set(NASM_STAGE2_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/boot/stage2/)

# Boot layout defaults:
# - LBA 0: stage1 boot sector
# - LBA 1..N: raw stage2 loader stored in reserved FAT sectors
set(STAGE2_RESERVED_SECTORS 5 CACHE STRING "Number of reserved sectors used to store raw stage2 after the boot sector")
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

add_custom_command(
  OUTPUT ${KERNEL_BIN}
  COMMAND ${NASM_EXECUTABLE} -i${NASM_KERNEL_INCLUDE_DIR} -f bin -MD ${KERNEL_DEPFILE} -o ${KERNEL_BIN} ${KERNEL_SRC}
  DEPFILE ${KERNEL_DEPFILE}
  DEPENDS ${KERNEL_SRC}
  COMMENT "Assembling kernel entry"
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
