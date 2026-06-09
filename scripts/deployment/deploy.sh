#!/bin/bash
# deploy.sh — deploy SuPCIS-L8 binary and config to a target environment
# Usage: scripts/deployment/deploy.sh [staging|production]
#
# What this script does:
#   1. Validates the binary was built
#   2. Loads the target environment config
#   3. Copies binary + config to the remote server via SSH/rsync
#   4. Runs database migrations on the target
#   5. Restarts the service via systemctl

set -e

ENV=${1:-staging}
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BINARY="$PROJECT_ROOT/build/bin/supcis-l8"

# Safety check: refuse to deploy if binary doesn't exist
if [ ! -f "$BINARY" ]; then
    echo "[deploy] ERROR: Binary not found at $BINARY — run 'make' first."
    exit 1
fi

# Load the target environment (provides DEPLOY_HOST, DEPLOY_USER, etc.)
source "$PROJECT_ROOT/config/environments/${ENV}.env"

echo "[deploy] Deploying to $ENV ($DEPLOY_HOST)..."

# Copy binary to remote server
rsync -avz --progress "$BINARY" "${DEPLOY_USER}@${DEPLOY_HOST}:/opt/supcis-l8/bin/"

# Copy customer config (exclude secrets — those are already on the server)
rsync -avz --exclude='*.env' \
    "$PROJECT_ROOT/config/" \
    "${DEPLOY_USER}@${DEPLOY_HOST}:/opt/supcis-l8/config/"

# Run DB migrations on the target server
echo "[deploy] Running migrations on $ENV..."
ssh "${DEPLOY_USER}@${DEPLOY_HOST}" \
    "cd /opt/supcis-l8 && scripts/database/migrate.sh $ENV"

# Restart the service
echo "[deploy] Restarting service..."
ssh "${DEPLOY_USER}@${DEPLOY_HOST}" \
    "sudo systemctl restart supcis-l8"

# Wait and confirm the service is running
sleep 3
ssh "${DEPLOY_USER}@${DEPLOY_HOST}" \
    "systemctl is-active supcis-l8 && echo '[deploy] Service is running.'"

echo "[deploy] Deployment to $ENV complete."
