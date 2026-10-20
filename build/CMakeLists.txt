cmake_minimum_required(VERSION 3.20)
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
set(TARGET_SUPPORTS_SHARED_LIBS TRUE)
project(irisOS_Kernel LANGUAGES C CXX ASM)

# Root directory paths
set(IRIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ZIRCON_ROOT "${IRIS_ROOT}/kernel/zircon")
set(HOST_TOOLS_DIR "${IRIS_ROOT}/out/host")
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")

# Detect architecture
if (NOT CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR "x86_64")
endif()

message(STATUS "Configuring irisOS for target architecture: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Target triple: ${CMAKE_C_COMPILER_TARGET}")

# ==============================================================================
# Host Tools & Syscall Generation
# ==============================================================================
set(ABIGEN_TOOL "${HOST_TOOLS_DIR}/abigen")
set(SYSCALLS_ABIGEN "${ZIRCON_ROOT}/system/public/zircon/syscalls.abigen")

set(GEN_SYSCALL_FILES
    "${GEN_DIR}/zircon/syscall-category.inc"
    "${GEN_DIR}/zircon/syscall-definitions.h"
    "${GEN_DIR}/zircon/syscall-kernel-branches.S"
    "${GEN_DIR}/zircon/syscall-kernel-wrappers.inc"
    "${GEN_DIR}/zircon/syscall-ktrace-info.inc"
    "${GEN_DIR}/zircon/syscall-vdso-definitions.h"
    "${GEN_DIR}/zircon/syscall-vdso-wrappers.inc"
    "${GEN_DIR}/zircon/syscalls-arm64.S"
    "${GEN_DIR}/zircon/syscalls-x86-64.S"
    "${GEN_DIR}/zircon/zx-syscall-numbers.h"
    "${GEN_DIR}/zircon/syscalls/definitions.h"
)

add_custom_command(
    OUTPUT ${GEN_SYSCALL_FILES}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}/zircon/syscalls"
    COMMAND "${ABIGEN_TOOL}"
        -trace "${GEN_DIR}/zircon/syscall-ktrace-info.inc"
        -category "${GEN_DIR}/zircon/syscall-category.inc"
        -kernel-header "${GEN_DIR}/zircon/syscall-definitions.h"
        -kernel-wrappers "${GEN_DIR}/zircon/syscall-kernel-wrappers.inc"
        -kernel-branch "${GEN_DIR}/zircon/syscall-kernel-branches.S"
        -arm-asm "${GEN_DIR}/zircon/syscalls-arm64.S"
        -x86-asm "${GEN_DIR}/zircon/syscalls-x86-64.S"
        -vdso-header "${GEN_DIR}/zircon/syscall-vdso-definitions.h"
        -vdso-wrappers "${GEN_DIR}/zircon/syscall-vdso-wrappers.inc"
        -numbers "${GEN_DIR}/zircon/zx-syscall-numbers.h"
        -user-header "${GEN_DIR}/zircon/syscalls/definitions.h"
        "${SYSCALLS_ABIGEN}"
    DEPENDS "${SYSCALLS_ABIGEN}"
    COMMENT "Generating Zircon syscall definitions and stubs from syscalls.abigen"
    VERBATIM
)

add_custom_target(zircon_syscall_headers DEPENDS ${GEN_SYSCALL_FILES})

# ==============================================================================
# Target: libzircon.so (vDSO)
# ==============================================================================
set(VDSO_SRCS
    "${ZIRCON_ROOT}/system/ulib/zircon/data.S"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_cache_flush.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_channel_call.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_cprng_draw.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_deadline_after.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_status_get_string.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_system_get_dcache_line_size.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_system_get_features.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_system_get_num_cpus.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_system_get_physmem.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_system_get_version.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_ticks_get.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/zx_ticks_per_second.cpp"
    "${ZIRCON_ROOT}/system/ulib/zircon/syscall-wrappers.cpp"
)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    list(APPEND VDSO_SRCS
        "${ZIRCON_ROOT}/system/ulib/zircon/syscalls-x86-64.S"
        "${ZIRCON_ROOT}/system/ulib/zircon/zx_futex_wake_handle_close_thread_exit-x86-64.S"
        "${ZIRCON_ROOT}/system/ulib/zircon/zx_vmar_unmap_handle_close_thread_exit-x86-64.S"
    )
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    list(APPEND VDSO_SRCS
        "${ZIRCON_ROOT}/system/ulib/zircon/syscalls-arm64.S"
        "${ZIRCON_ROOT}/system/ulib/zircon/zx_futex_wake_handle_close_thread_exit-arm64.S"
        "${ZIRCON_ROOT}/system/ulib/zircon/zx_vmar_unmap_handle_close_thread_exit-arm64.S"
    )
