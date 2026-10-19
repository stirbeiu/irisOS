/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/crt-aarch64/crtn.S
 * Purpose: aarch64 CRT init/fini epilogue.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

.ident "aarch64-iris-mlibc crtn"

.section .init
	ldp x29, x30, [sp], #16
	ret

.section .fini
	ldp x29, x30, [sp], #16
	ret

.section .note.GNU-stack,"",%progbits
