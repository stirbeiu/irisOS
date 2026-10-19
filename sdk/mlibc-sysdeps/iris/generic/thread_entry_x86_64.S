/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/generic/thread_entry_x86_64.S
 * Purpose: x86_64 thread trampoline unpacking arguments into registers for __mlibc_enter_thread.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

.section .text
.global __mlibc_start_thread
__mlibc_start_thread:
	pop %rdi
	pop %rsi
	pop %rdx
	call __mlibc_enter_thread

.section .note.GNU-stack,"",%progbits
