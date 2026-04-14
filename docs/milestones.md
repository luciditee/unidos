# Unidos Milestone Checklist (386 bring-up -> shell)

This checklist is ordered for minimum-risk bring-up on 80386 hardware and emulators.

Status key:
- [ ] not started
- [~] in progress
- [x] complete

---

## 0) Protocol + bring-up invariants (freeze first)

Goal: ensure bootloader->kernel handoff remains stable while kernel complexity grows.

- [X] Keep `docs/boot-protocol.md` authoritative for stage2->kernel state.
- [X] Verify handoff register contract uses **ECX** for kparams length (`xor ecx, ecx`, `mov word cx, [length]`).
- [X] Add a small "protocol assertions" section in kernel entry comments (expected `CS/DS/SS`, `DL`, `ESI`, `ECX`, `IF`, `DF`).
- [X] Add an early panic path that prints a clear fatal code and halts.

Exit criteria:
-[X] A boot with no kparams and a boot with kparams both reach the same kernel entry path.
- [X] Mismatch/fatal paths are visible on screen and deterministic.

---

## 1) Early kernel platform init (descriptor tables + safe defaults)

Goal: establish deterministic protected-mode execution environment under kernel control.

### 1.1 GDT hardening
- [X] Define a descriptive GDT layout with named selectors:
	- null descriptor
	- kernel code (ring 0, 32-bit, base=0, limit=4GiB)
	- kernel data (ring 0, 32-bit, base=0, limit=4GiB)
	- user code (ring 3, 32-bit, base=0, limit=4GiB) *(can be unused initially, define now)*
	- user data (ring 3, 32-bit, base=0, limit=4GiB) *(can be unused initially, define now)*
	- TSS descriptor (ring 0 system segment)
- [X] Place GDT at a fixed known kernel symbol/location and load with `lgdt` in kernel init.
- [X] Reload segment registers and perform far jump to activate new `CS`.

### 1.2 TSS baseline
- [X] Allocate one TSS structure and load with `ltr`.
- [X] Set `SS0:ESP0` for privilege transitions (even before user mode, prepare now).
- [~] Keep I/O bitmap disabled or all-deny initially.

Exit criteria:
- Kernel runs using its own GDT (not bootloader table).
- `ltr` succeeds and no fault occurs on descriptor reload.

---

## 2) IDT + exceptions before external IRQs

Goal: avoid triple-faults and make failures diagnosable.

### 2.1 IDT structure and stubs
- [X] Build a full 256-entry IDT at a fixed kernel symbol/location.
- [X] Install default "unexpected vector" handler for all entries.
- [X] Install explicit handlers for at least:
	- `#DE` (0)
	- `#UD` (6)
	- `#GP` (13)
	- `#PF` (14)
- [X] Save register context in a consistent trap frame structure.

### 2.2 Fault reporting
- [X] For faults, print vector number + error code + `EIP/CS/EFLAGS`.
- [X] Halt cleanly after fatal faults.

Exit criteria:
- Triggering intentional divide-by-zero produces diagnostic output, not reboot/triple-fault.

---

## 3) PIC remap + PIT + keyboard IRQ path

Goal: enable interrupts safely and prove IRQ servicing works.

### 3.1 PIC setup
- [X] Remap PIC vectors (typical: master `0x20`, slave `0x28`).
- [X] Mask all IRQ lines initially.
- [X] Implement correct EOI handling (slave first if IRQ >= 8, then master).

### 3.2 PIT setup
- [X] Program PIT channel 0 to desired scheduler tick (e.g. 100 Hz start point).
- [X] Route IRQ0 to timer handler and count ticks.

### 3.3 Keyboard IRQ setup
- [X] Unmask IRQ1 and install keyboard ISR.
- [X] Read scan code from port `0x60` and acknowledge controller/PIC correctly.
- [X] Demonstrate ISR by printing `K` at current cursor position.

Exit criteria:
- `sti` enabled without crash.
- Timer ticks increment continuously.
- Keypress emits visible `K` and system remains stable.

---

## 4) Scheduler proof milestone ("1/2/K" demo)

Goal: prove preemption + ISR interaction with basic kernel tasks.

- [X] Create two kernel tasks/threads:
	- task A writes `1`, burns cycles
	- task B writes `2`, burns cycles
