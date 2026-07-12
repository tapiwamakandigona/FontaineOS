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
global irq0_handler_stub
global irq1_handler_stub ; Expose our brand new keyboard interrupt gate stub label
global context_switch    ; Expose our raw scheduler stack-swapping engine

extern kernel_main
extern divide_by_zero_handler
extern timer_handler
extern keyboard_handler  ; Reference our external C++ keyboard routine handler

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
irq0_handler_stub:
    pusha                    ; Caches general-purpose CPU registers to protect active calculations
    call timer_handler       ; Jump straight into our live ticking C++ routine block
    popa                     ; Restores register configurations cleanly
    iret                     ; Special return command explicitly pops instruction pointer matrices

; This is our raw low-level hardware entry stub for Interrupt 33 (System Keyboard IRQ 1)
irq1_handler_stub:
    pusha                    ; Caches general-purpose CPU registers to protect active calculations
    call keyboard_handler    ; Jump straight into our live parsing C++ driver block
    popa                     ; Restores register configurations cleanly
    iret                     ; Special return command explicitly pops instruction pointer matrices

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
