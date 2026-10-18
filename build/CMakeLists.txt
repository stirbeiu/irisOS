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
)

add_executable(iris_init ${IRIS_INIT_SRCS})
add_dependencies(iris_init zircon_syscall_headers)

target_include_directories(iris_init PRIVATE
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

target_link_libraries(iris_init PRIVATE zircon_static)

set_target_properties(iris_init PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
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
