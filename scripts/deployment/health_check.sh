#!/bin/bash
# health_check.sh — verify SuPCIS-L8 is responding after deployment
# Usage: scripts/deployment/health_check.sh <host> <api_key>

HOST=${1:-localhost:8080}
API_KEY=${2:-replace-me}

echo "[health] Checking $HOST..."

# Hit a lightweight status endpoint — returns 200 if server is up
HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
    -H "X-API-Key: $API_KEY" \
    "http://$HOST/api/v1/health")

if [ "$HTTP_STATUS" = "200" ]; then
    echo "[health] Server is healthy (HTTP 200)."
    exit 0
else
    echo "[health] ERROR: Expected 200, got $HTTP_STATUS"
    exit 1
fi
