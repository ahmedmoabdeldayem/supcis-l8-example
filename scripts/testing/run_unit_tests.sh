#!/bin/bash
# run_unit_tests.sh — compile and run all unit tests
# Does NOT require a database or network connection.
# Usage: scripts/testing/run_unit_tests.sh

set -e
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_ROOT"

mkdir -p build/bin

# On macOS with Homebrew, cmocka lives outside the standard search path
EXTRA_FLAGS=""
if [ "$(uname)" = "Darwin" ] && [ -d /opt/homebrew ]; then
    EXTRA_FLAGS="-I/opt/homebrew/include -L/opt/homebrew/lib"
fi

ALL_INCLUDES="\
    -Isrc/common/include \
    -Isrc/domain/inventory/include \
    -Isrc/domain/order/include \
    -Isrc/domain/picking/include \
    -Isrc/domain/robot/include \
    -Isrc/application/include \
    -Isrc/infrastructure/api/include \
    -Isrc/infrastructure/database/include \
    -Isrc/infrastructure/configuration/include \
    -Isrc/infrastructure/external_integration/include"

COMMON="src/common/src/logger.c"

echo "[test] Building and running unit tests..."

# ── Inventory ──────────────────────────────────────────────────────────────
gcc -Wall -std=c99 $EXTRA_FLAGS $ALL_INCLUDES \
    "$COMMON" \
    src/domain/inventory/src/inventory_entity.c \
    tests/unit/domain/inventory/test_inventory_entity.c \
    -lcmocka -o build/bin/test_inventory
./build/bin/test_inventory

# ── Order ──────────────────────────────────────────────────────────────────
gcc -Wall -std=c99 $EXTRA_FLAGS $ALL_INCLUDES \
    "$COMMON" \
    src/domain/order/src/order_entity.c \
    tests/unit/domain/order/test_order_entity.c \
    -lcmocka -o build/bin/test_order
./build/bin/test_order

# ── Picking ────────────────────────────────────────────────────────────────
gcc -Wall -std=c99 $EXTRA_FLAGS $ALL_INCLUDES \
    "$COMMON" \
    src/domain/picking/src/picking_entity.c \
    tests/unit/domain/picking/test_picking_entity.c \
    -lcmocka -o build/bin/test_picking
./build/bin/test_picking

# ── AutoStore port ─────────────────────────────────────────────────────────
gcc -Wall -std=c99 $EXTRA_FLAGS $ALL_INCLUDES \
    "$COMMON" \
    src/domain/robot/src/autostore_port.c \
    tests/unit/domain/robot/test_autostore_port.c \
    -lcmocka -lcurl -o build/bin/test_autostore
./build/bin/test_autostore

# ── REST handlers ──────────────────────────────────────────────────────────
gcc -Wall -std=c99 $EXTRA_FLAGS $ALL_INCLUDES \
    "$COMMON" \
    src/domain/inventory/src/inventory_entity.c \
    src/infrastructure/api/src/inventory_rest_handlers.c \
    tests/unit/infrastructure/api/test_rest_handler.c \
    -lcmocka -lcurl -o build/bin/test_rest
./build/bin/test_rest

echo "[test] All unit tests passed."
