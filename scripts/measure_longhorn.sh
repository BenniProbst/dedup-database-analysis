#!/usr/bin/env bash
# =============================================================================
# measure_longhorn.sh - Longhorn Volume Metrics Collector
# =============================================================================
# Sammelt physische Speichermetriken fuer alle PVCs im dedup-research Namespace
#
# Usage: ./measure_longhorn.sh [namespace] [prometheus_url]
# =============================================================================

set -euo pipefail

NAMESPACE="${1:-dedup-research}"
PROM_URL="${2:-http://prometheus.monitoring.svc.cluster.local:9090}"
OUTPUT_DIR="${3:-results/longhorn-metrics}"
TIMESTAMP=$(date -Iseconds)

mkdir -p "${OUTPUT_DIR}"

echo "=== Longhorn Volume Metrics Collector ==="
echo "Namespace: ${NAMESPACE}"
echo "Prometheus: ${PROM_URL}"
echo "Timestamp: ${TIMESTAMP}"
echo ""

# Alle PVCs im Namespace auflisten
PVCS=$(kubectl get pvc -n "${NAMESPACE}" -o jsonpath='{range .items[*]}{.metadata.name}={.spec.volumeName}{"\n"}{end}')

if [ -z "${PVCS}" ]; then
    echo "No PVCs found in namespace ${NAMESPACE}"
    exit 0
fi

echo "PVC,Volume,ActualSize,LogicalSize,ReplicaCount" > "${OUTPUT_DIR}/metrics_${TIMESTAMP}.csv"

while IFS='=' read -r PVC_NAME VOLUME_NAME; do
    [ -z "${PVC_NAME}" ] && continue

    # Actual size (physisch genutzter Speicher)
    ACTUAL=$(curl -s "${PROM_URL}/api/v1/query?query=longhorn_volume_actual_size_bytes{volume=\"${VOLUME_NAME}\"}" \
        | jq -r '.data.result[0].value[1] // "0"')

    # Capacity
    CAPACITY=$(curl -s "${PROM_URL}/api/v1/query?query=longhorn_volume_capacity_bytes{volume=\"${VOLUME_NAME}\"}" \
        | jq -r '.data.result[0].value[1] // "0"')

    # Replica count
    REPLICAS=$(curl -s "${PROM_URL}/api/v1/query?query=longhorn_volume_robustness{volume=\"${VOLUME_NAME}\"}" \
        | jq -r '.data.result | length // 0')

    echo "${PVC_NAME},${VOLUME_NAME},${ACTUAL},${CAPACITY},${REPLICAS}" >> "${OUTPUT_DIR}/metrics_${TIMESTAMP}.csv"

    # Menschenlesbare Ausgabe
    ACTUAL_MB=$(echo "scale=2; ${ACTUAL}/1048576" | bc 2>/dev/null || echo "N/A")
    echo "  ${PVC_NAME}: ${ACTUAL_MB} MiB actual (volume: ${VOLUME_NAME})"

done <<< "${PVCS}"

echo ""
echo "Metrics saved to: ${OUTPUT_DIR}/metrics_${TIMESTAMP}.csv"
