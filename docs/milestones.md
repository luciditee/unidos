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
- [~] Add a small "protocol assertions" section in kernel entry comments (expected `CS/DS/SS`, `DL`, `ESI`, `ECX`, `IF`, `DF`).
- [~] Add an early panic path that prints a clear fatal code and halts.

Exit criteria:
-[X] A boot with no kparams and a boot with kparams both reach the same kernel entry path.
- [~] Mismatch/fatal paths are visible on screen and deterministic.

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
- [ ] Establish memory map strategy (BIOS E820 preferred, fallback rules documented).
- [ ] Implement page-frame allocator (bitmap or free-list, 4KiB pages).
- [ ] Reserve kernel image, boot structures, and device memory regions.

### 5.2 Paging enablement
- [ ] Build page directory/page tables for kernel mappings.
- [ ] Keep an initial identity mapping window for bring-up simplicity.
- [ ] Enable paging (`CR0.PG=1`) and verify execution continuity.
- [ ] Add robust `#PF` handler diagnostics (fault addr from `CR2`, error code decode).

Exit criteria:
- Kernel runs with paging on.
- Intentional unmapped access triggers readable page-fault diagnostics.

---

## 6) Kernel/user boundary + syscall entry

Goal: support ring 3 processes safely.

- [ ] Define trap/interrupt gate policy (`DPL=3` only where intended).
- [ ] Implement syscall entry path (software interrupt or fast trap strategy).
- [ ] Wire `TSS.ESP0`-based privilege stack switch.
- [ ] Implement safe user-memory copy helpers (`copyin/copyout`) with fault handling.
- [ ] Add minimal syscall set (recommended first pass):
	- `write` (console)
	- `exit`
	- `yield` or `nanosleep` equivalent stub

Exit criteria:
- A trivial ring3 test program can invoke syscall and return/exit without panic.

---

## 7) Process model MVP

Goal: run and manage multiple independent processes.

- [ ] Define process/thread structures (`pid`, state, kernel stack, address space, file table refs).
- [ ] Implement scheduler states: runnable, running, blocked, zombie.
- [ ] Implement `fork` baseline (or `spawn` shortcut first, then `fork`).
- [ ] Implement `exec` to replace address space with loaded image.
- [ ] Implement `wait`/reap semantics for parent/child lifecycle.

Exit criteria:
- Parent can create child, child exits, parent reaps deterministically.

---

## 8) Block I/O + filesystem + executable loader

Goal: load arbitrary user programs from disk.

### 8.1 Disk/block layer
- [ ] Introduce block device abstraction (even if floppy-only at first).
- [ ] Implement buffered block reads with retry/error surfaces.

### 8.2 Filesystem MVP
- [ ] Implement read-only FAT12 mount + path traversal sufficient for `/bin/*`.
- [ ] Add directory iteration + file read API.

### 8.3 Program format + crt0
- [ ] Choose executable format (ELF strongly recommended; flat binary only as temporary).
- [ ] Implement userspace image loader and initial user stack setup (`argc/argv/envp` plan).
- [ ] Provide crt0 that calls `main` then `exit`.

Exit criteria:
- Kernel loads and runs a user program from disk by pathname.

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

---

## Suggested immediate next 2-week target

1. Complete Milestones 1-3 fully.
2. Deliver Milestone 4 demo (`1/2/K`) as proof of preemption + IRQ correctness.
3. Begin Milestone 5 with allocator scaffolding and a functional `#PF` path.

