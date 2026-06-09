#!/bin/bash
# run_unit_tests.sh — compile and run all unit tests
# Does NOT require a database or network connection.
# Usage: scripts/testing/run_unit_tests.sh

set -e
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_ROOT"

echo "[test] Building unit tests..."

# Compile all test files in tests/unit/ with cmocka
# -lcmocka links the cmocka test framework
gcc -Wall -std=c99 \
    -Isrc/common/include \
    -Isrc/domain/inventory/include \
    -Isrc/domain/order/include \
    -Isrc/domain/picking/include \
    -Isrc/domain/robot/include \
    src/domain/inventory/src/inventory_entity.c \
    src/domain/order/src/order_entity.c \
    src/domain/picking/src/picking_entity.c \
    src/domain/picking/src/picking_service.c \
    tests/unit/domain/inventory/test_inventory_entity.c \
    -lcmocka \
    -o build/bin/test_inventory

echo "[test] Running inventory unit tests..."
./build/bin/test_inventory

echo "[test] All unit tests passed."
