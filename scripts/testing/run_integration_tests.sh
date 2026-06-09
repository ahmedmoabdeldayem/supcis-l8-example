#!/bin/bash
# run_integration_tests.sh — run integration tests against a real Oracle DB
# Requires: DB_TNS, DB_USER, DB_PASS set in environment
# In CI: Oracle XE runs as a service container (see .gitlab-ci.yml)
# Usage: scripts/testing/run_integration_tests.sh

set -e
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_ROOT"

echo "[integration] Waiting for Oracle to be ready..."
# Retry loop — Oracle takes ~30s to start in CI
for i in $(seq 1 20); do
    if sqlplus -S "$DB_USER/$DB_PASS@$DB_TNS" /nolog 2>/dev/null <<< "exit" ; then
        echo "[integration] Oracle is ready."
        break
    fi
    echo "[integration] Oracle not ready yet, retrying ($i/20)..."
    sleep 5
done

echo "[integration] Applying test migrations..."
scripts/database/migrate.sh test

echo "[integration] Building integration tests..."
gcc -Wall -std=c99 \
    -Isrc/common/include \
    -Isrc/domain/inventory/include \
    -Isrc/domain/order/include \
    -Isrc/domain/picking/include \
    src/domain/inventory/src/inventory_entity.c \
    src/domain/order/src/order_entity.c \
    src/domain/picking/src/picking_entity.c \
    src/domain/picking/src/picking_service.c \
    tests/integration/test_order_to_picking_flow.c \
    -lcmocka \
    -o build/bin/test_integration

echo "[integration] Running integration tests..."
./build/bin/test_integration

echo "[integration] All integration tests passed."
