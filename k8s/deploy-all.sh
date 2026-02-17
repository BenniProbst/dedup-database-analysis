#!/bin/bash
# =============================================================================
# Deploy all Dedup Experiment Infrastructure
#
# Prerequisites:
#   - kubectl configured with cluster access
#   - Helm 3 installed
#   - Longhorn StorageClass 'longhorn-database' exists
#   - Samba AD lab user 'dedup-lab' exists
#
# Order:
#   1. Namespaces
#   2. RBAC (ServiceAccount, ClusterRole, ClusterRoleBinding)
#   3. Secrets (must fill in real passwords first!)
#   4. MariaDB StatefulSet
#   5. ClickHouse StatefulSet
#   6. Prometheus + Grafana (Helm)
#   7. ConfigMap (experiment config)
#
# Usage: ./k8s/deploy-all.sh
# =============================================================================
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K8S_DIR="${PROJECT_ROOT}/k8s"

echo "=== Dedup Experiment Infrastructure Deploy ==="
echo "Project: ${PROJECT_ROOT}"
echo ""

# Step 1: Namespaces
echo "--- Step 1: Namespaces ---"
kubectl apply -f "${K8S_DIR}/base/namespace.yaml"

# Step 2: RBAC
echo "--- Step 2: RBAC ---"
kubectl apply -f "${K8S_DIR}/base/rbac.yaml"

# Step 3: Secrets (user must have edited secrets.yaml with real values)
echo "--- Step 3: Secrets ---"
echo "WARNING: Ensure k8s/base/secrets.yaml has real passwords!"
echo "Press Ctrl+C within 5s to abort, or wait to continue..."
sleep 5
kubectl apply -f "${K8S_DIR}/base/secrets.yaml"

# Step 4: MariaDB
echo "--- Step 4: MariaDB ---"
kubectl apply -f "${K8S_DIR}/mariadb/statefulset.yaml"
echo "Waiting for MariaDB pod..."
kubectl wait --for=condition=ready pod -l app.kubernetes.io/name=mariadb \
  -n databases --timeout=120s || echo "MariaDB not ready yet (may take longer)"

# Step 5: ClickHouse
echo "--- Step 5: ClickHouse ---"
kubectl apply -f "${K8S_DIR}/clickhouse/statefulset.yaml"
echo "Waiting for ClickHouse pod..."
kubectl wait --for=condition=ready pod -l app.kubernetes.io/name=clickhouse \
  -n databases --timeout=120s || echo "ClickHouse not ready yet (may take longer)"

# Step 6: Prometheus + Grafana
echo "--- Step 6: Prometheus + Grafana ---"
bash "${K8S_DIR}/monitoring/install.sh"

# Step 7: ConfigMap
echo "--- Step 7: Experiment ConfigMap ---"
kubectl apply -f "${K8S_DIR}/base/configmap.yaml"

echo ""
echo "=== Deploy Complete ==="
echo ""
echo "Verify:"
echo "  kubectl get pods -n databases"
echo "  kubectl get pods -n monitoring"
echo ""
echo "Run experiment:"
echo "  kubectl apply -f k8s/jobs/experiment-job.yaml"
echo "  kubectl logs -f job/dedup-experiment -n databases"
echo ""
echo "Cleanup:"
echo "  kubectl apply -f k8s/jobs/cleanup-job.yaml"
