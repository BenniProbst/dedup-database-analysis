#!/bin/bash
# Reset Kafka experiment data (dedup-lab-* topics)
set -euo pipefail
DRY_RUN="${1:-}"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/kafka"
TOPICS="dedup-lab-metrics dedup-lab-events dedup-lab-u0 dedup-lab-u50 dedup-lab-u90"

echo "=== Reset Kafka (dedup-lab-* topics) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would delete and recreate topics: $TOPICS"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

KAFKA_POD=$(kubectl -n kafka get pod -l strimzi.io/name=kafka-cluster-kafka -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$KAFKA_POD" ]; then
  echo "[ERROR] No Kafka pod found"
  exit 1
fi

echo "[1/3] Deleting experiment topics..."
for TOPIC in $TOPICS; do
  kubectl -n kafka exec "$KAFKA_POD" -- bin/kafka-topics.sh --bootstrap-server localhost:9092 --delete --topic "$TOPIC" 2>/dev/null || true
  echo "  Deleted: $TOPIC"
done

sleep 2

echo "[2/3] Recreating experiment topics..."
for TOPIC in $TOPICS; do
  kubectl -n kafka exec "$KAFKA_POD" -- bin/kafka-topics.sh --bootstrap-server localhost:9092 --create --topic "$TOPIC" --partitions 1 --replication-factor 1 2>/dev/null || true
  echo "  Created: $TOPIC"
done

# Re-add ACLs
echo "  Setting ACLs..."
for TOPIC in $TOPICS; do
  kubectl -n kafka exec "$KAFKA_POD" -- bin/kafka-acls.sh --bootstrap-server localhost:9092 --add --allow-principal "User:ANONYMOUS" --operation All --topic "$TOPIC" 2>/dev/null || true
done

echo "[3/3] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== Kafka reset complete ==="
