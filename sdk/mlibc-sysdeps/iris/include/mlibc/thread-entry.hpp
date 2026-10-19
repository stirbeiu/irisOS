/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/include/mlibc/thread-entry.hpp
 * Purpose: mlibc thread entry declarations for irisOS.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps header.
 */

#pragma once

#include <mlibc/tcb.hpp>

extern "C" void __mlibc_start_thread(void);
extern "C" void __mlibc_enter_thread(void *entry, void *user_arg, Tcb *tcb);

namespace mlibc {
	void *prepare_stack(void *entry, void *user_arg, void *tcb);
}
