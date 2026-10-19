/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/crt-aarch64/crt1.S
 * Purpose: ARM64 (aarch64) C runtime startup entry point for user-space processes.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

.section .text
.global _start
.type _start, %function
_start:
    /*
     * Arguments passed by zx_process_start():
     *  x0: start_arg (bootstrap channel handle)
     *  x1: vdso_base (mapped vDSO base address)
     *  sp: stack pointer (16-byte aligned)
     */
    mov x29, #0
    mov x30, #0

    /*
     * Call __iris_start(start_arg, vdso_base, main):
     *  x0 already contains start_arg
     *  x1 already contains vdso_base
     *  x2: function pointer to main
     */
    adrp x2, main
    add x2, x2, :lo12:main
    bl __iris_start

    /* Should never return */
1:  wfe
    b 1b

.size _start, . - _start
.section .note.GNU-stack,"",%progbits
