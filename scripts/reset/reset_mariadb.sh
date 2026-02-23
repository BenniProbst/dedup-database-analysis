#!/bin/bash
# Reset MariaDB experiment data (dedup_lab schema)
# Usage: ./reset_mariadb.sh [--dry-run]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRY_RUN="${1:-}"
DB_HOST="mariadb.databases.svc.cluster.local"
DB_PORT=3306
DB_USER="dedup-lab"
SCHEMA="dedup_lab"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/mariadb"

echo "=== Reset MariaDB ($SCHEMA) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would DROP and recreate schema $SCHEMA"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

# Find a MariaDB pod to run commands
MARIA_POD=$(kubectl -n databases get pod -l app.kubernetes.io/name=mariadb -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$MARIA_POD" ]; then
  echo "[ERROR] No MariaDB pod found in databases namespace"
  exit 1
fi

echo "[1/3] Dropping schema $SCHEMA..."
kubectl -n databases exec "$MARIA_POD" -- mariadb -u "$DB_USER" -e "DROP DATABASE IF EXISTS $SCHEMA;" 2>&1 || true

echo "[2/3] Recreating schema $SCHEMA..."
kubectl -n databases exec "$MARIA_POD" -- mariadb -u "$DB_USER" -e "CREATE DATABASE $SCHEMA; GRANT ALL ON $SCHEMA.* TO '$DB_USER'@'%';" 2>&1

echo "[3/3] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== MariaDB reset complete ==="
