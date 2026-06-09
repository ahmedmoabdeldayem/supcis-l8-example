#!/bin/bash
# migrate.sh — apply pending Oracle DB migrations in version order
# Usage: scripts/database/migrate.sh [dev|staging|production]
#
# HOW IT WORKS:
#   1. Load the correct environment file
#   2. Connect to Oracle via sqlplus
#   3. Read the schema_version table to see which migrations were already applied
#   4. Apply any V*.sql files whose version is not yet in schema_version
#   5. Insert a record into schema_version after each successful migration

set -e

ENV=${1:-dev}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/../.."
MIGRATIONS_DIR="$PROJECT_ROOT/database/migrations"

# Load environment variables (DB credentials etc.)
source "$PROJECT_ROOT/config/environments/${ENV}.env"

echo "[migrate] Connecting to Oracle: $DB_TNS as $DB_USER"
echo "[migrate] Scanning migrations in: $MIGRATIONS_DIR"

# Apply each migration file in sorted order (V001, V002, ...)
for migration_file in $(ls "$MIGRATIONS_DIR"/V*.sql | sort); do
    version=$(basename "$migration_file" | cut -d'_' -f1)  # extract "V001"

    # Check if already applied
    already_applied=$(sqlplus -S "$DB_USER/$DB_PASS@$DB_TNS" << SQL
SET HEADING OFF FEEDBACK OFF
SELECT COUNT(*) FROM schema_version WHERE version = '$version';
EXIT;
SQL
    )

    if [ "$already_applied" -eq "0" ]; then
        echo "[migrate] Applying $version: $migration_file"
        sqlplus -S "$DB_USER/$DB_PASS@$DB_TNS" @"$migration_file"

        # Record successful migration
        sqlplus -S "$DB_USER/$DB_PASS@$DB_TNS" << SQL
INSERT INTO schema_version (version, description)
VALUES ('$version', '$(basename "$migration_file")');
COMMIT;
EXIT;
SQL
        echo "[migrate] $version applied."
    else
        echo "[migrate] $version already applied — skipping."
    fi
done

echo "[migrate] All migrations up to date."
