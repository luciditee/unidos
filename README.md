# unidos - *NIX for DOS-era systems

Unidos is a UNIXlike kernel and user environment targeting period-correct 80386 hardware.

More specific documentation about what the project entails can be found in the `docs` directory.

## Prerequisites (Arch)

I am building on an Arch-based distro, but if you have access to a Linux machine with the following tools, you should be able to compile and run it.

- nasm
- cmake
- ninja
- qemu-system-i386 (optional but recommended)
- bochs (optional)
- dosfstools (`mkfs.fat`)
- mtools (`mcopy`)

I also recommend PCem for period-correct emulation (I use the AMIBIOS 386DX ROM for testing).

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
- Parameters passed to the kernel are stored in the filesystem root in `kparams.dat` and are loaded into `ESI` at kernel entry.
- Kernel contains basic VGA text driver for debugging on live hardware, and is currently getting descriptors set up to commence on real kernel work.

