#!/bin/bash
# Reset MinIO experiment data (dedup-lab-* buckets)
set -euo pipefail
DRY_RUN="${1:-}"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/minio"

echo "=== Reset MinIO (dedup-lab-* buckets) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would clear dedup-lab-* buckets"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

MINIO_POD=$(kubectl -n minio get pod -l app.kubernetes.io/name=minio -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$MINIO_POD" ]; then
  echo "[ERROR] No MinIO pod found"
  exit 1
fi

echo "[1/2] Clearing dedup-lab-* buckets..."
# Use mc inside MinIO pod to remove experiment buckets
kubectl -n minio exec "$MINIO_POD" -- sh -c '
  mc alias set local http://localhost:9000 "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" 2>/dev/null || true
  for bucket in $(mc ls local/ 2>/dev/null | grep "dedup-lab" | awk "{print \$NF}"); do
    mc rm --recursive --force "local/$bucket" 2>/dev/null || true
    echo "  Cleared: $bucket"
  done
' 2>&1 || echo "[WARN] mc cleanup failed, buckets may need manual cleanup"

echo "[2/2] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== MinIO reset complete ==="
