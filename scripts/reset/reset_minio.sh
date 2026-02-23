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

MINIO_POD=$(kubectl -n minio get pod -l app=minio -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$MINIO_POD" ]; then
  echo "[ERROR] No MinIO pod found (label: app=minio)"
  exit 1
fi

echo "[1/2] Clearing dedup-lab-* buckets..."
# mc alias 'local' is pre-configured in the MinIO pod
BUCKETS=$(kubectl -n minio exec "$MINIO_POD" -- mc ls local/ 2>/dev/null | grep "dedup-lab" | awk '{print $NF}') || true
if [ -n "$BUCKETS" ]; then
  for bucket in $BUCKETS; do
    kubectl -n minio exec "$MINIO_POD" -- mc rm --recursive --force "local/$bucket" 2>/dev/null || true
    echo "  Cleared: $bucket"
  done
else
  echo "  No dedup-lab-* buckets found"
fi

echo "[2/2] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== MinIO reset complete ==="
