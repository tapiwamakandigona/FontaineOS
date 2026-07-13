; Define constants for the Multiboot header alignment matrix
MAGIC    equ 0x1BADB002
FLAGS    equ 0x00
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .text
global _start
global isr0_handler_stub
global isr13_handler_stub  ; Expose the General Protection Fault trap stub
global isr80_syscall_stub  ; Expose the int 0x80 system call gate stub
global irq0_handler_stub
global irq1_handler_stub ; Expose our brand new keyboard interrupt gate stub label
global context_switch    ; Expose our raw scheduler stack-swapping engine
global enter_usermode    ; Expose the ring0 -> ring3 'iret trick' launcher
global kernel_reentry    ; Expose the ring3 -> ring0 permanent return path (sys_exit / GPF)

extern kernel_main
extern divide_by_zero_handler
extern timer_handler
extern keyboard_handler  ; Reference our external C++ keyboard routine handler
extern syscall_dispatcher ; The C++ system call demultiplexer (src/syscall.cpp)
extern gpf_handler        ; The C++ General Protection Fault reporter (src/syscall.cpp)

_start:
    ; Hand over our newly allocated stack boundary pointer to the CPU stack register
    mov esp, stack_top

    ; Jump directly into our main freestanding C++ environment kernel loop
    call kernel_main

    ; Safety fallback loop
    cli
halt_loop:
    hlt
    jmp halt_loop

; This is our raw low-level hardware entry stub for Interrupt 0 (Exception Trap)
isr0_handler_stub:
    pusha                    ; Push all general-purpose CPU registers onto the stack to save their state
    call divide_by_zero_handler ; Jump directly into our C++ error log function
    popa                     ; Restore all general-purpose CPU registers back to normal state
    iret                     ; Interrupt Return

; This is our raw low-level hardware entry stub for Interrupt 32 (System Timer IRQ 0)
;
; RING-3 UPDATE: now that the timer can preempt CODE RUNNING AT CPL=3, we can no
; longer assume ds/es/fs/gs hold kernel selectors on entry — a preempted user
; task leaves 0x23 (user data) in them. pusha does NOT save segment registers,
; so we save them by hand, load the kernel data segment 0x10 for the C++ handler,
; and restore the caller's originals before iret (so a preempted ring-3 task
; gets its 0x23 selectors back). The hardware side of the privilege dance is
; automatic: on a ring3 -> ring0 interrupt the CPU has ALREADY switched us onto
; the TSS ss0:esp0 kernel stack and pushed the user's SS/ESP before we run.
irq0_handler_stub:
    pusha                    ; Caches general-purpose CPU registers to protect active calculations
    push ds                  ; Segment registers are not covered by pusha — park the caller's set
    push es
    push fs
    push gs
    mov ax, 0x10             ; Mount the kernel data segment for the C++ handler code
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call timer_handler       ; Jump straight into our live ticking C++ routine block
    pop gs                   ; Hand the interrupted context its own segment set back
    pop fs
    pop es
    pop ds
    popa                     ; Restores register configurations cleanly
    iret                     ; Special return command explicitly pops instruction pointer matrices

; This is our raw low-level hardware entry stub for Interrupt 33 (System Keyboard IRQ 1)
; Same segment-hygiene pattern as the timer stub above: a keystroke can just as
; easily land while a ring-3 task holds the CPU with user selectors loaded.
irq1_handler_stub:
    pusha                    ; Caches general-purpose CPU registers to protect active calculations
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call keyboard_handler    ; Jump straight into our live parsing C++ driver block
    pop gs
    pop fs
    pop es
    pop ds
    popa                     ; Restores register configurations cleanly
    iret                     ; Special return command explicitly pops instruction pointer matrices

