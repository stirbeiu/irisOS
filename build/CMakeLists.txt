cmake_minimum_required(VERSION 3.20)
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
set(TARGET_SUPPORTS_SHARED_LIBS TRUE)
project(irisOS_Kernel LANGUAGES C CXX ASM)

# Root directory paths
set(IRIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ZIRCON_ROOT "${IRIS_ROOT}/kernel/zircon")
set(HOST_TOOLS_DIR "${IRIS_ROOT}/out/host")
set(BUILD_DIR "${IRIS_ROOT}/build")
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")

# ==============================================================================
# Host Formatter: mkufs2 (UFS2 + AES-256-XTS Formatter)
# ==============================================================================
if (NOT CMAKE_CROSSCOMPILING)
    message(STATUS "Configuring irisOS Host Formatter (mkufs2)...")

    set(LIBUFS_ROOT "${IRIS_ROOT}/vendor/libufs")
    if (NOT EXISTS "${LIBUFS_ROOT}/libufs/sblock.c")
        execute_process(COMMAND bash "${IRIS_ROOT}/scripts/fetch_ufs.sh")
    endif()

    set(MBEDTLS_ROOT "${IRIS_ROOT}/vendor/mbedtls")
    if (NOT EXISTS "${MBEDTLS_ROOT}/library/aes.c")
        execute_process(COMMAND bash "${IRIS_ROOT}/scripts/fetch_mbedtls.sh")
    endif()

    set(MKUFS2_SRCS
        "${BUILD_DIR}/mkufs2_host.c"
        "${LIBUFS_ROOT}/libufs/block.c"
        "${LIBUFS_ROOT}/libufs/sblock.c"
        "${LIBUFS_ROOT}/libufs/cgroup.c"
        "${LIBUFS_ROOT}/libufs/inode.c"
        "${LIBUFS_ROOT}/sys/ufs/ffs/ffs_subr.c"
        "${LIBUFS_ROOT}/sys/ufs/ffs/ffs_tables.c"
        "${MBEDTLS_ROOT}/library/aes.c"
        "${MBEDTLS_ROOT}/library/sha256.c"
        "${MBEDTLS_ROOT}/library/sha512.c"
        "${MBEDTLS_ROOT}/library/platform_util.c"
        "${MBEDTLS_ROOT}/library/constant_time.c"
        "${IRIS_ROOT}/servers/iris_crypt/src/argon2.c"
    )

    add_executable(mkufs2 ${MKUFS2_SRCS})

    target_include_directories(mkufs2 PRIVATE
        "${IRIS_ROOT}/servers/iris_fshost/include/compat"
        "${LIBUFS_ROOT}/libufs"
        "${LIBUFS_ROOT}/sys"
        "${MBEDTLS_ROOT}/include"
        "${MBEDTLS_ROOT}/library"
        "${BUILD_DIR}"
        "${IRIS_ROOT}/servers/iris_crypt/src"
        "${IRIS_ROOT}"
    )

    target_compile_definitions(mkufs2 PRIVATE
        _LIBUFS
        MBEDTLS_CONFIG_FILE="mbedtls_iris_config.h"
    )

    target_compile_options(mkufs2 PRIVATE
        -O2
        -Wno-sign-compare
        -Wno-pointer-sign
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-format
    )

    set_target_properties(mkufs2 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${HOST_TOOLS_DIR}"
    )

    return()
endif()

# When cross-compiling, provide a custom target to build host mkufs2 via host CMake
add_custom_target(mkufs2
    COMMAND ${CMAKE_COMMAND} -B "${HOST_TOOLS_DIR}" -S "${CMAKE_CURRENT_SOURCE_DIR}" -G Ninja
    COMMAND ${CMAKE_COMMAND} --build "${HOST_TOOLS_DIR}" --target mkufs2
    COMMENT "Building host mkufs2 via host CMake"
)

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
set(IRIS_SYSTEM_NODE "irisOS-dev" CACHE STRING "System node name / hostname for irisOS")

