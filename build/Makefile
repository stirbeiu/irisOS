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

.PHONY: all tools zbi abigen build-x86_64 build-arm64 clean fetch-compiler-rt fetch-mlibc builtins-x86_64 builtins-arm64 mlibc-x86_64 mlibc-arm64 mlibc

all: tools

# ------------------------------------------------------------------------------
# Host Tools (Darwin Apple Silicon)
# ------------------------------------------------------------------------------
tools: $(HOST_OUT)/zbi $(HOST_OUT)/abigen

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

kernel-arm64: tools
	@echo "==> Building Zircon microkernel (arm64)..."
	@$(MAKE) -C $(ZIRCON_DIR) \
		build-arm64-clang/zircon.elf \
		build-arm64-clang/kernel.zbi \
		PROJECT=arm64 \
		USE_CLANG=true \
		USE_LLD=true \
		CLANG_TOOLCHAIN_PREFIX=$(LLVM_PREFIX)/bin/ \
		ARCH_arm64_TOOLCHAIN_INCLUDED=1 \
		ENABLE_BUILD_SYSROOT=false
	@mkdir -p $(OUT_DIR)/build-arm64/kernel
	@cp -f $(ZIRCON_DIR)/build-arm64-clang/zircon.elf $(OUT_DIR)/build-arm64/kernel/zircon.elf
	@cp -f $(ZIRCON_DIR)/build-arm64-clang/kernel.zbi $(OUT_DIR)/build-arm64/kernel/kernel.zbi
	@echo "    [DONE] $(OUT_DIR)/build-arm64/kernel/zircon.elf"

build-x86_64: tools kernel-x86_64
	@mkdir -p $(OUT_DIR)/build-x86_64
	@cmake -B $(OUT_DIR)/build-x86_64 -S $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(BUILD_DIR)/toolchains/x86_64-iris.cmake \
		-G Ninja
	@ninja -C $(OUT_DIR)/build-x86_64

build-arm64: tools kernel-arm64
	@mkdir -p $(OUT_DIR)/build-arm64
	@cmake -B $(OUT_DIR)/build-arm64 -S $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(BUILD_DIR)/toolchains/arm64-iris.cmake \
		-G Ninja
	@ninja -C $(OUT_DIR)/build-arm64

# ------------------------------------------------------------------------------
# compiler-rt Builtins & mlibc C Library
# ------------------------------------------------------------------------------
fetch-compiler-rt:
	@$(ROOT_DIR)/scripts/fetch_compiler_rt.sh

fetch-mlibc:
	@$(ROOT_DIR)/scripts/fetch_mlibc.sh

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

