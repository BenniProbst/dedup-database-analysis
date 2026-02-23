#!/bin/bash
# Reset ClickHouse experiment data (dedup_lab database)
# Only drops the dedup_lab database — other databases/users untouched.
# Usage: ./reset_clickhouse.sh [--dry-run]
set -euo pipefail
DRY_RUN="${1:-}"
DB_USER="dedup-lab"
DB_PASS="${CLICKHOUSE_PASSWORD:-}"
SCHEMA="dedup_lab"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/clickhouse"

# Resolve password from K8s Secret if not set
if [ -z "$DB_PASS" ]; then
  DB_PASS=$(kubectl -n databases get secret dedup-credentials -o jsonpath='{.data.CLICKHOUSE_PASSWORD}' 2>/dev/null | base64 -d 2>/dev/null) || true
fi

echo "=== Reset ClickHouse ($SCHEMA) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would DROP and recreate database $SCHEMA"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

CH_POD=$(kubectl -n databases get pod -l app.kubernetes.io/name=clickhouse -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$CH_POD" ]; then
  echo "[ERROR] No ClickHouse pod found"
  exit 1
fi

# Build auth flags (use lab user if password available, otherwise default)
AUTH_FLAGS=""
if [ -n "$DB_PASS" ]; then
  AUTH_FLAGS="--user $DB_USER --password $DB_PASS"
else
  AUTH_FLAGS="--user default"
  echo "[WARN] No lab user password, using default user"
fi

echo "[1/3] Dropping database $SCHEMA..."
kubectl -n databases exec "$CH_POD" -- clickhouse-client $AUTH_FLAGS --query "DROP DATABASE IF EXISTS $SCHEMA;" 2>&1 || true

echo "[2/3] Recreating database $SCHEMA..."
kubectl -n databases exec "$CH_POD" -- clickhouse-client $AUTH_FLAGS --query "CREATE DATABASE IF NOT EXISTS $SCHEMA;" 2>&1

echo "[3/3] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== ClickHouse reset complete ==="