set(IRIS_INIT_SRCS
    "${IRIS_ROOT}/servers/iris_init/src/main.c"
    "${IRIS_ROOT}/servers/drivers/pci/pci.c"
)

add_executable(iris_init ${IRIS_INIT_SRCS})
add_dependencies(iris_init zircon_syscall_headers)

target_compile_definitions(iris_init PRIVATE
    IRIS_SYSTEM_NODE="${IRIS_SYSTEM_NODE}"
)

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
# Target: compiler-rt Builtins
# ==============================================================================
set(COMPILER_RT_DIR "${IRIS_ROOT}/vendor/compiler-rt/lib/builtins")
set(BUILTINS_LIB "${CMAKE_BINARY_DIR}/lib/libclang_rt.builtins-${CMAKE_SYSTEM_PROCESSOR}.a")

add_custom_command(
    OUTPUT "${BUILTINS_LIB}"
    COMMAND bash "${IRIS_ROOT}/scripts/build_builtins.sh" "${CMAKE_SYSTEM_PROCESSOR}" "${CMAKE_BINARY_DIR}" "${CMAKE_C_COMPILER}" "${CMAKE_AR}"
    COMMENT "Building compiler-rt builtins (${CMAKE_SYSTEM_PROCESSOR})"
    VERBATIM
)

add_custom_target(compiler_rt_target DEPENDS "${BUILTINS_LIB}")

add_library(compiler_rt_builtins STATIC IMPORTED GLOBAL)
set_target_properties(compiler_rt_builtins PROPERTIES IMPORTED_LOCATION "${BUILTINS_LIB}")
add_dependencies(compiler_rt_builtins compiler_rt_target)

# ==============================================================================
# Target: mlibc C Library & CRT Objects
# ==============================================================================
set(MLIBC_DIR "${CMAKE_BINARY_DIR}/mlibc")
set(MLIBC_LIBC "${MLIBC_DIR}/libc.a")
set(CRT1_OBJ "${MLIBC_DIR}/sysdeps/iris/crt1.o")
set(CRTI_OBJ "${MLIBC_DIR}/sysdeps/iris/crti.o")
set(CRTN_OBJ "${MLIBC_DIR}/sysdeps/iris/crtn.o")

add_custom_command(
    OUTPUT "${MLIBC_LIBC}" "${CRT1_OBJ}" "${CRTI_OBJ}" "${CRTN_OBJ}"
    COMMAND bash "${IRIS_ROOT}/scripts/build_mlibc.sh" "${CMAKE_SYSTEM_PROCESSOR}" "${CMAKE_BINARY_DIR}"
    DEPENDS zircon_static iris_static compiler_rt_target
    COMMENT "Building mlibc C library and CRT objects (${CMAKE_SYSTEM_PROCESSOR})"
    VERBATIM
)

add_custom_target(mlibc_target DEPENDS "${MLIBC_LIBC}" "${CRT1_OBJ}" "${CRTI_OBJ}" "${CRTN_OBJ}")

add_library(mlibc STATIC IMPORTED GLOBAL)
set_target_properties(mlibc PROPERTIES IMPORTED_LOCATION "${MLIBC_LIBC}")
add_dependencies(mlibc mlibc_target)

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

    add_library(mbedcrypto STATIC ${MBEDCRYPTO_SRCS})
    add_dependencies(mbedcrypto mlibc_target)

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

    add_library(ufs STATIC ${LIBUFS_SRCS})
    add_dependencies(ufs zircon_syscall_headers mlibc_target)

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
# Shared Userland Interface & Function
# ==============================================================================
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_library(iris_userland_common INTERFACE)
target_include_directories(iris_userland_common SYSTEM INTERFACE
    "${CMAKE_BINARY_DIR}/mlibc"
    "${IRIS_ROOT}/sdk/mlibc-sysdeps/iris/include"
    "${IRIS_ROOT}/sdk/libiris/include"
    "${IRIS_ROOT}"
    "${IRIS_ROOT}/kernel/zircon/system/public"
    "${GEN_DIR}"
    "${GEN_DIR}/zircon"
    "${IRIS_ROOT}/vendor/mlibc/options/ansi/include"
    "${IRIS_ROOT}/vendor/mlibc/options/posix/include"
    "${IRIS_ROOT}/vendor/mlibc/options/bsd/include"
    "${IRIS_ROOT}/vendor/mlibc/options/glibc/include"
    "${IRIS_ROOT}/vendor/mlibc/options/internal/include"
    "${IRIS_ROOT}/vendor/mlibc/include"
)