- [X] Implement timer-driven round-robin switch between A and B.
- [X] Preserve/restore full software context (`EIP`, `ESP`, GPRs, `EFLAGS`; segment registers as needed).
- [X] Ensure IRQ1 keyboard handler can preempt and print `K` while tasks alternate.

Exit criteria:
- On screen, mixed stream of `1`, `2`, and `K` appears without lockup or corruption.

---

## 5) Physical memory manager + paging bootstrap

Goal: move from flat physical assumptions to managed virtual memory.

### 5.1 Physical memory discovery/allocation
- [X] Establish memory map strategy (BIOS E820 preferred, fallback rules documented).
- [X] Implement page-frame allocator (bitmap or free-list, 4KiB pages).
- [X] Reserve kernel image, boot structures, and device memory regions.

### 5.2 Paging enablement
- [X] Build page directory/page tables for kernel mappings.
- [X] Keep an initial identity mapping window for bring-up simplicity.
- [X] Enable paging (`CR0.PG=1`) and verify execution continuity.
- [X] Add robust `#PF` handler diagnostics (fault addr from `CR2`, error code decode).

### 5.3 Post-bootstrap paging/runtime gates
Note: 5.3 and 5.4 items may be implemented in parallel with milestone 6. They are not required to declare Milestone 5.2 complete, but are required for stable kernel/user bringup in Milestone 6.

- [X] `#PF` handler prints [X] CR2, [X] raw error code, [X] P/W/U, and [X] EIP/CS.
- [X] Paging API exists and works: `map`, `unmap`, `query`. Workable early TLB flush policy implemented.
- [X] Controlled user-mode entry path exists (`iret` into ring3 works)
- [X] Fault policy split exists:
    - [X] Kernel fault => panic
	- [X] User fault => kill offending task

### 5.4 Higher-Half Refactor
- [X] Linker script updated to reflect kernel image copy to high memory
- [X] .bss section explicitly zeroed in kernel init assembly
- [X] PMM frames and VMM pages reserved for kernel code and kernel stack, with [X] guard page at end of stack (with value definable as N pages, default 4 e.g. 16KiB of stack space, likely more in practical use)
- [X] Kernel remapped to new region and far jump handled accordingly

Exit criteria:
- Kernel runs with paging on.
- Intentional unmapped access triggers readable page-fault diagnostics.

---

## 6) Kernel/user boundary + syscall entry

Goal: support ring 3 processes safely.

- [X] Define trap/interrupt gate policy (`DPL=3` only where intended).
- [X] Implement syscall entry path (software interrupt or fast trap strategy).
- [X] Wire `TSS.ESP0`-based privilege stack switch.
- [X] Implement safe user-memory copy helpers (`copyin/copyout`) with fault handling.
- [X] Add minimal syscall set (recommended first pass):
	- `write` (console)
	- `exit`
	- `yield` or `nanosleep` equivalent stub

Exit criteria:
- A trivial ring3 test program can invoke syscall and return/exit without panic.

### Syscall ABI