; ------------------------------------------------------------------
; Interrupt 13: GENERAL PROTECTION FAULT trap stub.
;
; CRITICAL DIFFERENCE from the IRQ stubs: vector 13 is one of the exceptions
; where the CPU pushes an ERROR CODE onto the stack AFTER eip/cs/eflags, so on
; entry the stack top is [error_code][eip][cs][eflags](+[esp][ss] if the fault
; came from ring 3). If we forgot that extra dword, iret would interpret the
; error code as the return EIP and fly into the weeds — the classic double
; fault. We pass the error code and the faulting CS to the C++ reporter, and
; explicitly discard the error code before iret.
;
; In practice our gpf_handler never returns for a ring-3 fault (it retires the
; user task through kernel_reentry) and halts for a ring-0 fault, but the stub
; is written to be correct either way.
; ------------------------------------------------------------------
isr13_handler_stub:
    pusha                    ; 32 bytes of general-purpose register state
    push ds                  ; plus the four data segment registers (16 bytes)
    push es
    push fs
    push gs
    mov ax, 0x10             ; C++ code needs kernel data segments mounted
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; Stack layout right now (offsets from esp):
    ;   +0..15  gs/fs/es/ds   +16..47 pusha frame
    ;   +48     error code    +52 eip   +56 cs   +60 eflags  (+64 esp, +68 ss if from ring 3)
    mov eax, [esp + 56]      ; the faulting CS — its low 2 bits are the faulting CPL
    push eax
    mov eax, [esp + 52]      ; the error code (offset shifted by 4 after the push above)
    push eax
    call gpf_handler         ; gpf_handler(error_code, faulting_cs)
    add esp, 8               ; drop the two C arguments
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 4               ; POP THE ERROR CODE the CPU pushed — iret does not do this!
    iret

; ------------------------------------------------------------------
; Interrupt 0x80: the SYSTEM CALL gate (reachable from ring 3 — DPL=3 in the IDT).
;
; ABI: eax = syscall number, ebx/ecx/edx = arguments, return value in eax.
; We forward all four to the C++ dispatcher as ordinary stack arguments, then
; overwrite the saved-eax slot inside the pusha frame so popa hands the ring-3
; caller the dispatcher's return value in eax.
;
; Segment discipline matters doubly here: the caller arrives with USER selectors
; (0x23) in ds/es/fs/gs and iret only restores cs/ss on the way out — if we left
; kernel 0x10 selectors in the data segment registers the resumed ring-3 code
; would fault on its very first memory access... actually worse: it would run
; with kernel data segments. So we save the user's set, run C++ on kernel
; segments, and restore the user's set before iret. The stack we are standing
; on is the TSS esp0 kernel stack (the CPU switched automatically because the
; int came from CPL=3).
; ------------------------------------------------------------------
isr80_syscall_stub:
    pusha                    ; Save the caller's general-purpose registers
    push ds
    push es
    push fs
    push gs
    mov eax, 0x10            ; Mount kernel data segments for the dispatcher
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, [esp + 44]      ; re-read the caller's eax (pusha slot: +16 segs, +28 into pusha)
    push edx                 ; arg3
    push ecx                 ; arg2
    push ebx                 ; arg1
    push eax                 ; syscall number
    call syscall_dispatcher  ; eax = syscall_dispatcher(num, arg1, arg2, arg3)
    add esp, 16              ; drop the four C arguments
    mov [esp + 44], eax      ; plant the return value into the saved-eax pusha slot
    pop gs                   ; give the ring-3 caller its user data segments back
    pop fs
    pop es
    pop ds
    popa                     ; restores registers, eax now carries the return value
    iret                     ; pops eip/cs/eflags/esp/ss and drops back to CPL=3

