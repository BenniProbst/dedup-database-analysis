#!/bin/bash
# Reset PostgreSQL experiment data (dedup_lab schema)
set -euo pipefail
DRY_RUN="${1:-}"
DB_USER="dedup-lab"
SCHEMA="dedup_lab"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/postgresql"

echo "=== Reset PostgreSQL ($SCHEMA) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would DROP and recreate schema $SCHEMA"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

PG_POD=$(kubectl -n databases get pod -l app.kubernetes.io/name=postgres-ha -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$PG_POD" ]; then
  # Try alternative label
  PG_POD=$(kubectl -n databases get pod -l application=spilo -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
fi
if [ -z "$PG_POD" ]; then
  echo "[ERROR] No PostgreSQL pod found"
  exit 1
fi

echo "[1/3] Dropping schema $SCHEMA..."
kubectl -n databases exec "$PG_POD" -- psql -U "$DB_USER" -d postgres -c "DROP SCHEMA IF EXISTS $SCHEMA CASCADE;" 2>&1 || true

echo "[2/3] Recreating schema $SCHEMA..."
kubectl -n databases exec "$PG_POD" -- psql -U "$DB_USER" -d postgres -c "CREATE SCHEMA $SCHEMA AUTHORIZATION \"$DB_USER\";" 2>&1

echo "[3/3] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== PostgreSQL reset complete ==="
