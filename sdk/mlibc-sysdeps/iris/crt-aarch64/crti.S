/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/crt-aarch64/crti.S
 * Purpose: aarch64 CRT init/fini preamble.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

.ident "aarch64-iris-mlibc crti"

.section .init
.global _init
_init:
	stp x29, x30, [sp, -16]!
	mov x29, sp

.section .fini
.global _fini
_fini:
	stp x29, x30, [sp, -16]!
	mov x29, sp

.section .note.GNU-stack,"",%progbits
