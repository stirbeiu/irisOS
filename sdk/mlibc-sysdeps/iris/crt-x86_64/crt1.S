/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/crt-x86_64/crt1.S
 * Purpose: x86_64 C runtime startup entry point for user-space processes.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

.section .text
.global _start
.type _start, @function
_start:
    /*
     * Arguments passed by zx_process_start():
     *  %rdi: start_arg (bootstrap channel handle)
     *  %rsi: vdso_base (mapped vDSO base address)
     *  %rsp: stack pointer (16-byte aligned - 8)
     */
    push %rbp
    mov %rsp, %rbp

    /* Align stack to 16-byte boundary before call (System V AMD64 ABI) */
    and $~0xF, %rsp

    /*
     * Call __iris_start(start_arg, vdso_base, main):
     *  %rdi already contains start_arg
     *  %rsi already contains vdso_base
     *  %rdx: function pointer to main (position-independent via RIP)
     */
    lea main(%rip), %rdx
    call __iris_start

    /* Should never return */
1:  hlt
    jmp 1b

.size _start, . - _start
.section .note.GNU-stack,"",%progbits
