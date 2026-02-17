#pragma once
// Collects Longhorn physical storage metrics via Prometheus API
// and pushes experiment metrics to Grafana
#include <string>
#include <cstdint>
#include "../config.hpp"

namespace dedup {

class MetricsCollector {
public:
    explicit MetricsCollector(const PrometheusConfig& prom, const GrafanaConfig& grafana = {})
        : prometheus_(prom), grafana_(grafana) {}

    // Query Longhorn physical storage size for a PVC volume
    // Returns bytes, or -1 on error
    int64_t get_longhorn_actual_size(const std::string& volume_name);

    // Query Longhorn volume name for a PVC
    std::string get_volume_for_pvc(const std::string& pvc_name, const std::string& ns);

    // Push a metric data point to Grafana (via Prometheus pushgateway or direct)
    bool push_metric(const std::string& metric_name, double value,
                     const std::string& db_system, const std::string& dup_grade,
                     const std::string& stage);

    // Query MinIO physical bucket size via MinIO Prometheus endpoint
    // Used when MinIO has no Longhorn PVC (Direct Disk)
    // Returns total bytes for all buckets matching prefix, or -1 on error
    int64_t get_minio_physical_size(const std::string& minio_endpoint,
                                     const std::string& bucket_prefix = "dedup-lab");

    // Calculate EDR: EDR = B_logical / (B_phys / N)
    static double calculate_edr(int64_t logical_bytes, int64_t phys_delta_bytes, int replica_count) {
        if (phys_delta_bytes <= 0 || replica_count <= 0) return 0.0;
        return static_cast<double>(logical_bytes) / (static_cast<double>(phys_delta_bytes) / replica_count);
    }

private:
    PrometheusConfig prometheus_;
    GrafanaConfig grafana_;

    // HTTP GET to Prometheus query API
    std::string prometheus_query(const std::string& query);
};

} // namespace dedup
