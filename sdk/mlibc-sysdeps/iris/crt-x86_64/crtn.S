/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/crt-x86_64/crtn.S
 * Purpose: x86_64 CRT init/fini epilogue.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

.ident "x86_64-iris-mlibc crtn"

.section .init
	pop %rax
	ret

.section .fini
	pop %rax
	ret

.section .note.GNU-stack,"",%progbits