; ------------------------------------------------------------------
; enter_usermode(uint32_t user_eip, uint32_t user_stack_top)
;
; The 'iret trick': iret is the ONLY sane way to LOWER the current privilege
; level on x86. We fabricate on the kernel stack exactly the 5-dword frame the
; CPU would have pushed if ring-3 code had been interrupted, then execute iret
; so the CPU "returns" to an interruption that never happened:
;
;       [esp+16]  SS      = 0x23  (user data selector, RPL=3)
;       [esp+12]  ESP     = user_stack_top
;       [esp+ 8]  EFLAGS  with IF=1 (so the PIT keeps preempting ring-3 code;
;                                    IOPL stays 0 so ring-3 in/out/cli all #GP)
;       [esp+ 4]  CS      = 0x1B  (user code selector 0x18 | RPL 3)
;       [esp+ 0]  EIP     = user_eip
;
; Because SS/CS carry RPL=3 and their descriptors carry DPL=3, the iret lands
; at CPL=3: genuine user mode. Before the drop we park the kernel's own
; register state and stack pointer so kernel_reentry (below) can resurrect
; this exact call frame when the user task exits or faults.
; ------------------------------------------------------------------
enter_usermode:
    mov ecx, [esp + 4]       ; ecx = user entry point (EIP for the fabricated frame)
    mov edx, [esp + 8]       ; edx = top of the user task's stack
    pusha                    ; park the kernel's registers for kernel_reentry
    mov [saved_kernel_esp], esp ; remember exactly where the kernel context sleeps
    mov ax, 0x23             ; load USER data selectors — after iret the CPU does
    mov ds, ax               ; not touch ds/es/fs/gs, so they must already hold
    mov es, ax               ; ring-3 legal values or the first user memory access faults
    mov fs, ax
    mov gs, ax
    push dword 0x23          ; SS  = user data, RPL=3
    push edx                 ; ESP = user stack top
    pushf                    ; start from the current EFLAGS...
    pop eax
    or  eax, 0x200           ; ...force IF=1 so interrupts stay live inside ring 3
    push eax                 ; EFLAGS
    push dword 0x1B          ; CS  = user code 0x18 | RPL 3
    push ecx                 ; EIP = user entry point
    iretd                    ; drop to CPL=3 — user mode begins here

; ------------------------------------------------------------------
; kernel_reentry: the one-way door back OUT of user mode.
;
; Jumped to (never called) by the C++ side when the user task retires — either
; voluntarily via sys_exit or involuntarily when the GPF handler kills it.
; Whatever stack we are on at that moment (the TSS esp0 stack, mid-syscall or
; mid-fault) is simply abandoned: we re-mount the kernel stack exactly where
; enter_usermode parked it, pop the parked registers, and 'ret' — from the
; caller's point of view enter_usermode just returned normally. The data
; segments are already kernel 0x10 because every stub loads them on entry.
; ------------------------------------------------------------------
kernel_reentry:
    mov esp, [saved_kernel_esp] ; abandon the interrupt stack, remount the kernel frame
    popa                        ; restore the registers enter_usermode parked
    ret                         ; resume the kernel as if enter_usermode returned

; ------------------------------------------------------------------
; The Context Switch Engine: context_switch(uint32_t* old_esp_slot, uint32_t new_esp)
;
; Implemented as a raw assembly routine (instead of inline asm inside C++)
; so the compiler cannot inject its own prologue/epilogue stack traffic
; around our carefully fabricated register frames.
;
; On entry the stack holds: [esp] return address, [esp+4] old_esp_slot, [esp+8] new_esp
; ------------------------------------------------------------------
context_switch:
    mov eax, [esp + 4]   ; eax = pointer to the outgoing task's saved-esp slot
    mov edx, [esp + 8]   ; edx = the incoming task's parked stack pointer
    pusha                ; Save all 8 general-purpose registers (32 bytes) of the outgoing task
    mov [eax], esp       ; Park the outgoing task's stack pointer inside its control block
    mov esp, edx         ; Mount the incoming task's stack
    popa                 ; Restore the incoming task's 8 general-purpose registers
    ret                  ; Resume the incoming task (or enter thread_bootstrap on first run)

section .bss
align 16
stack_bottom:
    resb 16384 ; Allocate a 16KB uninitialized data area for our temporary system stack pointer
stack_top:

; The parking slot enter_usermode/kernel_reentry share: holds the kernel ESP
; captured just before the iret drop into ring 3, so the kernel context can be
; resurrected when the user task exits or is killed by the GPF handler.
saved_kernel_esp:
    resd 1
