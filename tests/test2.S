.intel_syntax noprefix

.section .bss
.align 64
buf:
    .zero 32768

.section .text
.globl main
.type main, @function
main:
    push rbp
    mov rbp, rsp

    lea r8, [rip + buf]

    # Phase 1: warm-up sequential working set (fits in cache)
    xor rcx, rcx
.Lwarm_write:
    mov qword ptr [r8 + rcx*8], rcx
    inc rcx
    cmp rcx, 512
    jl .Lwarm_write

    xor rcx, rcx
    xor rax, rax
.Lwarm_read:
    add rax, qword ptr [r8 + rcx*8]
    inc rcx
    cmp rcx, 512
    jl .Lwarm_read

    # Phase 2: conflict phase that exposes replacement policy behavior
    # A = buf+0, B = buf+8192, C = buf+16384 (same set for recommended knobs)
    mov ecx, 400000
.Lconflict:
    add rax, qword ptr [r8 + 0]        # A
    add rax, qword ptr [r8 + 8192]     # B
    add rax, qword ptr [r8 + 16384]    # C
    add rax, qword ptr [r8 + 8192]     # B again (favors recency-aware policy)
    dec ecx
    jne .Lconflict

    # Phase 3: light noise with wider footprint
    xor rcx, rcx
.Lnoise:
    add rax, qword ptr [r8 + rcx*8]
    add rcx, 7
    and rcx, 1023
    cmp rcx, 0
    jne .Lnoise

    xor eax, eax
    pop rbp
    ret
.size main, .-main

.section .note.GNU-stack,"",@progbits
