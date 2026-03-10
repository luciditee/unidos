# unidos (scaffold)

This repository is scaffolded for fast iteration on a BIOS/FAT12 floppy boot chain using NASM + CMake.

## Prerequisites (Arch)

- nasm
- cmake
- ninja
- qemu-system-i386 (optional but recommended)
- bochs (optional)
- dosfstools (`mkfs.fat`)
- mtools (`mcopy`)

## Build image

- Configure: `cmake --preset debug`
- Build: `cmake --build --preset debug`

Output image:

- `build/debug/out/floppy.img`

## Run

- QEMU: `cmake --build --preset run-qemu`
- Bochs: `cmake --build build/debug --target run-bochs`

## Current state

- `stage1` is the boot sector at LBA 0.
- `stage2` is written raw to reserved sectors starting at LBA 1.
- FAT12 is formatted with extra reserved sectors so the stage2 reserved area is not part of normal file allocation.
- `kernel` is copied into the FAT12 filesystem as `KERNEL.BIN`.
- Default reserved stage2 area is 4 sectors (2048 bytes).

## Next implementation steps

1. Implement stage1 fixed-sector read from LBA 1..N into `0x1000:0x0000` and transfer control.
2. Populate `BOOTINFO` at physical `0x0500`.
3. In stage2, implement FAT12 directory + FAT chain loader for `KERNEL.BIN`.
4. Implement stage2 checks for BOOTINFO header and required fields.
