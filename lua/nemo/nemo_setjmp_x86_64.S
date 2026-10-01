// setjmp/longjmp x86-64 -- SOLO para la prueba en el host.
    .text
    .globl nemo_setjmp
nemo_setjmp:
    movq %rbx, 0(%rdi); movq %rbp, 8(%rdi); movq %r12, 16(%rdi); movq %r13, 24(%rdi)
    movq %r14, 32(%rdi); movq %r15, 40(%rdi)
    leaq 8(%rsp), %rdx; movq %rdx, 48(%rdi)      # rsp tras el ret
    movq (%rsp), %rdx;  movq %rdx, 56(%rdi)      # direccion de retorno
    xorl %eax, %eax; ret
    .globl nemo_longjmp
nemo_longjmp:
    movq 0(%rdi), %rbx; movq 8(%rdi), %rbp; movq 16(%rdi), %r12; movq 24(%rdi), %r13
    movq 32(%rdi), %r14; movq 40(%rdi), %r15
    movl %esi, %eax; testl %eax, %eax; jnz 1f; incl %eax
1:  movq 48(%rdi), %rsp; jmpq *56(%rdi)
    .section .note.GNU-stack,"",@progbits
