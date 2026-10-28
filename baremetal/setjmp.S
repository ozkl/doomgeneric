# setjmp / longjmp (Doom doesn't use them, but glibc's headers expect them).
# glibc's <setjmp.h> calls _setjmp; its jmp_buf has room for these registers
# (8 longs on x86_64, 6 ints on i386). Signal masks don't exist here.

.text
#ifdef __x86_64__

.global _setjmp, setjmp, __sigsetjmp
_setjmp:
setjmp:
__sigsetjmp:
    mov  %rbx, 0(%rdi)
    mov  %rbp, 8(%rdi)
    mov  %r12, 16(%rdi)
    mov  %r13, 24(%rdi)
    mov  %r14, 32(%rdi)
    mov  %r15, 40(%rdi)
    lea  8(%rsp), %rdx           # the caller's stack pointer after return
    mov  %rdx, 48(%rdi)
    mov  (%rsp), %rdx            # return address
    mov  %rdx, 56(%rdi)
    xor  %eax, %eax
    ret

.global longjmp, _longjmp, siglongjmp
longjmp:
_longjmp:
siglongjmp:
    mov  %esi, %eax
    test %eax, %eax
    jnz  1f
    inc  %eax                    # longjmp(env, 0) returns 1
1:  mov  0(%rdi), %rbx
    mov  8(%rdi), %rbp
    mov  16(%rdi), %r12
    mov  24(%rdi), %r13
    mov  32(%rdi), %r14
    mov  40(%rdi), %r15
    mov  48(%rdi), %rsp
    jmp  *56(%rdi)

#else

.global _setjmp, setjmp, __sigsetjmp
_setjmp:
setjmp:
__sigsetjmp:
    mov  4(%esp), %ecx           # env
    mov  %ebx, 0(%ecx)
    mov  %esi, 4(%ecx)
    mov  %edi, 8(%ecx)
    mov  %ebp, 12(%ecx)
    lea  4(%esp), %edx           # the caller's stack pointer after return
    mov  %edx, 16(%ecx)
    mov  (%esp), %edx            # return address
    mov  %edx, 20(%ecx)
    xor  %eax, %eax
    ret

.global longjmp, _longjmp, siglongjmp
longjmp:
_longjmp:
siglongjmp:
    mov  4(%esp), %ecx           # env
    mov  8(%esp), %eax           # value
    test %eax, %eax
    jnz  1f
    inc  %eax
1:  mov  0(%ecx), %ebx
    mov  4(%ecx), %esi
    mov  8(%ecx), %edi
    mov  12(%ecx), %ebp
    mov  16(%ecx), %esp
    jmp  *20(%ecx)

#endif

.section .note.GNU-stack,"",@progbits
