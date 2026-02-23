#!/bin/bash
# Reset MariaDB experiment data (dedup_lab database)
# Only drops tables inside dedup_lab — other databases/users untouched.
# Usage: ./reset_mariadb.sh [--dry-run]
set -euo pipefail
DRY_RUN="${1:-}"
DB_USER="dedup-lab"
DB_PASS="${MARIADB_PASSWORD:-}"
SCHEMA="dedup_lab"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/mariadb"

# Resolve password from K8s Secret if not set
if [ -z "$DB_PASS" ]; then
  DB_PASS=$(kubectl -n databases get secret dedup-credentials -o jsonpath='{.data.MARIADB_PASSWORD}' 2>/dev/null | base64 -d 2>/dev/null) || true
fi
if [ -z "$DB_PASS" ]; then
  echo "[ERROR] No MariaDB password. Set MARIADB_PASSWORD or ensure dedup-credentials secret exists."
  exit 1
fi

echo "=== Reset MariaDB ($SCHEMA) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would DROP and recreate database $SCHEMA"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

MARIA_POD=$(kubectl -n databases get pod -l app.kubernetes.io/name=mariadb -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$MARIA_POD" ]; then
  echo "[ERROR] No MariaDB pod found in databases namespace"
  exit 1
fi

echo "[1/3] Dropping database $SCHEMA..."
kubectl -n databases exec "$MARIA_POD" -- mariadb -u "$DB_USER" -p"$DB_PASS" -e "DROP DATABASE IF EXISTS $SCHEMA;" 2>&1 || true

echo "[2/3] Recreating database $SCHEMA..."
kubectl -n databases exec "$MARIA_POD" -- mariadb -u "$DB_USER" -p"$DB_PASS" -e "CREATE DATABASE IF NOT EXISTS $SCHEMA;" 2>&1

echo "[3/3] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== MariaDB reset complete ==="
