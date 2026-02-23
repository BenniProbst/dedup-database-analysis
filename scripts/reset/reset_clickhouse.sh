#!/bin/bash
# Reset ClickHouse experiment data (dedup_lab database)
set -euo pipefail
DRY_RUN="${1:-}"
DB_HOST="clickhouse.databases.svc.cluster.local"
SCHEMA="dedup_lab"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/clickhouse"

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

echo "[1/3] Dropping database $SCHEMA..."
kubectl -n databases exec "$CH_POD" -- clickhouse-client --query "DROP DATABASE IF EXISTS $SCHEMA;" 2>&1 || true

echo "[2/3] Recreating database $SCHEMA..."
kubectl -n databases exec "$CH_POD" -- clickhouse-client --query "CREATE DATABASE $SCHEMA;" 2>&1

echo "[3/3] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== ClickHouse reset complete ==="
