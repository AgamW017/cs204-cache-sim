.intel_syntax noprefix

# Generalized x86-64 memory access workload for Pin tool testing.
# Phases included:
# 1) Forward unit-stride access
# 2) Reverse unit-stride access
# 3) Hot-address reuse
# 4) Interleaved accesses across two arrays
# 5) Pseudo-random indexed accesses
# 6) Stack-resident accesses

.section .bss
.align 64
arrA:
    .zero 4096
.align 64
arrB:
    .zero 4096

.section .data
.align 64
hot0:
    .quad 0

.section .text
.globl main
.type main, @function
main:
    push rbp
    mov rbp, rsp
    sub rsp, 32

    # Phase 1: forward stride-8 read/write over arrA[0..127]
    lea r8, [rip + arrA]
    xor rcx, rcx
.Lfwd:
    mov rax, rcx
    shl rax, 3
    mov rdx, qword ptr [r8 + rax]
    add rdx, rcx
    mov qword ptr [r8 + rax], rdx
    inc rcx
    cmp rcx, 128
    jl .Lfwd

    # Phase 2: reverse stride-8 read/write over arrA[127..0]
    mov rcx, 127
.Lrev:
    mov rax, rcx
    shl rax, 3
    mov rdx, qword ptr [r8 + rax]
    add rdx, 1
    mov qword ptr [r8 + rax], rdx
    dec rcx
    jns .Lrev

    # Phase 3: repeatedly touch one hot address
    lea r9, [rip + hot0]
    mov rcx, 256
.Lhot:
    mov rax, qword ptr [r9]
    add rax, 3
    mov qword ptr [r9], rax
    dec rcx
    jne .Lhot

    # Phase 4: interleave accesses between arrA and arrB
    lea r10, [rip + arrA]
    lea r11, [rip + arrB]
    xor rcx, rcx
.Linterleave:
    mov rax, qword ptr [r10 + rcx*8]
    add rax, qword ptr [r11 + rcx*8]
    mov qword ptr [r10 + rcx*8], rax
    mov qword ptr [r11 + rcx*8], rax
    inc rcx
    cmp rcx, 64
    jl .Linterleave

    # Phase 5: pseudo-random index into arrB using an LCG
    mov eax, 1
    mov ecx, 256
    lea r8, [rip + arrB]
.Lrand:
    imul eax, eax, 1103515245
    add eax, 12345
    mov edx, eax
    shr edx, 16
    and edx, 127
    movsxd rdx, edx
    mov rax, qword ptr [r8 + rdx*8]
    xor rax, rdx
    mov qword ptr [r8 + rdx*8], rax
    dec ecx
    jne .Lrand

    # Phase 6: stack-based repeated accesses
    lea r8, [rbp - 32]
    mov qword ptr [r8], 0
    mov qword ptr [r8 + 8], 1
    mov ecx, 64
.Lstack:
    mov rax, qword ptr [r8]
    add rax, qword ptr [r8 + 8]
    mov qword ptr [r8], rax
    dec ecx
    jne .Lstack

    xor eax, eax
    leave
    ret
.size main, .-main

.section .note.GNU-stack,"",@progbits
