; ===========================================================================
; count.asm — a FontaineOS user program that exercises LOOPS and mutable DATA.
;
; Where hello.asm proves straight-line code + one syscall works, this program
; proves the loader handles a real program shape: a loop counter kept in
; memory, a message template patched in place each iteration, and several
; syscalls in sequence. It prints three numbered lines, then exits:
;
;     count.bin: line 1 of 3
;     count.bin: line 2 of 3
;     count.bin: line 3 of 3
;
; Self-modifying-DATA note: 'digit' below is a byte inside the message that we
; overwrite each pass. That works because the kernel copies the program into
; ordinary writable RAM at 0x180000 (there is no read-only text mapping for
; user programs — the whole low-4MB is mapped RW for this milestone).
;
; Same ABI as hello.asm (see its header comment / include/syscall.h):
; flat binary, org 0x180000, int 0x80 with eax=nr ebx/ecx/edx=args, must
; finish with SYS_EXIT.
;
; Build:  nasm -f bin user/count.asm -o user/count.bin      (make userprogs)
; ===========================================================================

[bits 32]
org 0x180000                    ; must match USER_PROG_LOAD_ADDR

_start:
    mov esi, 1                  ; esi = current line number (1..3)

.next_line:
    ; Patch the ASCII digit into the message template: '0' + line number.
    mov eax, esi
    add al, '0'
    mov [digit], al

    mov eax, 1                  ; SYS_WRITE
    mov ebx, msg
    mov ecx, 0x0E               ; yellow on black — distinct from hello's cyan
    int 0x80

    inc esi
    cmp esi, 3
    jbe .next_line              ; loop until all 3 lines are printed

    mov eax, 4                  ; SYS_EXIT
    xor ebx, ebx
    int 0x80

.hang:
    jmp .hang                   ; unreachable safety net (see hello.asm)

msg:   db "count.bin: line "
digit: db "?"                   ; patched to '1'/'2'/'3' each iteration
       db " of 3", 0
