# FontaineOS Architecture

A technical tour of everything the kernel does, in boot order. Every claim here
traces to a source file; paths are relative to the repo root.

Contents:

1. [Boot path](#1-boot-path)
2. [GDT — six descriptors including the TSS](#2-gdt--six-descriptors-including-the-tss)
3. [IDT — the five live gates](#3-idt--the-five-live-gates)
4. [Preemptive PIT scheduler](#4-preemptive-pit-scheduler)
5. [FontFS — the on-disk filesystem](#5-fontfs--the-on-disk-filesystem)
6. [Ring 3 — the user-mode model](#6-ring-3--the-user-mode-model)
7. [User program ABI and the `run` loader](#7-user-program-abi-and-the-run-loader)
8. [Memory map](#8-memory-map)

---

## 1. Boot path

`src/boot.s` opens with a Multiboot header (magic `0x1BADB002`, flags 0,
checksum) in its own `.multiboot` section, which `linker.ld` places first in
`.text` at the 1 MB mark (`. = 1M;`). GRUB/QEMU's `-kernel` loader jumps to
`_start`, which does exactly two things: point `esp` at a 16 KB `.bss` boot
stack (`stack_bottom`/`stack_top`, boot.s) and `call kernel_main`.

`kernel_main` (`src/kernel.cpp`) then initializes, in order:

| Step | Call | What it does |
|---|---|---|
| 1 | `init_gdt()` | 6-entry GDT + TSS, reloads segments, `ltr` (§2) |
| 2 | `init_idt()` | 256-entry IDT, arms gates 0/13/32/33/0x80 (§3) |
| 3 | `init_timer(100)` | remaps the PIC to vectors 32/40, programs the PIT to 100 Hz |
| 4 | `init_pmm(64MB)` | physical page bitmap; low 2 MB (512 pages) permanently reserved (`src/pmm.cpp`) |
| 5 | `init_vmm()` | identity-maps the first 4 MB (supervisor, flags `0x03`), enables paging via CR3/CR0 (`src/vmm.cpp`) |
| 6 | `init_heap(0x300000, 256)` | first-fit `kmalloc`/`kfree` heap at 3 MB (`src/heap.cpp`) |
| 7 | `init_keyboard()` | IRQ 1 scancode driver + interactive shell (`src/keyboard.cpp`) |
| 8 | `init_multitasking()` + `create_thread(...)` ×4 | task 0 (kernel/idle) plus Alpha, Beta, Gamma, and the ring-3 launcher (§4, §6) |
| 9 | `fontfs_mount()` | reads LBA 0, checks the FontFS magic, prints mount status on row 8 (§5) |
| 10 | `sti` + `hlt` loop | kernel_main becomes the always-runnable idle task 0 |

## 2. GDT — six descriptors including the TSS

`src/gdt.cpp` builds a flat 4 GB memory model in `struct gdt_entry gdt[6]`:

| Index | Selector | Descriptor | Access byte |
|---|---|---|---|
| 0 | — | mandatory null | `0x00` |
| 1 | `0x08` | kernel code, ring 0, base 0, limit 4 GB | `0x9A` |
| 2 | `0x10` | kernel data, ring 0 | `0x92` |
| 3 | `0x18` | user code, **DPL=3** (used from ring 3 as `0x1B` = `0x18\|RPL 3`) | `0xFA` |
| 4 | `0x20` | user data, **DPL=3** (used as `0x23`) | `0xF2` |
| 5 | `0x28` | TSS descriptor (base = `&tss`, byte-granular limit) | `0x89` |

The single system-wide TSS is used for exactly one thing: when an interrupt or
`int 0x80` arrives while the CPU is at CPL=3, the hardware refuses to push the
interrupt frame onto the untrusted user stack and instead loads `ss0:esp0` from
the TSS. `init_gdt` zeroes the TSS, sets `ss0 = 0x10`, leaves `esp0 = 0` (so a
skipped arming step faults loudly), and sets `iomap_base` past the end of the
segment — meaning "no I/O permission bitmap", so every ring-3 `in`/`out`
instruction takes a #GP. `tss_set_kernel_stack()` re-points `esp0` at a fresh
kernel stack before every drop into ring 3. Finally the code executes
`lgdt`, a far jump to reload CS with `0x08`, data-segment reloads with `0x10`,
and `ltr` with `0x28`.

## 3. IDT — the five live gates

`src/idt.cpp` clears all 256 gates, then arms five (all through selector `0x08`):

| Vector | Handler stub (boot.s) | Gate flags | Purpose |
|---|---|---|---|
| 0 | `isr0_handler_stub` | `0x8E` | divide-by-zero → red panic banner, halt |
| 13 | `isr13_handler_stub` | `0x8E` | #GP → `gpf_handler` (kills ring-3 offender, panics on ring-0 fault). The CPU pushes an **error code** for vector 13; the stub passes it (plus the faulting CS) to C++ and explicitly pops it before `iret` |
| 32 | `irq0_handler_stub` | `0x8E` | PIT tick → `timer_handler` → preemptive `schedule()` |
| 33 | `irq1_handler_stub` | `0x8E` | keyboard → `keyboard_handler` (scancode decode + shell parser) |
| 0x80 | `isr80_syscall_stub` | **`0xEE`** | system call gate. `0xEE` = present 32-bit interrupt gate with **DPL=3** — the one byte that lets CPL=3 code execute `int 0x80` at all; everything else at DPL=0 would #GP |

Because IRQs can now preempt CPL=3 code (which holds user selectors `0x23` in
`ds/es/fs/gs`, and `pusha` does not save segment registers), every stub saves
the four data-segment registers by hand, loads kernel `0x10` for the C++
handler, and restores the caller's originals before `iret` (boot.s, IRQ0/IRQ1/
ISR13/ISR80 stubs).

The PIC is remapped in `remap_pic()` (`src/timer.cpp`): master to vector 32,
slave to vector 40, so hardware IRQs no longer collide with CPU exceptions.

## 4. Preemptive PIT scheduler

`init_timer(100)` programs PIT channel 0 (command `0x36`, ports `0x43`/`0x40`)
with divisor `1193182 / 100`, so IRQ 0 fires at 100 Hz (`TIMER_HZ 100`,
`include/timer.h`).

Each task is a `thread_control_block` (`include/task.h`): id, saved `esp`,
entry function, state, `wake_tick`, and a `next` pointer forming a **circular
round-robin ring**. States are `THREAD_READY` / `THREAD_RUNNING` /
`THREAD_SLEEPING`. Task 0 is the kernel/idle timeline itself; it never sleeps,
which guarantees the ring always has at least one runnable task
(`init_multitasking`, `src/task.cpp`).

`create_thread()` allocates a private 4 KB stack from the heap and fabricates
exactly the frame `context_switch` expects: a return address pointing at
`thread_bootstrap`, below a zeroed 32-byte `pusha` frame. `thread_bootstrap`
re-enables interrupts (`sti`), calls the entry function, and yields forever if
it ever returns.

`context_switch(old_esp_slot, new_esp)` is a raw assembly routine in
`src/boot.s` — deliberately not inline asm, so `-O2` cannot wrap it with
prologue/epilogue stack traffic. It does `pusha`, parks `esp` into the outgoing
TCB, mounts the incoming task's `esp`, `popa`, `ret`.

Preemption happens in `timer_handler` (`src/timer.cpp`): increment
`timer_ticks`, send the PIC EOI **before** switching (otherwise the outgoing
task's frozen frame would hold the acknowledgement hostage and no further
ticks would ever fire), then call `schedule()` every `SCHEDULE_QUANTUM` ticks
(currently 1, i.e. a 10 ms time-slice). `schedule()` runs with IF=0 inside the
IRQ and never executes `sti`/`cli` itself — the eventual `iret` restores the
resumed task's saved EFLAGS. Cooperative paths exist too: `switch_task()`
(explicit yield, fenced by `cli`/`sti`) and `sleep(ticks)`, which sets
`wake_tick = timer_ticks + ticks`, marks the task SLEEPING, and reschedules.
`pick_next_runnable()` walks the ring once, promoting any sleeper whose alarm
expired back to READY, and returns the first READY/RUNNING task.

The boot screen demonstrates this: Alpha and Beta are pure CPU-bound loops
with **no yield anywhere**, yet their tickers advance concurrently, and Gamma
ticks once per second via `sleep(100)` (`src/kernel.cpp`).

## 5. FontFS — the on-disk filesystem

FontFS (`include/fontfs.h`, `src/fontfs.cpp`) is a deliberately minimal
persistent filesystem sitting on the polling ATA driver
(`src/ata.cpp`, one 512-byte sector per `ata_read_sector`/`ata_write_sector`).

On-disk layout:

| LBA | Contents |
|---|---|
| 0 | **Superblock**: magic `0xF047F5AA`, version, `max_files`, `filetable_lba`, `data_start_lba`, `blocks_per_file` (rest zero) |
| 1 | **File table**: one sector = 16 directory entries of exactly 32 bytes each |
| 2 – 257 | **Data area**: 16 fixed regions of 16 sectors (8192 B) each |

A directory entry is `{ char name[20]; u32 size; u32 start_lba; u8 in_use;
u8 pad[3]; }` (packed, 32 bytes). Hard limits: **16 files**, **8192 bytes per
file**, **names up to 19 characters** (`FONTFS_NAME_MAX 20` includes the NUL).

Slot *i*'s disk region is a pure function of its index
(`DATA_START_LBA + i * BLOCKS_PER_FILE`), so there is no allocation bitmap, no
fragmentation, and create/delete/rename is a single read-modify-write of LBA 1.
`fontfs_mount()` reads LBA 0 and checks the magic — that check *is* the
"filesystem present?" test the boot banner reports on row 8.
`fontfs_format()` writes a fresh superblock and an empty, self-describing file
table.

Context note: the shell parser runs **inside the keyboard IRQ handler**, and
FontFS issues its synchronous polling ATA transfers from there — the same
proven pattern as `disktest`. The ATA driver gates off the drive IRQ line
(nIEN via port `0x3F6`), so no stray IRQ 14 fires mid-transfer, and all
sector-sized scratch buffers are static globals, never on the small
IRQ-context stack.

The host-side twin of this layout is `tools/fontfs_inject.py`, which writes
files into `bin/disk.img` byte-for-byte the way the kernel would (see
`tools/README.md`).

## 6. Ring 3 — the user-mode model

FontaineOS uses the **controlled-jump model** (rationale at the top of
`src/syscall.cpp`): an ordinary scheduled kernel thread (`ring3_demo_task`)
drops into ring 3 with `enter_usermode()`; the user code runs at CPL=3 until it
retires via `SYS_EXIT` or is killed by the GPF handler, at which point control
lands back in that same kernel thread as if `enter_usermode` had simply
returned. Ring-3 tasks are not separate TCBs in the scheduler ring — but they
are still **fully preemptible**, because `enter_usermode` forces IF=1 in the
fabricated EFLAGS, so every PIT tick during a user slice performs a genuine
ring3→ring0 transition through the TSS `esp0` stack and rotates the other tasks.

The pieces, all in `src/boot.s` + `src/syscall.cpp`:

* **Paging flip** — `init_vmm()` maps the first 4 MB supervisor-only (`0x03`).
  Before the first drop, `make_low_memory_user_accessible()` ORs the User bit
  (`0x04`) into PDE 0 and all 1024 PTEs, then reloads CR3 to flush the TLB.
  Honest caveat (documented in the source): this makes the whole first 4 MB
  readable/writable from ring 3 — the protection demonstrated in this
  milestone is the *privilege-level* kind, not memory isolation.
* **TSS arming** — `tss_set_kernel_stack()` points `esp0` at the top of a
  dedicated 8 KB `ring0_syscall_stack` before every ring-3 entry.
* **`enter_usermode(eip, stack_top)`** — the "iret trick": parks the kernel's
  registers and `esp` in `saved_kernel_esp`, loads `0x23` into the data
  segments, then fabricates the 5-dword frame
  `SS=0x23 / ESP / EFLAGS(IF=1) / CS=0x1B / EIP` and executes `iretd`. Because
  the selectors carry RPL=3 and their descriptors DPL=3, the iret lands at
  CPL=3 — genuine user mode.
* **`kernel_reentry`** — the one-way door back: jumped to (never called) by
  `SYS_EXIT` or the GPF handler. It abandons whatever esp0 stack it stands on,
  remounts the kernel `esp` parked by `enter_usermode`, `popa`, `ret` — to the
  caller it looks like `enter_usermode` returned normally.
* **Caught-GPF demo** — the demo drops to ring 3 twice at boot: once for the
  happy-path hello (which reads its own CS and prints `CS=0x1B RPL=3` — the
  CPU cannot lie about CS), and once for `user_gpf_main`, which executes `cli`.
  With IOPL=0 at CPL=3 the CPU vetoes it with #GP; `gpf_handler` sees the
  faulting CS has RPL=3, prints
  `GPF caught: privileged op from ring 3 -- task killed, kernel fine`, and
  retires the task through `kernel_reentry`. A GPF at CPL=0 is a real kernel
  bug and halts with a panic banner instead.

One-user-context invariant: there is a single TSS esp0 stack, which is safe
because at most one user context ever exists at a time. Running two concurrent
ring-3 tasks would need a per-task esp0 swap in the scheduler (documented in
`src/syscall.cpp` so nobody adds one casually).

## 7. User program ABI and the `run` loader

The ABI (specified in `include/syscall.h`, exemplified in `user/hello.asm`
and `user/count.asm`):

* **Flat 32-bit binary, no header.** Byte 0 of the file is the entry point.
  Assembled with `nasm -f bin` and `org 0x180000`; no relocation is performed.
* **Loaded at `USER_PROG_LOAD_ADDR = 0x180000`** (1.5 MB), max
  `USER_PROG_MAX_SIZE = 8192` bytes — exactly one FontFS file region, so any
  readable file fits.
* **Runs at CPL=3.** The only doors into the kernel are `int 0x80` and
  faulting. The kernel supplies a valid ESP (top of a private 4 KB stack).
* **Syscall convention** (classic 32-bit Linux flavour): `eax` = number,
  `ebx/ecx/edx` = args, result in `eax`:

  | # | Name | Arguments | Behaviour |
  |---|---|---|---|
  | 1 | `SYS_WRITE` | ebx = NUL-terminated string, ecx = VGA color | prints on the syscall console (VGA rows 5–7) |
  | 2 | `SYS_READ` | — | stub: always returns 0 ("no input") in this milestone |
  | 3 | `SYS_YIELD` | — | cooperative reschedule on the caller's behalf |
  | 4 | `SYS_EXIT` | ebx = exit code (currently ignored) | retires the task via `kernel_reentry`; never returns |

  The assembly stub (`isr80_syscall_stub`, boot.s) forwards all four registers
  to `syscall_dispatcher(num, a1, a2, a3)` and plants the return value into
  the saved-eax `pusha` slot so `popa` hands it back to the ring-3 caller.

* **Programs must terminate with `SYS_EXIT`** — falling off the end executes
  whatever bytes follow the image and usually gets killed by the GPF handler
  (harmless to the kernel, but sloppy).

**The deferred launch design.** The shell's `run <file>` handler executes
inside the keyboard IRQ, where entering ring 3 would be unsafe on two counts
(full rationale above `user_prog_pending` in `src/syscall.cpp`): the keyboard
EOI is only sent at the end of the handler, and `enter_usermode` never returns
by falling through, so the PIC would wedge with IRQ 1 in-service; and the
half-finished IRQ stack frame parked for `kernel_reentry` would no longer
match reality. So the IRQ side only **loads**: `run` reads the file from
FontFS into a buffer (`fontfs_read`, same proven polling-ATA-from-IRQ pattern
as `cat`), calls `user_program_submit()` — which copies the image to
`0x180000` *before* raising the volatile `user_prog_pending` flag, so a torn
image can never be observed — and returns from the IRQ normally. The launcher
(`ring3_demo_task`, after its boot demo) is an ordinary scheduled thread that
polls the flag every 10 ticks (100 ms), resets the syscall console, re-arms
TSS `esp0`, drops to CPL=3 at `0x180000`, and clears the flag only after the
program retires — a second `run` during execution is refused (`-1 busy`)
instead of clobbering the image, preserving the one-user-context invariant.

## 8. Memory map

The low-4 MB layout (documented and justified in `include/syscall.h`):

| Range | Contents |
|---|---|
| `0x000000 – 0x0FFFFF` | real-mode/BIOS/VGA region (VGA text buffer at `0xB8000`) |
| `0x100000 – ~0x113000` | kernel image (`linker.ld` places it at 1 MB): `.text`/`.rodata`/`.data`/`.bss`, including the 16 KB boot stack, FontFS/ATA sector buffers, the shell's 8 KB file buffer, the 4 KB user demo stack, and the 8 KB TSS esp0 stack |
| `0x180000 – 0x182000` | **user program load region** (8 KB, = FontFS max file size) |
| `0x200000 – 0x2FFFFF` | free (first RAM the PMM would hand out) |
| `0x300000 – 0x3FFFFF` | kernel heap: `init_heap(0x300000, 256 pages)` = 1 MB, source of the per-thread 4 KB stacks |

The load region is safe on three counts: it lies inside the identity-mapped
first 4 MB whose pages are flipped user-accessible; it lies inside the PMM's
permanently reserved low 2 MB (`init_pmm` marks pages 0–511 used forever, so
`pmm_alloc_page()` can never hand them out); and it clears the kernel image end
by >400 KB while staying clear of the heap.

Paging structures (`src/vmm.cpp`) are two page-aligned static arrays inside the
kernel image — `page_directory[1024]` and `first_page_table[1024]` — with only
PDE 0 present, identity-mapping `0x000000–0x3FFFFF`. Physical memory is
tracked by a bitmap over 64 MB of 4 KB pages (`src/pmm.cpp`).
