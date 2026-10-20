# ==============================================================================
# irisOS - Master Multi-Toolchain Build Orchestrator
# ==============================================================================

SHELL := /bin/bash
ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD_DIR := $(ROOT_DIR)/build
OUT_DIR := $(ROOT_DIR)/out
HOST_OUT := $(OUT_DIR)/host
LLVM_PREFIX := /opt/homebrew/opt/llvm
HOST_CC := $(LLVM_PREFIX)/bin/clang
HOST_CXX := $(LLVM_PREFIX)/bin/clang++

ZIRCON_DIR := $(ROOT_DIR)/kernel/zircon

CMD := $(firstword $(MAKECMDGOALS))

ifeq ($(CMD),build)
  BUILD_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  %::
	@:
.PHONY: build
build:
	@$(ROOT_DIR)/scripts/iris.sh build $(BUILD_ARGS)
else ifeq ($(CMD),run)
  RUN_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  ifneq ($(RUN_ARGS),)
    %::
	@:
.PHONY: run
run:
	@$(ROOT_DIR)/scripts/iris.sh run $(RUN_ARGS)
  endif
endif

all: image-x86_64
all-x86_64: image-x86_64
all-aarch64: image-aarch64

# ------------------------------------------------------------------------------
# Host Tools (Darwin Apple Silicon)
# ------------------------------------------------------------------------------
tools: $(HOST_OUT)/zbi $(HOST_OUT)/abigen $(HOST_OUT)/mkufs2