endif()

# 1. Static archive (libzircon.a) for freestanding early boot binaries (iris_init)
add_library(zircon_static STATIC ${VDSO_SRCS})
add_dependencies(zircon_static zircon_syscall_headers)

target_include_directories(zircon_static PRIVATE
    "${GEN_DIR}"
    "${GEN_DIR}/zircon"
    "${ZIRCON_ROOT}/system/public"
    "${ZIRCON_ROOT}/system/ulib/zircon"
    "${ZIRCON_ROOT}/system/ulib/zircon/include"
    "${ZIRCON_ROOT}/kernel/lib/vdso/include"
)

target_compile_options(zircon_static PRIVATE
    -ffreestanding
    -nostdinc++
    -fPIC
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility=hidden
)

set_target_properties(zircon_static PROPERTIES
    OUTPUT_NAME "zircon"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# 2. Dynamic shared library (libzircon.so) for user-space vDSO
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/lib/libzircon.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/lib"
    COMMAND "${CMAKE_CXX_COMPILER}"
        "--target=${CMAKE_CXX_COMPILER_TARGET}"
        "-Wl,-shared"
        "-nostdlib"
        "-fuse-ld=${CMAKE_LINKER}"
        "-Wl,-soname,libzircon.so"
        $<TARGET_OBJECTS:zircon_static>
        "-o" "${CMAKE_BINARY_DIR}/lib/libzircon.so"
    DEPENDS zircon_static
    COMMENT "Linking dynamic vDSO shared library (libzircon.so)"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_custom_target(zircon_shared ALL DEPENDS "${CMAKE_BINARY_DIR}/lib/libzircon.so")

# ==============================================================================
# Target: iris_init (Freestanding Bootstrap Server)
# ==============================================================================
set(IRIS_INIT_SRCS
    "${IRIS_ROOT}/servers/iris_init/src/main.c"
    "${IRIS_ROOT}/servers/drivers/pci/pci.c"
)

add_executable(iris_init ${IRIS_INIT_SRCS})
add_dependencies(iris_init zircon_syscall_headers)

target_include_directories(iris_init PRIVATE
    "${IRIS_ROOT}"
    "${IRIS_ROOT}/sdk/libiris/include"
    "${GEN_DIR}"
    "${GEN_DIR}/zircon"
    "${ZIRCON_ROOT}/system/public"
)

target_compile_options(iris_init PRIVATE
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fPIE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_options(iris_init PRIVATE
    -nostdlib
    -fuse-ld=${CMAKE_LINKER}
    -Wl,-pie
    -Wl,-e,_start
    -Wl,--gc-sections
    -Wl,-z,nocopyreloc
)

# Note: iris_init does not link zircon_static because Zircon microkernel
# requires all syscall instructions to execute from within the mapped vDSO.
# iris_init resolves its syscall entry points dynamically from vdso_base.

set_target_properties(iris_init PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ==============================================================================
# Target: libiris (irisOS Native SDK - Zone 1 Permissive MIT)
# ==============================================================================
set(LIBIRIS_SRCS
    "${IRIS_ROOT}/sdk/libiris/src/channel.c"
    "${IRIS_ROOT}/sdk/libiris/src/vmo.c"
    "${IRIS_ROOT}/sdk/libiris/src/spawn.c"
    "${IRIS_ROOT}/sdk/libiris/src/init.c"
    "${IRIS_ROOT}/sdk/libiris/src/pci.c"
    "${IRIS_ROOT}/sdk/libiris/src/pty.c"
)

# 1. Static library (libiris.a)
add_library(iris_static STATIC ${LIBIRIS_SRCS})
add_dependencies(iris_static zircon_syscall_headers)

target_include_directories(iris_static PUBLIC
    "${IRIS_ROOT}/sdk/libiris/include"
    "${IRIS_ROOT}"
    "${GEN_DIR}"
    "${GEN_DIR}/zircon"
    "${ZIRCON_ROOT}/system/public"
)

target_compile_options(iris_static PRIVATE
    -ffreestanding
    -fPIC
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

set_target_properties(iris_static PROPERTIES
    OUTPUT_NAME "iris"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# 2. Dynamic shared library (libiris.so)
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/lib/libiris.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/lib"
    COMMAND "${CMAKE_C_COMPILER}"
        "--target=${CMAKE_C_COMPILER_TARGET}"
        "-Wl,-shared"
        "-nostdlib"
        "-fuse-ld=${CMAKE_LINKER}"
        "-Wl,-soname,libiris.so"
        "-Wl,--allow-shlib-undefined"
        $<TARGET_OBJECTS:iris_static>
        "-L${CMAKE_BINARY_DIR}/lib"
        "-Wl,-Bdynamic"
        "-lzircon"
        "-o" "${CMAKE_BINARY_DIR}/lib/libiris.so"
    DEPENDS iris_static zircon_shared
    COMMENT "Linking dynamic shared library (libiris.so)"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_custom_target(iris_shared ALL DEPENDS "${CMAKE_BINARY_DIR}/lib/libiris.so")

# ==============================================================================
# Target: test_libiris (Standalone Sanity / Verification Test)
# ==============================================================================
set(TEST_LIBIRIS_SRCS
    "${IRIS_ROOT}/sdk/libiris/tests/test_libiris.c"
)

add_executable(test_libiris ${TEST_LIBIRIS_SRCS})
add_dependencies(test_libiris zircon_syscall_headers iris_static zircon_static)

target_include_directories(test_libiris PRIVATE
    "${IRIS_ROOT}/sdk/libiris/include"
    "${GEN_DIR}"
    "${GEN_DIR}/zircon"
    "${ZIRCON_ROOT}/system/public"
)

target_compile_options(test_libiris PRIVATE
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fPIE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_libraries(test_libiris PRIVATE
    iris_static
    zircon_static
)

target_link_options(test_libiris PRIVATE
    -nostdlib
    -fuse-ld=${CMAKE_LINKER}
    -Wl,-pie
    -Wl,-e,_start
    -Wl,--gc-sections
    -Wl,-z,nocopyreloc
)

set_target_properties(test_libiris PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ==============================================================================
# Target: mbedcrypto (Minimal Freestanding Mbed-TLS 3.6 + Argon2id - Apache 2.0)
# ==============================================================================
set(MBEDTLS_ROOT "${IRIS_ROOT}/vendor/mbedtls")
if (EXISTS "${MBEDTLS_ROOT}/library/aes.c")
    set(MBEDCRYPTO_SRCS
        "${MBEDTLS_ROOT}/library/aes.c"
        "${MBEDTLS_ROOT}/library/cipher.c"
        "${MBEDTLS_ROOT}/library/cipher_wrap.c"
        "${MBEDTLS_ROOT}/library/constant_time.c"
        "${MBEDTLS_ROOT}/library/error.c"
        "${MBEDTLS_ROOT}/library/hkdf.c"
        "${MBEDTLS_ROOT}/library/md.c"
        "${MBEDTLS_ROOT}/library/pkcs5.c"
        "${MBEDTLS_ROOT}/library/platform.c"
        "${MBEDTLS_ROOT}/library/platform_util.c"
        "${MBEDTLS_ROOT}/library/sha256.c"
        "${MBEDTLS_ROOT}/library/sha512.c"
        "${IRIS_ROOT}/servers/iris_crypt/src/argon2.c"
    )

    add_library(mbedcrypto STATIC EXCLUDE_FROM_ALL ${MBEDCRYPTO_SRCS})

    target_include_directories(mbedcrypto PUBLIC
        "${MBEDTLS_ROOT}/include"
        "${MBEDTLS_ROOT}/library"
        "${IRIS_ROOT}/build"
        "${IRIS_ROOT}"
        "${CMAKE_BINARY_DIR}/mlibc"
        "${IRIS_ROOT}/sdk/mlibc-sysdeps/iris/include"
        "${IRIS_ROOT}/vendor/mlibc/options/ansi/include"
        "${IRIS_ROOT}/vendor/mlibc/options/posix/include"
        "${IRIS_ROOT}/vendor/mlibc/options/bsd/include"
        "${IRIS_ROOT}/vendor/mlibc/options/glibc/include"
        "${IRIS_ROOT}/vendor/mlibc/options/internal/include"
        "${IRIS_ROOT}/vendor/mlibc/include"
    )

    target_compile_definitions(mbedcrypto PUBLIC
        MBEDTLS_CONFIG_FILE="mbedtls_iris_config.h"
    )

    target_compile_options(mbedcrypto PRIVATE
        -ffreestanding
        -fPIC
        -O2
        -Wno-unused-parameter
        -Wno-unused-function
    )

    set_target_properties(mbedcrypto PROPERTIES
        OUTPUT_NAME "mbedcrypto"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

# ==============================================================================
# Target: libufs (FreeBSD UFS2 / FFS Filesystem Library - BSD 2-Clause)
# ==============================================================================
set(LIBUFS_ROOT "${IRIS_ROOT}/vendor/libufs")
if (EXISTS "${LIBUFS_ROOT}/libufs/sblock.c")
    set(LIBUFS_SRCS
        "${LIBUFS_ROOT}/libufs/block.c"
        "${LIBUFS_ROOT}/libufs/cgroup.c"
        "${LIBUFS_ROOT}/libufs/inode.c"
        "${LIBUFS_ROOT}/libufs/sblock.c"
        "${LIBUFS_ROOT}/sys/ufs/ffs/ffs_subr.c"
        "${LIBUFS_ROOT}/sys/ufs/ffs/ffs_tables.c"
        "${IRIS_ROOT}/servers/iris_fshost/src/ufs_io.c"
    )

    add_library(ufs STATIC EXCLUDE_FROM_ALL ${LIBUFS_SRCS})
    add_dependencies(ufs zircon_syscall_headers)

    target_include_directories(ufs PUBLIC
        "${IRIS_ROOT}/servers/iris_fshost/include/compat"
        "${IRIS_ROOT}/servers/iris_fshost/include"
        "${LIBUFS_ROOT}/libufs"
        "${LIBUFS_ROOT}/sys"
        "${IRIS_ROOT}/sdk/libiris/include"
        "${IRIS_ROOT}"
        "${GEN_DIR}"
        "${GEN_DIR}/zircon"
        "${ZIRCON_ROOT}/system/public"
        "${CMAKE_BINARY_DIR}/mlibc"
        "${IRIS_ROOT}/sdk/mlibc-sysdeps/iris/include"
        "${IRIS_ROOT}/vendor/mlibc/options/ansi/include"
        "${IRIS_ROOT}/vendor/mlibc/options/posix/include"
        "${IRIS_ROOT}/vendor/mlibc/options/bsd/include"
        "${IRIS_ROOT}/vendor/mlibc/options/glibc/include"
        "${IRIS_ROOT}/vendor/mlibc/options/internal/include"
        "${IRIS_ROOT}/vendor/mlibc/include"
    )

    target_compile_definitions(ufs PRIVATE
        _LIBUFS
        pread=iris_ufs_pread
        pwrite=iris_ufs_pwrite
    )

    target_compile_options(ufs PRIVATE
        -ffreestanding
        -fPIC
        -O2
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-sign-compare
        -Wno-pointer-sign
    )

    set_target_properties(ufs PROPERTIES
        OUTPUT_NAME "ufs"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

# ==============================================================================
# Target: Zircon Microkernel Core
# ==============================================================================
set(KERNEL_ROOT "${ZIRCON_ROOT}/kernel")
set(KERNEL_INCLUDES
    "${GEN_DIR}"
    "${GEN_DIR}/zircon"
    "${ZIRCON_ROOT}/system/public"
    "${KERNEL_ROOT}/include"
    "${KERNEL_ROOT}/arch/${CMAKE_SYSTEM_PROCESSOR}/include"
    "${KERNEL_ROOT}/lib/vdso/include"
    "${KERNEL_ROOT}/lib/fbl/include"
    "${KERNEL_ROOT}/lib/heap/include"
    "${KERNEL_ROOT}/lib/crypto/include"
)

message(STATUS "Kernel include paths registered: ${KERNEL_ROOT}/include")
