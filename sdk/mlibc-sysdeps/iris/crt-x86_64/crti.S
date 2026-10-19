/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/crt-x86_64/crti.S
 * Purpose: x86_64 CRT init/fini preamble.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

.ident "x86_64-iris-mlibc crti"

.section .init
.global _init
.type _init,@function
_init:
	push %rax

.section .fini
.global _fini
.type _fini,@function
_fini:
	push %rax

.section .note.GNU-stack,"",%progbits