target_compile_options(iris_userland_common INTERFACE
    -O2
    -fPIE
    -nostdinc
    -isystem "${CLANG_RESOURCE_DIR}/include"
)

function(iris_add_userland_executable TARGET_NAME)
    cmake_parse_arguments(USERLAND "" "" "SOURCES;LIBRARIES;INCLUDES;DEFINITIONS;OPTIONS" ${ARGN})

    add_executable(${TARGET_NAME} ${USERLAND_SOURCES})
    add_dependencies(${TARGET_NAME} mlibc_target)

    target_link_libraries(${TARGET_NAME} PRIVATE iris_userland_common)

    target_link_options(${TARGET_NAME} PRIVATE
        -nostdlib
        -fuse-ld=${CMAKE_LINKER}
        -Wl,-pie
        -Wl,-z,text
        "${CRT1_OBJ}"
        "${CRTI_OBJ}"
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        ${USERLAND_LIBRARIES}
        mlibc
        iris_static
        compiler_rt_builtins
        "${CRTN_OBJ}"
    )

    if (USERLAND_INCLUDES)
        target_include_directories(${TARGET_NAME} SYSTEM PRIVATE ${USERLAND_INCLUDES})
    endif()

    if (USERLAND_DEFINITIONS)
        target_compile_definitions(${TARGET_NAME} PRIVATE ${USERLAND_DEFINITIONS})
    endif()

    if (USERLAND_OPTIONS)
        target_compile_options(${TARGET_NAME} PRIVATE ${USERLAND_OPTIONS})
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

# ==============================================================================
# Userland Targets (Test Suite, Drivers & Servers)
# ==============================================================================

# 1. Interactive Test Suite
iris_add_userland_executable(its_irisOS
    SOURCES "${IRIS_ROOT}/apps/tests/its_irisOS/main.c"
)

# 2. VirtIO Block Driver Daemon
iris_add_userland_executable(virtio_blk
    SOURCES "${IRIS_ROOT}/servers/drivers/virtio_blk/src/main.c"
)

# 3. Block Encryption Proxy Daemon
iris_add_userland_executable(iris_crypt
    SOURCES "${IRIS_ROOT}/servers/iris_crypt/src/main.c"
    LIBRARIES mbedcrypto
    INCLUDES
        "${IRIS_ROOT}/vendor/mbedtls/include"
        "${IRIS_ROOT}/build"
    DEFINITIONS
        MBEDTLS_CONFIG_FILE="mbedtls_iris_config.h"
)

# 4. Central VFS Router & UFS2 Filesystem Daemon
iris_add_userland_executable(iris_fshost
    SOURCES "${IRIS_ROOT}/servers/iris_fshost/src/main.c"
    LIBRARIES ufs
    INCLUDES
        "${IRIS_ROOT}/servers/iris_fshost/include/compat"
        "${IRIS_ROOT}/servers/iris_fshost/include"
        "${IRIS_ROOT}/vendor/libufs/libufs"
        "${IRIS_ROOT}/vendor/libufs/sys"
    DEFINITIONS
        _LIBUFS
    OPTIONS
        -Wno-sign-compare
        -Wno-pointer-sign
        -Wno-unused-parameter
)

# 5. Pseudo-Terminal Server Daemon
iris_add_userland_executable(iris_pty
    SOURCES
        "${IRIS_ROOT}/servers/iris_pty/src/main.c"
        "${IRIS_ROOT}/servers/drivers/virtio_input/src/virtio_input.c"
    INCLUDES
        "${IRIS_ROOT}/servers/iris_pty/include"
        "${IRIS_ROOT}/servers/drivers/virtio_input/include"
)

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
