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

## Memory map (stage1 relevant)

| Address | Usage |
|---|---|
| `0x7C00` | stage1 load address (`ORG 0x7C00`) |
| `0x7C00` | initial stack pointer |
| `0x10000` | stage2 load destination (`0x1000:0x0000`) |
| `0x0500` | BOOTINFO target location (planned; currently stage1 does not populate) |

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

## Stage2 responsibilities (current/future)

Stage2 owns:

- FAT12 root directory scan for `KERNEL.BIN` (or selected kernel file)
- FAT12 cluster-chain traversal
- Kernel image load and transfer
- BOOTINFO population/finalization (as protocol evolves)

## Stage2 header notes

Stage2 begins with executable bytes (entry jump), followed by a fixed-offset header.
Stage1 validates header magic/version before jumping to stage2 entry.

The stage2 header currently uses magic `BTP1` and version fields for compatibility checks.