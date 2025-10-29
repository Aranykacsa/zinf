# ====== Global Makefile ======
.PHONY: all clean embedded desktop tests tools

# Directories
SRC_DIR := src
CORE_DIR := $(SRC_DIR)/core
INC_DIR := $(SRC_DIR)/include
PLATFORM_DIR := $(SRC_DIR)/platform

BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

CC := gcc
CFLAGS := -Wall -Wextra -I$(INC_DIR) -std=c11 -O2

# Default target
all: desktop

# ---- Build host simulation ----
desktop:
	@echo "🔧 Building MyFS desktop simulation..."
	$(MAKE) -C examples/desktop

# ---- Build embedded firmware ----
embedded:
	@echo "🔧 Building MyFS embedded firmware..."
	$(MAKE) -C examples/embedded

# ---- Run tests ----
tests:
	@echo "🧪 Running unit tests..."
	$(MAKE) -C tests run

# ---- Build tools ----
tools:
	@echo "🔧 Building filesystem utilities..."
	$(MAKE) -C tools

# ---- Clean ----
clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C examples/desktop clean
	$(MAKE) -C examples/embedded clean
	$(MAKE) -C tests clean
	$(MAKE) -C tools clean

