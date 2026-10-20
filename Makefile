# ==============================================================================
# irisOS - Root Makefile
# ==============================================================================
# Dispatches high-level build and run commands:
#   make build all | x86_64 | aarch64
#   make run all   | x86_64 | aarch64
#   make clean
# ==============================================================================

SHELL := /bin/bash
ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
IRIS_CLI := $(ROOT_DIR)/scripts/iris.sh

CMD := $(firstword $(MAKECMDGOALS))

ifeq ($(CMD),build)
  BUILD_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  # Suppress errors for subsequent words treated as goals
  %::
	@:
.PHONY: build
build:
	@$(IRIS_CLI) build $(BUILD_ARGS)

else ifeq ($(CMD),run)
  RUN_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  # Suppress errors for subsequent words treated as goals
  %::
	@:
.PHONY: run
run:
	@$(IRIS_CLI) run $(RUN_ARGS)

else ifeq ($(CMD),test)
  TEST_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  # Suppress errors for subsequent words treated as goals
  %::
	@:
.PHONY: test
test:
	@$(IRIS_CLI) test $(TEST_ARGS)

else ifeq ($(CMD),image)
  IMAGE_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  # Suppress errors for subsequent words treated as goals
  %::
	@:
.PHONY: image
image:
	@$(MAKE) -C $(ROOT_DIR)/build image$(if $(IMAGE_ARGS),-$(IMAGE_ARGS),)

else

.PHONY: default help all clean x86_64 aarch64 image

default: help

help:
	@$(IRIS_CLI) help

all:
	@$(IRIS_CLI) build all

clean:
	@$(IRIS_CLI) clean

x86_64:
	@$(IRIS_CLI) build x86_64

aarch64:
	@$(IRIS_CLI) build aarch64

endif
