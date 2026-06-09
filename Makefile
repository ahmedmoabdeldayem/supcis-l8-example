# SuPCIS-L8 Makefile
# Usage:
#   make           — build the server binary
#   make test      — run unit tests
#   make clean     — remove build artefacts
#   make check     — run cppcheck static analysis

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -fPIC \
          -Isrc/common/include \
          -Isrc/domain/inventory/include \
          -Isrc/domain/order/include \
          -Isrc/domain/picking/include \
          -Isrc/domain/robot/include \
          -Isrc/application/include \
          -Isrc/infrastructure/api/include \
          -Isrc/infrastructure/database/include \
          -Isrc/infrastructure/configuration/include \
          -Isrc/infrastructure/external_integration/include

# Libraries available on a production RedHat/Oracle server:
#   -loci     Oracle Call Interface  (requires Oracle Instant Client — Linux only)
#   -ljansson JSON library           (install: sudo dnf install jansson-devel)
#   -lcurl    HTTP client            (available on macOS and Linux)
#
# On a Mac dev machine, remove -loci and -ljansson to link without Oracle/jansson.
# The stubs compile fine without them — only the real implementations need them.
LDFLAGS = -lcurl -lpthread -lm

BUILD_DIR = build
BIN_DIR   = $(BUILD_DIR)/bin
OBJ_DIR   = $(BUILD_DIR)/obj

TARGET = $(BIN_DIR)/supcis-l8

# Collect all .c source files recursively.
# Exclude *_oci_real.c — those files require Oracle Instant Client headers
# (oci.h) and are provided for study only, not compiled on Mac dev machines.
SRCS = $(shell find src -name '*.c' ! -name '*_oci_real.c')
OBJS = $(patsubst src/%.c, $(OBJ_DIR)/%.o, $(SRCS))

.PHONY: all test clean check

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build OK: $@"

$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test:
	@cd tests && make -f Makefile.test

check:
	cppcheck --enable=all --std=c99 --error-exitcode=1 src/

clean:
	rm -rf $(BUILD_DIR)