builtins-arm64: fetch-compiler-rt
	@mkdir -p $(OUT_DIR)/build-arm64/lib $(OUT_DIR)/build-arm64/builtins
	@echo "==> Building compiler-rt builtins (arm64)..."
	@for f in $(ROOT_DIR)/vendor/compiler-rt/lib/builtins/*.c; do \
		$(HOST_CC) -c "$$f" --target=aarch64-unknown-linux-gnu -O2 -fPIC \
			-I$(ROOT_DIR)/vendor/compiler-rt/lib/builtins \
			-o $(OUT_DIR)/build-arm64/builtins/$$(basename "$$f" .c).o 2>/dev/null || true; \
	done
	@$(LLVM_PREFIX)/bin/llvm-ar rcs $(OUT_DIR)/build-arm64/lib/libclang_rt.builtins-aarch64.a $(OUT_DIR)/build-arm64/builtins/*.o
	@echo "    [DONE] $(OUT_DIR)/build-arm64/lib/libclang_rt.builtins-aarch64.a"

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

mlibc-arm64: build-arm64 builtins-arm64 fetch-mlibc
	@echo "==> Building mlibc (arm64)..."
	@if [ ! -d "$(OUT_DIR)/build-arm64/mlibc" ]; then \
		meson setup $(OUT_DIR)/build-arm64/mlibc $(ROOT_DIR)/vendor/mlibc \
			--cross-file $(BUILD_DIR)/toolchains/meson-arm64.txt \
			--default-library=both \
			-Dld_library_name=ld-iris \
			-Dlibgcc_dependency=false; \
	fi
	@ninja -C $(OUT_DIR)/build-arm64/mlibc
	@echo "    [DONE] mlibc arm64"

mlibc: mlibc-x86_64 mlibc-arm64

# ------------------------------------------------------------------------------
# User-Space Test Programs (hello_c)
# ------------------------------------------------------------------------------
hello-c-x86_64: mlibc-x86_64
	@echo "==> Compiling hello_c (x86_64)..."
	@mkdir -p $(OUT_DIR)/build-x86_64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=x86_64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-x86_64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-c $(ROOT_DIR)/apps/tests/hello_c/main.c -o $(OUT_DIR)/build-x86_64/hello_c.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-x86_64/hello_c.o \
		$(OUT_DIR)/build-x86_64/mlibc/libc.a \
		$(OUT_DIR)/build-x86_64/lib/libiris.a \
		$(OUT_DIR)/build-x86_64/lib/libclang_rt.builtins-x86_64.a \
		$(OUT_DIR)/build-x86_64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-x86_64/bin/hello_c
	@echo "    [DONE] $(OUT_DIR)/build-x86_64/bin/hello_c"

hello-c-arm64: mlibc-arm64
	@echo "==> Compiling hello_c (arm64)..."
	@mkdir -p $(OUT_DIR)/build-arm64/bin
	@CLANG_RES=$$($(LLVM_PREFIX)/bin/clang -print-resource-dir)/include; \
	$(LLVM_PREFIX)/bin/clang --target=aarch64-unknown-linux-gnu -O2 -fPIE -nostdinc \
		-isystem "$$CLANG_RES" \
		-isystem $(OUT_DIR)/build-arm64/mlibc \
		-isystem $(ROOT_DIR)/sdk/mlibc-sysdeps/iris/include \
		-isystem $(ROOT_DIR)/sdk/libiris/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/ansi/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/posix/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/bsd/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/glibc/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/options/internal/include \
		-isystem $(ROOT_DIR)/vendor/mlibc/include \
		-c $(ROOT_DIR)/apps/tests/hello_c/main.c -o $(OUT_DIR)/build-arm64/hello_c.o
	@/opt/homebrew/bin/ld.lld -pie -z text \
		$(OUT_DIR)/build-arm64/mlibc/sysdeps/iris/crt1.o \
		$(OUT_DIR)/build-arm64/mlibc/sysdeps/iris/crti.o \
		$(OUT_DIR)/build-arm64/hello_c.o \
		$(OUT_DIR)/build-arm64/mlibc/libc.a \
		$(OUT_DIR)/build-arm64/lib/libiris.a \
		$(OUT_DIR)/build-arm64/lib/libclang_rt.builtins-aarch64.a \
		$(OUT_DIR)/build-arm64/mlibc/sysdeps/iris/crtn.o \
		-o $(OUT_DIR)/build-arm64/bin/hello_c
	@echo "    [DONE] $(OUT_DIR)/build-arm64/bin/hello_c"

hello-c: hello-c-x86_64 hello-c-arm64

# ------------------------------------------------------------------------------
# Image Creation & QEMU Execution
# ------------------------------------------------------------------------------
image-x86_64: build-x86_64 hello-c-x86_64
	@$(BUILD_DIR)/mkimage.sh x86_64

image-arm64: build-arm64 hello-c-arm64
	@$(BUILD_DIR)/mkimage.sh arm64

run-x86: image-x86_64
	@$(ROOT_DIR)/scripts/run_qemu.sh -a x86_64 $(if $(filter 1,$(GUI)),--gui,)

run-arm: image-arm64
	@$(ROOT_DIR)/scripts/run_qemu.sh -a arm64 $(if $(filter 1,$(GUI)),--gui,)

run-x86-gui: image-x86_64
	@$(ROOT_DIR)/scripts/run_qemu.sh -a x86_64 --gui

run-arm-gui: image-arm64
	@$(ROOT_DIR)/scripts/run_qemu.sh -a arm64 --gui

clean-kernel:
	@rm -rf $(ZIRCON_DIR)/build-x64-clang $(ZIRCON_DIR)/build-arm64-clang
	@echo "==> Cleaned kernel build outputs."

clean: clean-kernel
	@rm -rf $(OUT_DIR)/build-x86_64 $(OUT_DIR)/build-arm64 $(HOST_OUT) $(OUT_DIR)/images
	@echo "==> Cleaned build output."