$(HOST_OUT)/zbi: $(ZIRCON_DIR)/system/host/zbi/zbi.cpp
	@mkdir -p $(HOST_OUT)
	@echo "==> Building host zbi utility ($(HOST_OUT)/zbi)..."
	@$(HOST_CC) -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include/lz4 \
		-c $(ZIRCON_DIR)/third_party/ulib/lz4/lz4.c -o $(HOST_OUT)/lz4.o
	@$(HOST_CC) -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include/lz4 \
		-c $(ZIRCON_DIR)/third_party/ulib/lz4/lz4frame.c -o $(HOST_OUT)/lz4frame.o
	@$(HOST_CC) -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include/lz4 \
		-c $(ZIRCON_DIR)/third_party/ulib/lz4/lz4hc.c -o $(HOST_OUT)/lz4hc.o
	@$(HOST_CC) -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include/lz4 \
		-c $(ZIRCON_DIR)/third_party/ulib/lz4/xxhash.c -o $(HOST_OUT)/xxhash.o
	@$(HOST_CC) -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/third_party/ulib/cksum/include \
		-c $(ZIRCON_DIR)/third_party/ulib/cksum/crc32.c -o $(HOST_OUT)/crc32.o
	@$(HOST_CC) -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/third_party/ulib/cksum/include \
		-c $(ZIRCON_DIR)/third_party/ulib/cksum/adler32.c -o $(HOST_OUT)/adler32.o
	@$(HOST_CXX) -std=c++17 -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/system/ulib/fbl/include \
		-c $(ZIRCON_DIR)/system/ulib/fbl/alloc_checker.cpp -o $(HOST_OUT)/alloc_checker.o
	@$(HOST_CXX) -std=c++17 -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/system/ulib/fbl/include \
		-c $(ZIRCON_DIR)/system/ulib/fbl/string.cpp -o $(HOST_OUT)/string.o
	@$(HOST_CXX) -std=c++17 -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/system/ulib/fbl/include \
		-c $(ZIRCON_DIR)/system/ulib/fbl/string_piece.cpp -o $(HOST_OUT)/string_piece.o
	@$(HOST_CXX) -std=c++17 -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/system/ulib/fbl/include \
		-c $(ZIRCON_DIR)/system/ulib/fbl/string_buffer.cpp -o $(HOST_OUT)/string_buffer.o
	@$(HOST_CXX) -std=c++17 -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/system/ulib/fbl/include \
		-c $(ZIRCON_DIR)/system/ulib/fbl/string_printf.cpp -o $(HOST_OUT)/string_printf.o
	@$(HOST_CXX) -std=c++17 -O2 \
		-I$(ZIRCON_DIR)/system/public \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include \
		-I$(ZIRCON_DIR)/third_party/ulib/lz4/include/lz4 \
		-I$(ZIRCON_DIR)/third_party/ulib/cksum/include \
		-I$(ZIRCON_DIR)/system/ulib/fbl/include \
		-c $(ZIRCON_DIR)/system/host/zbi/zbi.cpp -o $(HOST_OUT)/zbi.o
	@$(HOST_CXX) $(HOST_OUT)/*.o -o $(HOST_OUT)/zbi
	@rm -f $(HOST_OUT)/*.o
	@echo "    [DONE] $(HOST_OUT)/zbi"

$(HOST_OUT)/abigen: $(ZIRCON_DIR)/system/host/abigen/abigen.cpp
	@mkdir -p $(HOST_OUT)
	@echo "==> Building host abigen utility ($(HOST_OUT)/abigen)..."
	@$(HOST_CXX) -std=c++17 -O2 \
		-I$(ZIRCON_DIR)/system/host/abigen \
		$(ZIRCON_DIR)/system/host/abigen/*.cpp \
		$(ZIRCON_DIR)/system/host/abigen/parser/parser.cpp \
		-o $(HOST_OUT)/abigen
	@echo "    [DONE] $(HOST_OUT)/abigen"

zbi: $(HOST_OUT)/zbi
abigen: $(HOST_OUT)/abigen
mkufs2: $(HOST_OUT)/mkufs2

$(HOST_OUT)/mkufs2: $(BUILD_DIR)/mkufs2_host.c fetch-libufs fetch-mbedtls
	@mkdir -p $(HOST_OUT)
	@echo "==> Building host UFS2 + AES-256-XTS formatter ($(HOST_OUT)/mkufs2)..."
	@$(HOST_CC) -O2 -D_LIBUFS \
		-DMBEDTLS_CONFIG_FILE='"mbedtls_iris_config.h"' \
		-I$(ROOT_DIR)/servers/iris_fshost/include/compat \
		-I$(ROOT_DIR)/vendor/libufs/libufs \
		-I$(ROOT_DIR)/vendor/libufs/sys \
		-I$(ROOT_DIR)/vendor/mbedtls/include \
		-I$(ROOT_DIR)/vendor/mbedtls/library \
		-I$(BUILD_DIR) \
		-I$(ROOT_DIR)/servers/iris_crypt/src \
		-I$(ROOT_DIR) \
		$(BUILD_DIR)/mkufs2_host.c \
		$(ROOT_DIR)/vendor/libufs/libufs/block.c \
		$(ROOT_DIR)/vendor/libufs/libufs/sblock.c \
		$(ROOT_DIR)/vendor/libufs/libufs/cgroup.c \
		$(ROOT_DIR)/vendor/libufs/libufs/inode.c \
		$(ROOT_DIR)/vendor/libufs/sys/ufs/ffs/ffs_subr.c \
		$(ROOT_DIR)/vendor/libufs/sys/ufs/ffs/ffs_tables.c \
		$(ROOT_DIR)/vendor/mbedtls/library/aes.c \
		$(ROOT_DIR)/vendor/mbedtls/library/sha256.c \
		$(ROOT_DIR)/vendor/mbedtls/library/sha512.c \
		$(ROOT_DIR)/vendor/mbedtls/library/platform_util.c \
		$(ROOT_DIR)/vendor/mbedtls/library/constant_time.c \
		$(ROOT_DIR)/servers/iris_crypt/src/argon2.c \
		-Wno-sign-compare -Wno-pointer-sign -Wno-unused-parameter -Wno-unused-function -Wno-format \
		-o $(HOST_OUT)/mkufs2
	@echo "    [DONE] $(HOST_OUT)/mkufs2"

# ------------------------------------------------------------------------------
# Target Architecture Builds (CMake + Ninja + Microkernel)
# ------------------------------------------------------------------------------
kernel-x86_64: tools
	@echo "==> Building Zircon microkernel (x86_64)..."
	@$(MAKE) -C $(ZIRCON_DIR) \
		build-x64-clang/zircon.elf \
		build-x64-clang/kernel.zbi \
		PROJECT=x64 \
		USE_CLANG=true \
		USE_LLD=true \
		CLANG_TOOLCHAIN_PREFIX=$(LLVM_PREFIX)/bin/ \
		ARCH_x86_64_TOOLCHAIN_INCLUDED=1 \
		ENABLE_BUILD_SYSROOT=false
	@mkdir -p $(OUT_DIR)/build-x86_64/kernel
	@cp -f $(ZIRCON_DIR)/build-x64-clang/zircon.elf $(OUT_DIR)/build-x86_64/kernel/zircon.elf
	@cp -f $(ZIRCON_DIR)/build-x64-clang/kernel.zbi $(OUT_DIR)/build-x86_64/kernel/kernel.zbi
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/kernel/zircon.elf"

kernel-aarch64: tools
	@echo "==> Building Zircon microkernel (aarch64)..."
	@$(MAKE) -C $(ZIRCON_DIR) \
		build-arm64-clang/zircon.elf \
		build-arm64-clang/kernel.zbi \
		PROJECT=arm64 \
		USE_CLANG=true \
		USE_LLD=true \
		CLANG_TOOLCHAIN_PREFIX=$(LLVM_PREFIX)/bin/ \
		ARCH_arm64_TOOLCHAIN_INCLUDED=1 \
		ENABLE_BUILD_SYSROOT=false
	@mkdir -p $(OUT_DIR)/build-aarch64/kernel
	@cp -f $(ZIRCON_DIR)/build-arm64-clang/zircon.elf $(OUT_DIR)/build-aarch64/kernel/zircon.elf
	@cp -f $(ZIRCON_DIR)/build-arm64-clang/kernel.zbi $(OUT_DIR)/build-aarch64/kernel/kernel.zbi
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/kernel/zircon.elf"

build-x86_64: tools kernel-x86_64 fetch-mlibc fetch-mbedtls
	@mkdir -p $(OUT_DIR)/build-x86_64
	@cmake -B $(OUT_DIR)/build-x86_64 -S $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(BUILD_DIR)/toolchains/x86_64-iris.cmake \
		-G Ninja
	@ninja -C $(OUT_DIR)/build-x86_64

build-aarch64: tools kernel-aarch64 fetch-mlibc fetch-mbedtls
	@mkdir -p $(OUT_DIR)/build-aarch64
	@cmake -B $(OUT_DIR)/build-aarch64 -S $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(BUILD_DIR)/toolchains/aarch64-iris.cmake \
		-G Ninja
	@ninja -C $(OUT_DIR)/build-aarch64

# ------------------------------------------------------------------------------
# compiler-rt Builtins, mlibc C Library & Mbed-TLS Crypto
# ------------------------------------------------------------------------------
fetch-compiler-rt:
	@$(ROOT_DIR)/scripts/fetch_compiler_rt.sh

fetch-mlibc:
	@$(ROOT_DIR)/scripts/fetch_mlibc.sh

fetch-mbedtls:
	@$(ROOT_DIR)/scripts/fetch_mbedtls.sh

fetch-libufs:
	@$(ROOT_DIR)/scripts/fetch_ufs.sh

libufs-x86_64: mlibc-x86_64 fetch-libufs
	@echo "==> Building libufs.a (x86_64)..."
	@ninja -C $(OUT_DIR)/build-x86_64 ufs
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/lib/libufs.a"

libufs-aarch64: mlibc-aarch64 fetch-libufs
	@echo "==> Building libufs.a (aarch64)..."
	@ninja -C $(OUT_DIR)/build-aarch64 ufs
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/lib/libufs.a"

libufs: libufs-x86_64 libufs-aarch64

mbedtls-x86_64: mlibc-x86_64 fetch-mbedtls
	@echo "==> Building libmbedcrypto.a (x86_64)..."
	@ninja -C $(OUT_DIR)/build-x86_64 mbedcrypto
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/lib/libmbedcrypto.a"

mbedtls-aarch64: mlibc-aarch64 fetch-mbedtls
	@echo "==> Building libmbedcrypto.a (aarch64)..."
	@ninja -C $(OUT_DIR)/build-aarch64 mbedcrypto
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/lib/libmbedcrypto.a"

mbedtls: mbedtls-x86_64 mbedtls-aarch64

builtins-x86_64: fetch-compiler-rt
	@mkdir -p $(OUT_DIR)/build-x86_64/lib $(OUT_DIR)/build-x86_64/builtins
	@echo "==> Building compiler-rt builtins (x86_64)..."
	@for f in $(ROOT_DIR)/vendor/compiler-rt/lib/builtins/*.c; do \
		$(HOST_CC) -c "$$f" --target=x86_64-unknown-linux-gnu -O2 -fPIC \
			-I$(ROOT_DIR)/vendor/compiler-rt/lib/builtins \
			-o $(OUT_DIR)/build-x86_64/builtins/$$(basename "$$f" .c).o 2>/dev/null || true; \
	done
	@$(LLVM_PREFIX)/bin/llvm-ar rcs $(OUT_DIR)/build-x86_64/lib/libclang_rt.builtins-x86_64.a $(OUT_DIR)/build-x86_64/builtins/*.o
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/lib/libclang_rt.builtins-x86_64.a"

builtins-aarch64: fetch-compiler-rt
	@mkdir -p $(OUT_DIR)/build-aarch64/lib $(OUT_DIR)/build-aarch64/builtins
	@echo "==> Building compiler-rt builtins (aarch64)..."
	@for f in $(ROOT_DIR)/vendor/compiler-rt/lib/builtins/*.c; do \
		$(HOST_CC) -c "$$f" --target=aarch64-unknown-linux-gnu -O2 -fPIC \
			-I$(ROOT_DIR)/vendor/compiler-rt/lib/builtins \
			-o $(OUT_DIR)/build-aarch64/builtins/$$(basename "$$f" .c).o 2>/dev/null || true; \
	done
	@$(LLVM_PREFIX)/bin/llvm-ar rcs $(OUT_DIR)/build-aarch64/lib/libclang_rt.builtins-aarch64.a $(OUT_DIR)/build-aarch64/builtins/*.o
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/lib/libclang_rt.builtins-aarch64.a"

mlibc-x86_64: build-x86_64 builtins-x86_64 fetch-mlibc
	@echo "==> Building mlibc (x86_64)..."
	@if [ ! -d "$(OUT_DIR)/build-x86_64/mlibc" ]; then \
		meson setup $(OUT_DIR)/build-x86_64/mlibc $(ROOT_DIR)/vendor/mlibc \
			--cross-file $(BUILD_DIR)/toolchains/meson-x86_64.txt \
			--default-library=both \
			-Dld_library_name=ld-iris \
			-Dlibgcc_dependency=false; \
	fi
	@ninja -C $(OUT_DIR)/build-x86_64/mlibc
	@echo "    [DONE] mlibc x86_64"

mlibc-aarch64: build-aarch64 builtins-aarch64 fetch-mlibc
	@echo "==> Building mlibc (aarch64)..."
	@if [ ! -d "$(OUT_DIR)/build-aarch64/mlibc" ]; then \
		meson setup $(OUT_DIR)/build-aarch64/mlibc $(ROOT_DIR)/vendor/mlibc \
			--cross-file $(BUILD_DIR)/toolchains/meson-aarch64.txt \
			--default-library=both \
			-Dld_library_name=ld-iris \
			-Dlibgcc_dependency=false; \
	fi
	@ninja -C $(OUT_DIR)/build-aarch64/mlibc
	@echo "    [DONE] mlibc aarch64"

mlibc: mlibc-x86_64 mlibc-aarch64

# ------------------------------------------------------------------------------
# User-Space Interactive Test Suite (its_irisOS)
# ------------------------------------------------------------------------------
its-irisOS-x86_64: mlibc-x86_64
	@echo "==> Compiling its_irisOS (x86_64)..."
	@mkdir -p $(OUT_DIR)/build-x86_64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=x86_64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-x86_64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-x86_64/gen \
		-isystem $(OUT_DIR)/build-x86_64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-c $(ROOT_DIR)/apps/tests/its_irisOS/main.c -o $(OUT_DIR)/build-x86_64/its_irisOS.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-x86_64/its_irisOS.o \
		$(OUT_DIR)/build-x86_64/mlibc/libc.a \
		$(OUT_DIR)/build-x86_64/lib/libiris.a \
		$(OUT_DIR)/build-x86_64/lib/libclang_rt.builtins-x86_64.a \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-x86_64/bin/its_irisOS
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/bin/its_irisOS"

its-irisOS-aarch64: mlibc-aarch64
	@echo "==> Compiling its_irisOS (aarch64)..."
	@mkdir -p $(OUT_DIR)/build-aarch64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=aarch64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-aarch64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-aarch64/gen \
		-isystem $(OUT_DIR)/build-aarch64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-c $(ROOT_DIR)/apps/tests/its_irisOS/main.c -o $(OUT_DIR)/build-aarch64/its_irisOS.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-aarch64/its_irisOS.o \
		$(OUT_DIR)/build-aarch64/mlibc/libc.a \
		$(OUT_DIR)/build-aarch64/lib/libiris.a \
		$(OUT_DIR)/build-aarch64/lib/libclang_rt.builtins-aarch64.a \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-aarch64/bin/its_irisOS
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/bin/its_irisOS"

its-irisOS: its-irisOS-x86_64 its-irisOS-aarch64

# ------------------------------------------------------------------------------
# Ring 3 VirtIO Block Driver Daemon (bin/virtio_blk)
# ------------------------------------------------------------------------------
virtio-blk-x86_64: mlibc-x86_64
	@echo "==> Compiling virtio_blk (x86_64)..."
	@mkdir -p $(OUT_DIR)/build-x86_64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=x86_64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-x86_64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-x86_64/gen \
		-isystem $(OUT_DIR)/build-x86_64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-c $(ROOT_DIR)/servers/drivers/virtio_blk/src/main.c -o $(OUT_DIR)/build-x86_64/virtio_blk.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-x86_64/virtio_blk.o \
		$(OUT_DIR)/build-x86_64/mlibc/libc.a \
		$(OUT_DIR)/build-x86_64/lib/libiris.a \
		$(OUT_DIR)/build-x86_64/lib/libclang_rt.builtins-x86_64.a \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-x86_64/bin/virtio_blk
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/bin/virtio_blk"

virtio-blk-aarch64: mlibc-aarch64
	@echo "==> Compiling virtio_blk (aarch64)..."
	@mkdir -p $(OUT_DIR)/build-aarch64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=aarch64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-aarch64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-aarch64/gen \
		-isystem $(OUT_DIR)/build-aarch64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-c $(ROOT_DIR)/servers/drivers/virtio_blk/src/main.c -o $(OUT_DIR)/build-aarch64/virtio_blk.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-aarch64/virtio_blk.o \
		$(OUT_DIR)/build-aarch64/mlibc/libc.a \
		$(OUT_DIR)/build-aarch64/lib/libiris.a \
		$(OUT_DIR)/build-aarch64/lib/libclang_rt.builtins-aarch64.a \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-aarch64/bin/virtio_blk
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/bin/virtio_blk"

virtio-blk: virtio-blk-x86_64 virtio-blk-aarch64

# ------------------------------------------------------------------------------
# Ring 3 Block Encryption Proxy Daemon (bin/iris_crypt)
# ------------------------------------------------------------------------------
iris-crypt-x86_64: mlibc-x86_64 mbedtls-x86_64
	@echo "==> Compiling iris_crypt (x86_64)..."
	@mkdir -p $(OUT_DIR)/build-x86_64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=x86_64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-DMBEDTLS_CONFIG_FILE='"mbedtls_iris_config.h"' \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-x86_64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR)/vendor/mbedtls/include \
		-isystem $(ROOT_DIR)/build \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-x86_64/gen \
		-isystem $(OUT_DIR)/build-x86_64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-c $(ROOT_DIR)/servers/iris_crypt/src/main.c -o $(OUT_DIR)/build-x86_64/iris_crypt.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-x86_64/iris_crypt.o \
		$(OUT_DIR)/build-x86_64/lib/libmbedcrypto.a \
		$(OUT_DIR)/build-x86_64/mlibc/libc.a \
		$(OUT_DIR)/build-x86_64/lib/libiris.a \
		$(OUT_DIR)/build-x86_64/lib/libclang_rt.builtins-x86_64.a \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-x86_64/bin/iris_crypt
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/bin/iris_crypt"

iris-crypt-aarch64: mlibc-aarch64 mbedtls-aarch64
	@echo "==> Compiling iris_crypt (aarch64)..."
	@mkdir -p $(OUT_DIR)/build-aarch64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=aarch64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-DMBEDTLS_CONFIG_FILE='"mbedtls_iris_config.h"' \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-aarch64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR)/vendor/mbedtls/include \
		-isystem $(ROOT_DIR)/build \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-aarch64/gen \
		-isystem $(OUT_DIR)/build-aarch64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-c $(ROOT_DIR)/servers/iris_crypt/src/main.c -o $(OUT_DIR)/build-aarch64/iris_crypt.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-aarch64/iris_crypt.o \
		$(OUT_DIR)/build-aarch64/lib/libmbedcrypto.a \
		$(OUT_DIR)/build-aarch64/mlibc/libc.a \
		$(OUT_DIR)/build-aarch64/lib/libiris.a \
		$(OUT_DIR)/build-aarch64/lib/libclang_rt.builtins-aarch64.a \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-aarch64/bin/iris_crypt
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/bin/iris_crypt"

iris-crypt: iris-crypt-x86_64 iris-crypt-aarch64

# ------------------------------------------------------------------------------
# Ring 3 Central VFS Router, GPT Parser & UFS2 Server Daemon (bin/iris_fshost)
# ------------------------------------------------------------------------------
iris-fshost-x86_64: mlibc-x86_64 libufs-x86_64
	@echo "==> Compiling iris_fshost (x86_64)..."
	@mkdir -p $(OUT_DIR)/build-x86_64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=x86_64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-D_LIBUFS \
		-isystem "$$CLANG_RES" \
		-isystem $(ROOT_DIR)/servers/iris_fshost/include/compat \
		-isystem $(ROOT_DIR)/servers/iris_fshost/include \
		-isystem $(ROOT_DIR)/vendor/libufs/libufs \
		-isystem $(ROOT_DIR)/vendor/libufs/sys \
		-isystem $(OUT_DIR)/build-x86_64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-x86_64/gen \
		-isystem $(OUT_DIR)/build-x86_64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-Wno-sign-compare -Wno-pointer-sign -Wno-unused-parameter \
		-c $(ROOT_DIR)/servers/iris_fshost/src/main.c -o $(OUT_DIR)/build-x86_64/iris_fshost.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-x86_64/iris_fshost.o \
		$(OUT_DIR)/build-x86_64/lib/libufs.a \
		$(OUT_DIR)/build-x86_64/mlibc/libc.a \
		$(OUT_DIR)/build-x86_64/lib/libiris.a \
		$(OUT_DIR)/build-x86_64/lib/libclang_rt.builtins-x86_64.a \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-x86_64/bin/iris_fshost
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/bin/iris_fshost"

iris-fshost-aarch64: mlibc-aarch64 libufs-aarch64
	@echo "==> Compiling iris_fshost (aarch64)..."
	@mkdir -p $(OUT_DIR)/build-aarch64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=aarch64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-D_LIBUFS \
		-isystem "$$CLANG_RES" \
		-isystem $(ROOT_DIR)/servers/iris_fshost/include/compat \
		-isystem $(ROOT_DIR)/servers/iris_fshost/include \
		-isystem $(ROOT_DIR)/vendor/libufs/libufs \
		-isystem $(ROOT_DIR)/vendor/libufs/sys \
		-isystem $(OUT_DIR)/build-aarch64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-aarch64/gen \
		-isystem $(OUT_DIR)/build-aarch64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-Wno-sign-compare -Wno-pointer-sign -Wno-unused-parameter \
		-c $(ROOT_DIR)/servers/iris_fshost/src/main.c -o $(OUT_DIR)/build-aarch64/iris_fshost.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-aarch64/iris_fshost.o \
		$(OUT_DIR)/build-aarch64/lib/libufs.a \
		$(OUT_DIR)/build-aarch64/mlibc/libc.a \
		$(OUT_DIR)/build-aarch64/lib/libiris.a \
		$(OUT_DIR)/build-aarch64/lib/libclang_rt.builtins-aarch64.a \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-aarch64/bin/iris_fshost
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/bin/iris_fshost"

iris-fshost: iris-fshost-x86_64 iris-fshost-aarch64

# ------------------------------------------------------------------------------
# Ring 3 Pseudo-Terminal Server Daemon (bin/iris_pty)
# ------------------------------------------------------------------------------
iris-pty-x86_64: mlibc-x86_64
	@echo "==> Compiling iris_pty (x86_64)..."
	@mkdir -p $(OUT_DIR)/build-x86_64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=x86_64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-x86_64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-x86_64/gen \
		-isystem $(OUT_DIR)/build-x86_64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-isystem $(ROOT_DIR)/servers/iris_pty/include \
		-isystem $(ROOT_DIR)/servers/drivers/virtio_input/include \
		-c $(ROOT_DIR)/servers/drivers/virtio_input/src/virtio_input.c -o $(OUT_DIR)/build-x86_64/virtio_input.o; \
	$(LLVM_PREFIX)/bin/clang --target=x86_64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-x86_64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-x86_64/gen \
		-isystem $(OUT_DIR)/build-x86_64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-isystem $(ROOT_DIR)/servers/iris_pty/include \
		-isystem $(ROOT_DIR)/servers/drivers/virtio_input/include \
		-c $(ROOT_DIR)/servers/iris_pty/src/main.c -o $(OUT_DIR)/build-x86_64/iris_pty.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-x86_64/iris_pty.o \
		$(OUT_DIR)/build-x86_64/virtio_input.o \
		$(OUT_DIR)/build-x86_64/mlibc/libc.a \
		$(OUT_DIR)/build-x86_64/lib/libiris.a \
		$(OUT_DIR)/build-x86_64/lib/libclang_rt.builtins-x86_64.a \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-x86_64/bin/iris_pty
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/bin/iris_pty"

iris-pty-aarch64: mlibc-aarch64
	@echo "==> Compiling iris_pty (aarch64)..."
	@mkdir -p $(OUT_DIR)/build-aarch64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=aarch64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-aarch64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-aarch64/gen \
		-isystem $(OUT_DIR)/build-aarch64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-isystem $(ROOT_DIR)/servers/iris_pty/include \
		-isystem $(ROOT_DIR)/servers/drivers/virtio_input/include \
		-c $(ROOT_DIR)/servers/drivers/virtio_input/src/virtio_input.c -o $(OUT_DIR)/build-aarch64/virtio_input.o; \
	$(LLVM_PREFIX)/bin/clang --target=aarch64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-aarch64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR) \
		-isystem $(ROOT_DIR)/kernel/zircon/system/public \
		-isystem $(OUT_DIR)/build-aarch64/gen \
		-isystem $(OUT_DIR)/build-aarch64/gen/zircon \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-isystem $(ROOT_DIR)/servers/iris_pty/include \
		-isystem $(ROOT_DIR)/servers/drivers/virtio_input/include \
		-c $(ROOT_DIR)/servers/iris_pty/src/main.c -o $(OUT_DIR)/build-aarch64/iris_pty.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-aarch64/iris_pty.o \
		$(OUT_DIR)/build-aarch64/virtio_input.o \
		$(OUT_DIR)/build-aarch64/mlibc/libc.a \
		$(OUT_DIR)/build-aarch64/lib/libiris.a \
		$(OUT_DIR)/build-aarch64/lib/libclang_rt.builtins-aarch64.a \
		$(OUT_DIR)/build-aarch64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-aarch64/bin/iris_pty
	@echo "    [DONE] $(OUT_DIR)/build-aarch64/bin/iris_pty"

iris-pty: iris-pty-x86_64 iris-pty-aarch64

# ------------------------------------------------------------------------------
# Image Creation & QEMU Execution
# ------------------------------------------------------------------------------
image-x86_64: build-x86_64 its-irisOS-x86_64 virtio-blk-x86_64 iris-crypt-x86_64 iris-fshost-x86_64 iris-pty-x86_64
	@$(BUILD_DIR)/mkimage.sh x86_64

image-aarch64: build-aarch64 its-irisOS-aarch64 virtio-blk-aarch64 iris-crypt-aarch64 iris-fshost-aarch64 iris-pty-aarch64
	@$(BUILD_DIR)/mkimage.sh aarch64

run: run-x86_64
run-gui: run-x86_64-gui

run-x86_64: image-x86_64
	@$(ROOT_DIR)/scripts/run_qemu.sh -a x86_64 $(if $(filter 1,$(GUI)),--gui,)

run-aarch64: image-aarch64
	@$(ROOT_DIR)/scripts/run_qemu.sh -a aarch64 $(if $(filter 1,$(GUI)),--gui,)

run-x86_64-gui: image-x86_64
	@$(ROOT_DIR)/scripts/run_qemu.sh -a x86_64 --gui

run-aarch64-gui: image-aarch64
	@$(ROOT_DIR)/scripts/run_qemu.sh -a aarch64 --gui

# ------------------------------------------------------------------------------
# Direct QEMU Execution (Fast launch without rebuilding or repacking images)
# ------------------------------------------------------------------------------
qemu-x86_64:
	@$(ROOT_DIR)/scripts/run_qemu.sh -a x86_64 $(if $(filter 1,$(GUI)),--gui,)

qemu-aarch64:
	@$(ROOT_DIR)/scripts/run_qemu.sh -a aarch64 $(if $(filter 1,$(GUI)),--gui,)

qemu-x86_64-gui:
	@$(ROOT_DIR)/scripts/run_qemu.sh -a x86_64 --gui

qemu-aarch64-gui:
	@$(ROOT_DIR)/scripts/run_qemu.sh -a aarch64 --gui

qemu: qemu-x86_64
qemu-gui: qemu-x86_64-gui

clean-kernel:
	@rm -rf $(ZIRCON_DIR)/build-x64-clang $(ZIRCON_DIR)/build-arm64-clang
	@echo "==> Cleaned kernel build outputs."

clean: clean-kernel
	@rm -rf $(OUT_DIR)/build-x86_64 $(OUT_DIR)/build-aarch64 $(OUT_DIR)/build-arm64 $(HOST_OUT) $(OUT_DIR)/images
	@echo "==> Cleaned build output."
