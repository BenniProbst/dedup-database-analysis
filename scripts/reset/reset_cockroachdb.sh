#!/bin/bash
# Reset CockroachDB experiment data (dedup_lab database)
set -euo pipefail
DRY_RUN="${1:-}"
DB_USER="dedup_lab"
DATABASE="dedup_lab"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/cockroachdb"

echo "=== Reset CockroachDB ($DATABASE) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would DROP and recreate database $DATABASE"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

CRDB_POD=$(kubectl -n cockroach-operator-system get pod -l app.kubernetes.io/name=cockroachdb -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$CRDB_POD" ]; then
  echo "[ERROR] No CockroachDB pod found"
  exit 1
fi

echo "[1/3] Dropping database $DATABASE..."
kubectl -n cockroach-operator-system exec "$CRDB_POD" -- cockroach sql --insecure -e "DROP DATABASE IF EXISTS $DATABASE CASCADE;" 2>&1 || true

echo "[2/3] Recreating database $DATABASE..."
kubectl -n cockroach-operator-system exec "$CRDB_POD" -- cockroach sql --insecure -e "CREATE DATABASE $DATABASE; GRANT ALL ON DATABASE $DATABASE TO $DB_USER;" 2>&1

echo "[3/3] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== CockroachDB reset complete ==="
