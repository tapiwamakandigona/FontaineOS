# FontaineOS

FontaineOS is a lightweight, bare-metal x86 micro-kernel operating system built entirely from scratch using Freestanding C++20 and Assembly. Operating with zero external dependencies and completely divorced from the standard library, it communicates directly with x86 CPU systems, physical registers, and basic hardware memory interfaces.

## 🔧 Core Architectural Principles
* **Pure Freestanding Environment:** No `glibc`, no standard allocation schemes, no native wrappers.
* **Direct Hardware Address Mapping:** Direct writing to hardcoded hardware lines like VGA memory profiles.
* **Strict Memory Containment:** Zero runtime allocation overhead until a secure physical paging engine is operational.

## 🗺 Chronological Roadmap
- [x] Phase A: Multiboot Bootloader Entrypoint Assembly & VGA Hardware Text Output Mapping
- [x] Phase B: Global Descriptor Table (GDT) Realignment & Custom Machine Interrupt Vectors
- [x] Phase C: Custom System Clock PIT (Programmable Interval Timer) Timing Loops
- [x] Phase D: Hardware Keyboard Interrupt Line Parsing & Scancode Map Buffering
- [x] Phase E: Physical Memory Manager Layer & Bitmap-Driven Page Allocator Core
- [x] Phase F: Virtual Memory Paging Systems & Basic Kernel Space Protection Matrices
- [x] Phase G: Kernel Heap Manager Matrix Support (`kmalloc` and `kfree` Implementation)
- [x] Phase H: Multitasking Architecture Layout & Cooperative Thread Control Blocks

## 🛠 Required Build Stack
* Compiler: `g++` (Freestanding C++20 mode flags)
* Linker: `ld` (Targeted machine maps)
* Assembler: `nasm` (ELF32 system layouts)
* Virtualizer: `qemu-system-i386` (Hardware simulation frame)
