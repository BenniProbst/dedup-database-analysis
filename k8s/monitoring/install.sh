#!/bin/bash
# =============================================================================
# Install kube-prometheus-stack for Dedup Experiment
# Run from the project root directory
# =============================================================================
set -euo pipefail

NAMESPACE="monitoring"
RELEASE="kube-prometheus-stack"
CHART="prometheus-community/kube-prometheus-stack"

echo "=== Installing kube-prometheus-stack for Dedup Experiment ==="

# Add Helm repo
helm repo add prometheus-community https://prometheus-community.github.io/helm-charts 2>/dev/null || true
helm repo update

# Create namespace
kubectl create namespace "${NAMESPACE}" --dry-run=client -o yaml | kubectl apply -f -

# Create Grafana dashboard ConfigMap from actual JSON
echo "Creating Grafana dashboard ConfigMap..."
kubectl create configmap dedup-grafana-dashboard \
  --from-file=dedup-experiment.json=results/grafana-dashboard.json \
  -n "${NAMESPACE}" \
  --dry-run=client -o yaml | kubectl apply -f -

# Install/upgrade Helm release
echo "Installing ${RELEASE}..."
helm upgrade --install "${RELEASE}" "${CHART}" \
  -n "${NAMESPACE}" \
  -f k8s/monitoring/values.yaml \
  --wait --timeout 10m

echo ""
echo "=== Installed ==="
echo "Prometheus: http://kube-prometheus-stack-prometheus.${NAMESPACE}.svc.cluster.local:9090"
echo "Grafana:    http://kube-prometheus-stack-grafana.${NAMESPACE}.svc.cluster.local:80"
echo "Pushgateway: http://kube-prometheus-stack-pushgateway.${NAMESPACE}.svc.cluster.local:9091"
echo ""
echo "Grafana admin password: dedup-research-2026"
echo "Dashboard: Dedup Research > Dedup Database Analysis"
