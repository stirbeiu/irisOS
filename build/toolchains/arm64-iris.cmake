# CMake toolchain file for irisOS ARM64 (aarch64)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Force Homebrew LLVM binaries (Apple Clang targets Mach-O only and lacks ld.lld)
set(LLVM_PREFIX "/opt/homebrew/opt/llvm")
set(CMAKE_C_COMPILER "${LLVM_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${LLVM_PREFIX}/bin/clang++")
set(CMAKE_ASM_COMPILER "${LLVM_PREFIX}/bin/clang")
set(CMAKE_AR "${LLVM_PREFIX}/bin/llvm-ar")
set(CMAKE_RANLIB "${LLVM_PREFIX}/bin/llvm-ranlib")
set(CMAKE_OBJCOPY "${LLVM_PREFIX}/bin/llvm-objcopy")
set(CMAKE_STRIP "${LLVM_PREFIX}/bin/llvm-strip")
set(CMAKE_LINKER "/opt/homebrew/bin/ld.lld")

# Target architecture triple
set(IRIS_TARGET_TRIPLE "aarch64-unknown-elf")
set(CMAKE_C_COMPILER_TARGET "${IRIS_TARGET_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${IRIS_TARGET_TRIPLE}")
set(CMAKE_ASM_COMPILER_TARGET "${IRIS_TARGET_TRIPLE}")

# Linker flags (force LLVM lld)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Search paths for cross-compilation
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
