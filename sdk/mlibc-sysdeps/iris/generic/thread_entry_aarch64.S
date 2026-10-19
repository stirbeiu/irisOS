/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/generic/thread_entry_arm64.S
 * Purpose: ARM64 thread trampoline unpacking arguments into registers for __mlibc_enter_thread.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

.section .text
.global __mlibc_start_thread
__mlibc_start_thread:
	ldp x0, x1, [sp], #16
	ldr x2, [sp], #16
	b __mlibc_enter_thread

.section .note.GNU-stack,"",%progbits
