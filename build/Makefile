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

.PHONY: all tools zbi abigen build-x86_64 build-arm64 clean

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
# Target Architecture Builds (CMake + Ninja)
# ------------------------------------------------------------------------------
build-x86_64: tools
	@mkdir -p $(OUT_DIR)/build-x86_64
	@cmake -B $(OUT_DIR)/build-x86_64 -S $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(BUILD_DIR)/toolchains/x86_64-iris.cmake \
		-G Ninja
	@ninja -C $(OUT_DIR)/build-x86_64

build-arm64: tools
	@mkdir -p $(OUT_DIR)/build-arm64
	@cmake -B $(OUT_DIR)/build-arm64 -S $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(BUILD_DIR)/toolchains/arm64-iris.cmake \
		-G Ninja
	@ninja -C $(OUT_DIR)/build-arm64

clean:
	@rm -rf $(OUT_DIR)/build-x86_64 $(OUT_DIR)/build-arm64 $(HOST_OUT)
	@echo "==> Cleaned build output."
