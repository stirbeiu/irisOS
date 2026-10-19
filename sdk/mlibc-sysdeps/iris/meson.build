sysdep_supported_options = {
	'posix': true,
	'linux': false,
	'glibc': true,
	'bsd': true,
}

zircon_dep = declare_dependency(link_args: ['-l:libzircon.a'])
libc_deps += [zircon_dep]
rtld_deps += [zircon_dep]

builtins_name = 'libclang_rt.builtins-' + host_machine.cpu_family() + '.a'
builtins_dep = declare_dependency(link_args: ['-l:' + builtins_name])
libc_deps += [builtins_dep]
rtld_deps += [builtins_dep]

rtld_sources += files(
	'generic/generic.cpp',
)

libc_sources += files(
	'generic/entry.cpp',
	'generic/generic.cpp',
	'generic/thread.cpp',
	'generic/thread_entry_' + host_machine.cpu_family() + '.S',
)

if not no_headers
	install_headers(
		'include/abi-bits/access.h',
		'include/abi-bits/auxv.h',
		'include/abi-bits/blkcnt_t.h',
		'include/abi-bits/blksize_t.h',
		'include/abi-bits/clockid_t.h',
		'include/abi-bits/dev_t.h',
		'include/abi-bits/epoll.h',
		'include/abi-bits/errno.h',
		'include/abi-bits/fcntl.h',
		'include/abi-bits/fsblkcnt_t.h',
		'include/abi-bits/fsfilcnt_t.h',
		'include/abi-bits/gid_t.h',
		'include/abi-bits/in.h',
		'include/abi-bits/ino_t.h',
		'include/abi-bits/inotify.h',
		'include/abi-bits/ioctls.h',
		'include/abi-bits/ipc.h',
		'include/abi-bits/limits.h',
		'include/abi-bits/mode_t.h',
		'include/abi-bits/mqueue.h',
		'include/abi-bits/msg.h',
		'include/abi-bits/nlink_t.h',
		'include/abi-bits/packet.h',
		'include/abi-bits/pid_t.h',
		'include/abi-bits/poll.h',
		'include/abi-bits/ptrace.h',
		'include/abi-bits/random.h',
		'include/abi-bits/reboot.h',
		'include/abi-bits/resource.h',
		'include/abi-bits/riscv-hwprobe.h',
		'include/abi-bits/rlim_t.h',
		'include/abi-bits/seek-whence.h',
		'include/abi-bits/shm.h',
		'include/abi-bits/sigevent.h',
		'include/abi-bits/signal.h',
		'include/abi-bits/sigval.h',
		'include/abi-bits/socket.h',
		'include/abi-bits/socklen_t.h',
		'include/abi-bits/stat.h',
		'include/abi-bits/statfs.h',
		'include/abi-bits/statvfs.h',
		'include/abi-bits/statx.h',
		'include/abi-bits/suseconds_t.h',
		'include/abi-bits/termios.h',
		'include/abi-bits/time.h',
		'include/abi-bits/uid_t.h',
		'include/abi-bits/utmp-defines.h',
		'include/abi-bits/utmpx.h',
		'include/abi-bits/utsname.h',
		'include/abi-bits/vm-flags.h',
		'include/abi-bits/vt.h',
		'include/abi-bits/wait.h',
		'include/abi-bits/xattr.h',
		subdir: 'abi-bits',
		follow_symlinks: true
	)
endif

if not headers_only
	crt_target = host_machine.cpu_family() == 'aarch64' ? 'aarch64-unknown-linux-gnu' : 'x86_64-unknown-linux-gnu'
	crt_cmd = c_compiler.cmd_array() + ['--target=' + crt_target, '-ffreestanding', '-c', '-o', '@OUTPUT@', '@INPUT@']

	crt = custom_target('crt1',
		build_by_default: true,
		command: crt_cmd,
		input: 'crt-' + host_machine.cpu_family() / 'crt1.S',
		output: 'crt1.o',
		install: true,
		install_dir: get_option('libdir')
	)

	custom_target('crti',
		build_by_default: true,
		command: crt_cmd,
		input: 'crt-' + host_machine.cpu_family() / 'crti.S',
		output: 'crti.o',
		install: true,
		install_dir: get_option('libdir')
	)

	custom_target('crtn',
		build_by_default: true,
		command: crt_cmd,
		input: 'crt-' + host_machine.cpu_family() / 'crtn.S',
		output: 'crtn.o',
		install: true,
		install_dir: get_option('libdir')
	)
endif
