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
valgrind \
    --leak-check=full \       # report all memory leaks with full stack traces
    --error-exitcode=1 \      # fail the script if any error is found
    --track-origins=yes \     # show where uninitialised values came from
    ./build/bin/test_inventory

echo "[valgrind] No memory issues detected."
