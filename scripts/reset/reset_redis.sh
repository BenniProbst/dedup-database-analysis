#!/bin/bash
# Reset Redis experiment data (dedup:* keys)
set -euo pipefail
DRY_RUN="${1:-}"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/redis"

echo "=== Reset Redis (dedup:* keys) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would FLUSHDB on dedup:* keys"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

REDIS_POD=$(kubectl -n redis get pod -l app.kubernetes.io/name=redis-cluster -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$REDIS_POD" ]; then
  echo "[ERROR] No Redis pod found"
  exit 1
fi

echo "[1/2] Deleting dedup-lab:* keys..."
# Use SCAN + DEL to safely remove experiment keys (cluster mode safe)
COUNT=$(kubectl -n redis exec "$REDIS_POD" -- redis-cli --scan --pattern 'dedup-lab:*' 2>/dev/null | wc -l)
if [ "$COUNT" -gt 0 ]; then
  kubectl -n redis exec "$REDIS_POD" -- sh -c 'redis-cli --scan --pattern "dedup-lab:*" | xargs -r redis-cli DEL' 2>/dev/null || true
  echo "  Deleted $COUNT keys"
else
  echo "  No dedup-lab:* keys found"
fi

echo "[2/2] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== Redis reset complete ==="
