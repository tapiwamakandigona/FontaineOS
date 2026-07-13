; ===========================================================================
; hello.asm — the smallest possible FontaineOS user program.
;
; FontaineOS USER PROGRAM ABI (documented in include/syscall.h):
;   * Flat 32-bit binary — no ELF, no header. Byte 0 is the entry point.
;   * Assembled with 'org 0x180000' because the kernel loads every program at
;     that fixed physical==virtual address (USER_PROG_LOAD_ADDR); absolute
;     references like 'mov ebx, msg' below therefore resolve correctly with
;     no relocation step.
;   * Executes at CPL=3 (ring 3). The only doors into the kernel are int 0x80
;     and faulting; privileged instructions (cli/hlt/in/out) draw a #GP and
;     get the program killed — cleanly — by the kernel's GPF handler.
;   * Syscall convention: eax = number, ebx/ecx/edx = args, result in eax.
;       SYS_WRITE (1): ebx = NUL-terminated string, ecx = VGA color byte
;       SYS_EXIT  (4): terminate; control returns to the kernel launcher
;   * The kernel supplies a valid stack (ESP at the top of a private 4KB
;     buffer) before entry — no stack setup needed here.
;
; Build:  nasm -f bin user/hello.asm -o user/hello.bin      (make userprogs)
; Ship :  python3 tools/fontfs_inject.py bin/disk.img user/hello.bin ...
; Run  :  FontaineOS shell> run hello.bin
; ===========================================================================

[bits 32]
org 0x180000                    ; must match USER_PROG_LOAD_ADDR

_start:
    mov eax, 1                  ; SYS_WRITE
    mov ebx, msg                ; absolute address of the string (org makes it right)
    mov ecx, 0x0B               ; bright cyan on black
    int 0x80                    ; -> kernel syscall console (VGA rows 5-7)

    mov eax, 4                  ; SYS_EXIT
    xor ebx, ebx                ; exit code 0 (ignored by the kernel for now)
    int 0x80                    ; never returns — kernel_reentry resumes the launcher

    ; Defensive: if SYS_EXIT ever DID return, spin instead of executing junk.
.hang:
    jmp .hang

msg: db "hello.bin: greetings from ring 3, loaded off FontFS!", 0