System calls are activated via `int 0x80` (the `sysenter` and `syscall` instructions are unavailable prior to i686, and we are minspec'd to 80386 for now).

- On any system interrupt, the standard trap frame `trap_frame_t` passes the interrupt vector, which is parsed by the ISR. If the vector in the trap frame is `0x80`, the syscall handler is invoked.
- From there, the contents of `eax` are read to decode which system call is being emitted (`0`/`read`, `1`/`write`, `2`/`open`, etc.) and the syscall table is used to decode exact values and the C function pointers they map to.
- Syscall numeric IDs (to be passed in `eax`) should generally use Linux's standard. If not available, 4.4BSD's standard to be used as a fallback as I implement.
- Callers are also expected to populate GP registers `ebx`, `ecx`, `edx`, `esi`, `edi`, `ebp` with parameters for the C function that ultimately runs (in that order).
- Return values from the functions are placed back into `eax` when control returns from the C function to the `int 0x80` ISR stub, and `iret` returns control to the userspace program which initiated the system call.

Note: On return, only `eax` is guaranteed to contain a useful value -- other GP registers may be clobbered or undefined.

---

## 7) Process model MVP

Goal: run and manage multiple independent processes.

- [~] Define process/thread structures ([X] `pid`, [X] state, [X] kernel stack, [~]address space, [ ] file table refs).
- [X] Implement scheduler states: runnable, running, blocked, zombie.
- [X] Implement `fork` baseline (or `spawn` shortcut first, then `fork`).
- [X] Implement `exec` to replace address space with loaded image.
- [X] Implement `wait`/reap semantics for parent/child lifecycle.

Exit criteria:
- Parent can create child, child exits, parent reaps deterministically.

## 7.5) Process model stabilization

Some of this is covered by milestone 8.3, but as I progress it has become increasingly clear that some of these make more sense to do before the rest of milestone 8.

- [X] Define per-process CR3 (on context switch, load process CR3 when thread_current->proc->addr_space changes). Switch/flush behavior *must* be deterministic
- [X] Eager fork-copy of process code/data, not just trap frame (will do copy-on-write later)
- [X] `exec(2)` should replace process images + stack in new `mm_t` instance (create new `mm_t`, map code and stack there, copy it, swap current->proc->addr_space to new one, switch cr3, then do what I already do and set tf->eip and user-tail ESP/SS). Old image should stay intact on failure to avoid half-baked `mm_t` swaps.
- [X] `mm_destroy` on last process-thread exit/reap, honoring refcount
- [X] `fork(2)` should fully tear down any PMM or paging manager allocations done in the event of a failure-to-fork

Deferring copy-on-write, actual ELF binary loader, definable argv/envp, and proper `brk`/`mmap` syscalls until later.

Exit criteria:
- Successful parent/child process memory isolation test (child should be able to update value in its own address space without affecting parent)
- `waitpid` should return expected child + status
- One-shot `execve` success path and one error path

---

## 8) Block I/O + filesystem + executable loader

Goal: load arbitrary user programs from disk.

### 8.0 Object model + FD groundwork (do first)
- [X] Introduce kernel object model with refcount + type tags (`file`, `vnode`, `blockdev`).
- [X] Add per-process file descriptor table + system-wide open file table.
- [~] Preserve current console-only behavior while plumbing generic `read`/`write`/`close` file ops.
- [ ] Reserve and document block-device FD path (`open_file` -> `file_target` -> block layer).

### 8.1 Device discovery + generic block layer (floppy first, read/write v1)
- [ ] Define `block_device` + `block_ops` API (`read_blocks`, `write_blocks`, `flush`, `get_info`) with write support required in v1.
- [ ] Use LBA + block-count API for all higher layers (no filesystem assumptions at block layer).
- [ ] Bring up floppy FDC backend first; register canonical handle (`fd0`) to exercise existing floppy image-injection flow end-to-end.
- [ ] Keep driver probe registry shape (ordered probe list) at init, even if only floppy is populated first.
- [ ] Enumerate supported devices and register canonical handles (`fd0` first, later `hd0`, etc.).
- [ ] Add retry + surfaced error reporting at block-layer boundaries.
- [ ] Leave inline TODO hooks at key block/device glue points where partition offset/length translation will attach once MBR support is enabled.

### 8.2 Partition shim (MBR only, deferred during floppy-only bring-up)
- [ ] Defer active MBR parsing while floppy is the only target; keep whole-device paths usable until fixed-disk support lands.
- [ ] Add thin MBR parser for partition-capable block devices.
- [ ] Expose each partition as a child block device (`<dev>p1..p4`) with LBA offset/length.
- [ ] Keep partition handling transparent to filesystem code (filesystem sees a normal block handle).
- [ ] Defer extended/logical partitions initially, but document this explicitly.

### 8.3 Buffer cache
- [ ] Add shared block cache keyed by (`device`, `lba`) for metadata + data reads.
- [ ] Track at least `valid/dirty/busy` states and enforce basic concurrency discipline.
- [ ] Start with sync/write-through policy; leave delayed writeback as future optimization.

### 8.4 VFS scaffold + mount framework
- [ ] Introduce VFS objects (`superblock`, `vnode`, `dentry`/name cache, `file`).
- [ ] Define minimal FS operation table common to all filesystems.
	- [ ] mount/unmount
	- [ ] lookup
	- [ ] open/close
	- [ ] read
	- [ ] readdir
	- [ ] stat (or minimal equivalent)
- [ ] Add mount options parser scaffold (store `key[=value]` options even if not all are used yet).
- [ ] Define root mount policy (which boot device/partition becomes `/`).

### 8.5 FAT family driver (single core for FAT12/16/32)
- [ ] Implement one FAT driver with runtime FAT-type detection from BPB + cluster count.
- [ ] Keep FAT-type differences opaque above the filesystem layer.
- [ ] Implement FAT12 read-first path while keeping write-capable plumbing intact to avoid API churn; then generalize to FAT16/32.
- [ ] Implement path traversal + directory iteration + regular file read.

### 8.6 FAT permission sidecar (`UNIDOS.PRM`)
- [ ] Implement optional per-directory sidecar parser for `UNIDOS.PRM` metadata.
- [ ] Map entries to minimal permission byte (and reserve bits for future symlink/file-type hints).
- [ ] Define deterministic fallback when sidecar file/entry is missing.
- [ ] Gate behavior behind mount options scaffold so policy can later be toggled.

### 8.7 Executable loading bridge
- [ ] Refactor `execve` path to resolve executable via VFS path lookup.
- [ ] Introduce binary format dispatch (`flat` now, ELF later) behind a common loader interface.
- [ ] Keep current staging-`mm_t` commit semantics (old image unchanged on load failure).
- [ ] Load flat binaries with current shared code/data-page policy as temporary implementation.

### 8.8 Syscall bridge for shell-era userspace
- [ ] Add `open`, `close`, `read`, `lseek`, and minimal directory enumeration syscall surface.
- [ ] Keep `write` integrated with FD layer (stdout/stderr still map to console initially).
- [ ] Add minimal userspace test programs for path lookup, file read, and exec-by-path.

### 8.9 Hardening to prevent churn in Milestone 9 onward
- [ ] Ensure no sleeping/blocking in hard IRQ context across block + FS paths.
- [ ] Normalize internal error mapping to stable `-errno` returns.
- [ ] Add path normalization policy (`.`, `..`, duplicate `/`, max path length).
- [ ] Add failure-unwind tests for partial map/load/open paths.

Exit criteria:
- [ ] Kernel loads and runs a user program from disk by pathname.
- [ ] Same loader path works with at least one non-hardcoded pathname.
- [ ] VFS + block interfaces support adding a second filesystem without API breakage.

---

## 9) TTY + interactive shell milestone

Goal: reach minimally useful interactive system.

- [ ] Implement console/TTY driver split (VGA text console + tty discipline).
- [ ] Keyboard input path to tty buffer (line editing can be minimal).
- [ ] `init` process launches shell as foreground session.
- [ ] Shell supports basic command execution, exit status, and blocking wait.

Exit criteria:
- Boot reaches shell prompt and can execute at least one external program.

---

## 10) Reliability and hardening pass (before broad feature expansion)

Goal: reduce hidden failure modes before adding advanced features.

- [ ] Introduce kernel assertions and structured panic codes.
- [ ] Add interrupt nesting/reentrancy safeguards (critical sections + IRQ masking discipline).
- [ ] Verify no sleeping/blocking in hard IRQ context.
- [ ] Add basic stress tests for scheduler fairness and memory leaks.
- [ ] Add build-time/run-time config for debug vs release behavior.

Exit criteria:
- Long-running emulator session stays responsive with no silent corruption.

---

## Deferred milestone: V8086/DOS compatibility track

Goal: postpone until kernel fundamentals are stable.

- [ ] Define v8086 execution model and trap/reflect strategy for BIOS/DOS interrupts.
- [ ] Plan I/O permission and emulation boundaries (TSS I/O bitmap, virtual interrupt semantics).
- [ ] Add compatibility layer only after process, VM, and signal/fault paths are robust.

---

## Cross-cutting pitfalls to watch continuously

- [ ] Keep ABI docs and code synchronized (register widths, struct layouts, calling conventions).
- [ ] Preserve strict separation of bootloader concerns vs kernel concerns.
- [ ] Avoid hidden dependence on emulator quirks (validate in both QEMU and PCem).
- [ ] Treat every fault as diagnosable: never reboot without a visible reason in debug builds.
- [ ] Make on-disk and in-memory structure endianness/packing explicit.

## Tech debt to resolve
- [ ] Deprecate physical window at 0xD0000000 - Reliance on this substantially limits the amount of addressable memory the kernel may boot with. A physical window for mappings below 1MB is acceptable
- [ ] Proxy PD mapping at a fixed location in kernel address space - Ensure kernel-mode PD exists at a predictable place by keeping a guaranteed address range to walk the PD/PTs without having to have kernel CR3 active


