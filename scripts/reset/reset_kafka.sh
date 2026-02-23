#!/bin/bash
# Reset Kafka experiment data (dedup-lab-* topics)
# Topics are managed by Strimzi KafkaTopic CRDs — do NOT delete/recreate them.
# Instead, purge data via retention trick (set retention.ms=1, wait, restore).
# ACLs are managed by KafkaUser CRD (dedup-lab, SCRAM-SHA-512).
# Usage: ./reset_kafka.sh [--dry-run]
set -euo pipefail
DRY_RUN="${1:-}"
NFS_CHECKPOINT="/datasets/real-world/checkpoints/kafka"
TOPICS="dedup-lab-metrics dedup-lab-events dedup-lab-u0 dedup-lab-u50 dedup-lab-u90"
KAFKA_BIN="/opt/kafka/bin"

echo "=== Reset Kafka (dedup-lab-* topics — purge data only) ==="

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY-RUN] Would purge data from topics: $TOPICS"
  echo "[DRY-RUN] Would clear NFS checkpoints at $NFS_CHECKPOINT"
  exit 0
fi

# Strimzi broker pod label (not the old kafka-cluster-kafka label)
KAFKA_POD=$(kubectl -n kafka get pod -l strimzi.io/name=kafka-cluster-broker -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -z "$KAFKA_POD" ]; then
  echo "[ERROR] No Kafka broker pod found (label: strimzi.io/name=kafka-cluster-broker)"
  exit 1
fi

echo "Using pod: $KAFKA_POD"

echo "[1/3] Purging data from experiment topics (retention trick)..."
for TOPIC in $TOPICS; do
  kubectl -n kafka exec "$KAFKA_POD" -- "$KAFKA_BIN/kafka-configs.sh" \
    --bootstrap-server localhost:9092 \
    --alter --entity-type topics --entity-name "$TOPIC" \
    --add-config retention.ms=1 2>/dev/null && echo "  $TOPIC: retention.ms=1" || echo "  $TOPIC: skip (not found)"
done

echo "  Waiting 10s for log compaction..."
sleep 10

echo "[2/3] Restoring default retention..."
for TOPIC in $TOPICS; do
  kubectl -n kafka exec "$KAFKA_POD" -- "$KAFKA_BIN/kafka-configs.sh" \
    --bootstrap-server localhost:9092 \
    --alter --entity-type topics --entity-name "$TOPIC" \
    --delete-config retention.ms 2>/dev/null && echo "  $TOPIC: retention restored" || echo "  $TOPIC: skip"
done

echo "  Resetting consumer group offsets..."
kubectl -n kafka exec "$KAFKA_POD" -- "$KAFKA_BIN/kafka-consumer-groups.sh" \
  --bootstrap-server localhost:9092 \
  --group dedup-exporter --reset-offsets --all-topics --to-earliest --execute 2>/dev/null || true

echo "[3/3] Clearing NFS checkpoints..."
RUNNER4_POD=$(kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
if [ -n "$RUNNER4_POD" ]; then
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- rm -rf "$NFS_CHECKPOINT" 2>/dev/null || true
  kubectl -n gitlab-runner exec "$RUNNER4_POD" -- mkdir -p "$NFS_CHECKPOINT" 2>/dev/null || true
fi

echo "=== Kafka reset complete ==="
