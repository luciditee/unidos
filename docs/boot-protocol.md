# Boot protocol

The following is the frozen v1 boot protocol for the current implementation.

## Guarantees

### Scope brief

- BIOS boot only (no EFI/GPT)
- FAT12 floppy media
- 80386+ target
- Stage1 is a strict 512-byte boot sector
- Stage1 does **not** parse FAT
- Stage2 is loaded from reserved sectors immediately after LBA 0

### Platform assumptions

This OS, kernel, bootstrap, etc. assumes an IBM PC/AT-class BIOS-compatible environment.

- Boot sector is loaded to physical `0x7C00` (`ORG 0x7C00`)
- Boot signature `0xAA55` is present
- `DL` contains BIOS boot drive on entry

### Stage1 contract (current)

Because of the 512-byte budget, stage1 is intentionally minimal:

1. Initialize real-mode state (`cli`, `cld`, stack setup)
2. Save boot drive from `DL`
3. Reset floppy controller (`int 13h`, `AH=0`)
4. Load stage2 from fixed reserved sectors (LBA 1..N) into physical `0x10000`
5. Validate stage2 header magic/version at a fixed offset
6. Far jump to stage2 entry (`0x1000:0x0000`)

### Handoff state to stage2

Guaranteed at jump point:

- `CS:IP = 0x1000:0x0000`
- `DL = boot drive` (as preserved by stage1)
- `SP = 0x7C00` (stage2 may immediately replace stack)
- `IF = 0`, `DF = 0`

Not guaranteed by stage1:

- `A20` enabled
- General-purpose registers zeroed
- BOOTINFO populated at `0x0500`

## Disk layout contract (v1)

Stage2 is not loaded via FAT in stage1. Instead, it is stored in the FAT reserved region.

- LBA 0: stage1 boot sector + BPB
- LBA 1..`STAGE2_RESERVED_SECTORS`: raw stage2 image
- FAT region starts after reserved sectors

Reserved sector count in BPB must match image build settings:

`ReservedSectors = 1 + STAGE2_RESERVED_SECTORS`

## Memory map (bootloader relevant)

| Address | Usage |
|---|---|
| `0x7C00` | stage1 load address (`ORG 0x7C00`) |
| `0x7C00` | initial stack pointer |
| `0x10000` | stage2 load destination (`0x1000:0x0000`) |
| `0x0500` | BOOTINFO finalized copy location (`0000:0500`) |
| `0x00100000` | kernel load destination and protected-mode jump target |

## Floppy read procedure (stage1)

Stage1 reads one sector at a time using BIOS CHS (`int 13h`, `AH=0x02`) with retry.

Inputs for each read:

- `AL = 1` (one sector)
- `CH`, `CL`, `DH` from LBA→CHS conversion
- `DL = boot drive`
- `ES:BX = destination buffer`

Error behavior:

- Retry on `CF=1` up to configured maximum
- On failure, emit error message and wait for keypress
- After keypress, trigger BIOS restart (`int 19h`)

## Stage2 responsibilities (current)

Stage2 owns:

- A20 enable and unreal-mode setup
- FAT12 root directory scan for `KERNEL.BIN`
- FAT12 cluster-chain traversal
- Kernel image load to linear `0x00100000`
- Runtime CRC32 calculation while loading
- CRC32 verification before handoff (with user override: `F8` skip)
- BOOTINFO field finalization and copy to `0000:0500`
- Transition to 32-bit protected mode and kernel jump

## Integrity enforcement (stage2)

Stage2 computes kernel CRC32 incrementally while copying kernel bytes.

- CRC32 polynomial: `0xEDB88320` (reflected)
- Initial value: `0xFFFFFFFF`
- Finalization: XOR with `0xFFFFFFFF`
- Expected checksum source: build-generated `KERNEL_CHECKSUM`, embedded in stage2 image

Behavior:

- If CRC32 matches expected value, boot continues.
- If CRC32 mismatches, boot is aborted as a fatal error.
- If `F8` is detected in BIOS keyboard buffer at checksum-finalize time, CRC mismatch enforcement is skipped and boot continues.

## Stage2 -> kernel handoff contract (v1)

At kernel entry (`0x00100000`), stage2 guarantees:

- CPU mode: 32-bit protected mode
- Paging: disabled (`CR0.PG = 0`)
- A20: enabled
- Interrupt flag: cleared (`IF = 0`)
- Direction flag: cleared (`DF = 0`)
- Flat GDT active for handoff
- `CS = PM32_CODE_SELECTOR`
- `DS = ES = SS = FS = GS = PM32_DATA_SELECTOR`
- `ESP = PM32_STACK_TOP`
- Boot drive in `DL` (upper bits of `EDX` cleared by stage2)
- Kernel image is loaded at linear `0x00100000`

Not guaranteed by stage2:

- IDT initialized
- PIC/APIC remapped or configured
- Interrupt handlers installed
- Paging structures created
- Any higher-level runtime state beyond BOOTINFO and register/mode contract above

## BOOTINFO contract (v1)

Stage2 publishes BOOTINFO to `0000:0500` before protected-mode handoff.

Fields finalized dynamically by stage2:

- `totalSize`
- `bootDrive`
- `checksum32` (runtime-computed kernel CRC32)

All other BOOTINFO fields are static protocol/media metadata emitted from stage2 header constants.

Kernel code may treat `0x00000500` as the canonical BOOTINFO pointer on entry.

## Stage2 header notes

Stage2 begins with executable bytes (entry jump), followed by a fixed-offset header.
Stage1 validates header magic/version before jumping to stage2 entry.

The stage2 header currently uses magic `BTP1` and version fields for compatibility checks.