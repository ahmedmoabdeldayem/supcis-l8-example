#!/bin/bash
# rollback.sh — roll back the last applied migration
# Usage: scripts/database/rollback.sh [dev|staging|production]
#
# NOTE: Use with caution. Rollbacks can cause data loss.
# The rollback scripts are in database/rollback/R*.sql

set -e
ENV=${1:-dev}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/../.."
source "$PROJECT_ROOT/config/environments/${ENV}.env"

# Find the last applied version
last_version=$(sqlplus -S "$DB_USER/$DB_PASS@$DB_TNS" << SQL
SET HEADING OFF FEEDBACK OFF TRIMOUT ON
SELECT MAX(version) FROM schema_version;
EXIT;
SQL
)
last_version=$(echo "$last_version" | tr -d '[:space:]')

echo "[rollback] Rolling back: $last_version"

rollback_file="$PROJECT_ROOT/database/rollback/R${last_version#V}__rollback.sql"
if [ -f "$rollback_file" ]; then
    sqlplus -S "$DB_USER/$DB_PASS@$DB_TNS" @"$rollback_file"
    sqlplus -S "$DB_USER/$DB_PASS@$DB_TNS" << SQL
DELETE FROM schema_version WHERE version = '$last_version';
COMMIT;
EXIT;
SQL
    echo "[rollback] Done."
else
    echo "[rollback] ERROR: No rollback script found at $rollback_file"
    exit 1
fi
