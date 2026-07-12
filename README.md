# FontaineOS

FontaineOS is an advanced, bare-metal x86 micro-kernel operating system built entirely from scratch using Freestanding C++20 and x86 Assembly. Operating with absolutely zero external runtime dependencies and completely divorced from the standard library (`libstdc++`/`glibc`), it manages raw x86 CPU systems, configures physical registers, routes asynchronous hardware lines, and orchestrates an interactive block-storage terminal workstation [x].

---

## 🔧 Core Architectural Principles
* **Pure Freestanding Architecture:** Zero runtime abstraction layers. Built with strict `-ffreestanding` parameters, managing all low-level system attributes manually [x].
* **Hardware Register Layer Translation:** Directly manipulates hardware lines through inline assembly port interactions (`inb`, `outb`, `insw`, `outsw`) [x].
* **Atomic Protection & Synchronization:** Shields volatile memory tracking arrays using strict hardware interrupt constraints (`cli`/`sti`) [x].

---

## 🗺 Completed Chronological Roadmap

### 📦 Phase 1: Core System & Privilege Segments
* **[x] Bootloader Entry Assembly:** Leverages Multiboot-compliant headers to hand execution safely over from GRUB straight to C++ code [x].
* **[x] GDT Realignment:** Reconfigures the Global Descriptor Table to implement a flat memory space layout, defining Ring 0 supervisor and Ring 3 user code/data spaces [x].
* **[x] IDT Exception Handlers:** Configures the Interrupt Descriptor Table to hook all 32 core CPU faults alongside critical hardware IRQs [x].

### ⏱ Phase 2: System Clock & Input Engineering
* **[x] PIT Timing Loops:** Re-programs the Programmable Interval Timer (Channel 0, Port `0x40`) to generate precise 100Hz hardware clock ticks [x].
* **[x] Keyboard Driver Engineering:** Hooks IRQ 1 (Port `0x60`) to catch raw scan keys asynchronously, deploying a strict Break-Code scancode gate (`scancode & 0x80`) to isolate valid keypresses [x].

### 🧠 Phase 3: Advanced Memory Architecture
* **[x] Physical Memory Bitmap Allocation:** Tracks every individual 4KB physical page across 64MB of RAM using a continuous bit-array allocation mapping grid [x].
* **[x] Virtual Memory Page Translation:** Mounts a Page Directory and secondary Page Tables to enable x86 CR3 hardware paging, identity-mapping the first 4MB of memory [x].
* **[x] Bare-Metal Heap Framework:** Deploys a First-Fit linked-list block tracker to drive working, byte-aligned `kmalloc()` and `kfree()` allocators [x].

### 🧵 Phase 4: Multitasking & Concurrent Schedulers
* **[x] Thread Control Block (TCB) Layer:** Spawns parallel runtime threads with completely isolated, heap-allocated 1KB task execution stacks [x].
* **[x] Cooperative Context Switcher:** Implements an x86 stack-frame saving mechanism to backup general-purpose registers, cycling thread execution states concurrently [x].

### 💾 Phase 5: Storage Architecture & Shell Workstation (Built Beyond Original Roadmap)
* **[x] Atomic Interrupt-Level Parser Shell:** Pulls the interactive prompt processing out of loose user loops and mounts it entirely inside the hardware interrupt ring [x].
* **[x] System Diagnostics Commands:** `uptime` reports wall time straight off the 100Hz PIT tick counter, and `meminfo` walks the PMM allocation bitmap live to report used/free physical pages plus the active scheduler thread count [x].
* **[x] ATA/IDE Hard Drive Block Driver:** Communicates directly with motherboard disk ports (`0x1F0`–`0x1F7`) using 28-bit Logical Block Addressing (LBA) [x].
* **[x] Specialized Read/Write Hardware Wait Synchronization:** Solves x86 deadlock traps by implementing separate, dedicated synchronization loops: `ata_wait_read()` (polling BSY + DRQ) and `ata_wait_write()` (polling BSY exclusively) [x].
* **[x] Flat Global I/O Landing Pads:** Completely eliminates stack alignment drift under high-level compiler optimizations by routing block bursts into static, 4-byte aligned global memory arrays (`ata_io_buffer`, `disk_test_pad`) [x].
* **[x] Universal Terminal Scrolling Engine:** Manages video buffer layouts dynamically by checking rows (`cursor_position >= 4000`), executing memory block copies to shift rows upward, and blanking out the bottom line cleanly [x].

---

## 🛠 System Compilation & Build Stack
* **Compiler:** `g++` (Using flags: `-m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti -std=c++20`) [x]
* **Linker:** `ld` (Targeting architecture map: `-m elf_i386 -T linker.ld`) [x]
* **Assembler:** `nasm` (Targeting system layout format: `-f elf32`) [x]
* **Virtualizer emulation core:** `qemu-system-i386` (With explicitly configured format boundaries: `-drive file=bin/disk.img,format=raw,index=0,media=disk`) [x]

---

## 🚀 The Automated Continuous Integration Script (`build.sh`)
FontaineOS utilizes a custom, production-grade deployment tool script that completely handles the development lifecycle [x]:
1. **Scrubs Workspace Assets:** Erases dead cached intermediate object models (`make clean`) [x].
2. **Initializes Master Block Drive Media:** Uses `dd` to generate a 10MB raw disk space image and injects a real MBR structural signature placeholder (`\x55\xAA`) into sector block 0 to unlock emulator status pins [x].
3. **Validates Module Constraints:** Compiles the files, checking for code layout bugs [x].
4. **Historical State Backups:** Saves a timestamped backup point copy inside your build directory for permanent tracking safety [x].
5. **Syncs Remote Logs:** Pushes your progress straight to your master branch workspace repository on GitHub [x].
6. **Hardware Emulation Execution:** Launches QEMU, mounting your kernel and raw block storage drives safely [x].
