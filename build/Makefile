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
else ifeq ($(CMD),test)
  TEST_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  ifneq ($(TEST_ARGS),)
    %::
	@:
.PHONY: test
test:
	@$(ROOT_DIR)/scripts/iris.sh test $(TEST_ARGS)
  endif
endif

all: image-x86_64
all-x86_64: image-x86_64
all-aarch64: image-aarch64
all-both: image-x86_64 image-aarch64

build: build-x86_64
build-all: build-x86_64 build-aarch64

image: image-x86_64
image-all: image-x86_64 image-aarch64

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
	@echo "==> Building host UFS2 + AES-256-XTS formatter via CMake ($(HOST_OUT)/mkufs2)..."
	@cmake -B $(HOST_OUT) -S $(BUILD_DIR) -G Ninja
	@ninja -C $(HOST_OUT) mkufs2
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
# Dependency Fetch Rules
# ------------------------------------------------------------------------------
fetch-compiler-rt:
	@$(ROOT_DIR)/scripts/fetch_compiler_rt.sh

fetch-mlibc:
	@$(ROOT_DIR)/scripts/fetch_mlibc.sh

fetch-mbedtls:
	@$(ROOT_DIR)/scripts/fetch_mbedtls.sh

fetch-libufs:
	@$(ROOT_DIR)/scripts/fetch_ufs.sh

fetch-toybox:
	@$(ROOT_DIR)/scripts/fetch_toybox.sh

# ------------------------------------------------------------------------------
# Sub-Target Aliases (delegating to Ninja)
# ------------------------------------------------------------------------------
libufs-x86_64: build-x86_64
libufs-aarch64: build-aarch64
libufs: libufs-x86_64 libufs-aarch64

mbedtls-x86_64: build-x86_64
mbedtls-aarch64: build-aarch64
mbedtls: mbedtls-x86_64 mbedtls-aarch64

builtins-x86_64: build-x86_64
builtins-aarch64: build-aarch64

mlibc-x86_64: build-x86_64
mlibc-aarch64: build-aarch64
mlibc: mlibc-x86_64 mlibc-aarch64

# ------------------------------------------------------------------------------
# Userland Daemons & Tests (delegating to Ninja)
# ------------------------------------------------------------------------------
its-irisOS-x86_64: build-x86_64
its-irisOS-aarch64: build-aarch64
its-irisOS: its-irisOS-x86_64 its-irisOS-aarch64

virtio-blk-x86_64: build-x86_64
virtio-blk-aarch64: build-aarch64
virtio-blk: virtio-blk-x86_64 virtio-blk-aarch64

iris-crypt-x86_64: build-x86_64
iris-crypt-aarch64: build-aarch64
iris-crypt: iris-crypt-x86_64 iris-crypt-aarch64

iris-fshost-x86_64: build-x86_64
iris-fshost-aarch64: build-aarch64
iris-fshost: iris-fshost-x86_64 iris-fshost-aarch64

iris-pty-x86_64: build-x86_64
iris-pty-aarch64: build-aarch64
iris-pty: iris-pty-x86_64 iris-pty-aarch64

toybox-x86_64: build-x86_64
	@$(ROOT_DIR)/scripts/build_toybox.sh x86_64 $(OUT_DIR)/build-x86_64

toybox-aarch64: build-aarch64
	@$(ROOT_DIR)/scripts/build_toybox.sh aarch64 $(OUT_DIR)/build-aarch64

toybox: toybox-x86_64 toybox-aarch64

# ------------------------------------------------------------------------------
# Image Creation & QEMU Execution
# ------------------------------------------------------------------------------
image-x86_64: build-x86_64 toybox-x86_64
	@$(BUILD_DIR)/mkimage.sh x86_64

image-aarch64: build-aarch64 toybox-aarch64
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
