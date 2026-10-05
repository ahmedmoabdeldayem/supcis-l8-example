#!/bin/bash
# valgrind_check.sh — run unit tests under Valgrind to detect memory leaks
# Valgrind instruments every malloc/free and reports leaks or invalid accesses.
# Usage: scripts/testing/valgrind_check.sh

set -e
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_ROOT"

# Make sure the test binary is compiled first
scripts/testing/run_unit_tests.sh

echo "[valgrind] Running memory checks..."
# --leak-check=full    report all leaks with full stack traces
# --track-origins=yes  show where uninitialised values came from
# --error-exitcode=1   fail if any error is detected
valgrind \
    --leak-check=full \
    --error-exitcode=1 \
    --track-origins=yes \
    ./build/bin/test_inventory

echo "[valgrind] No memory issues detected."
