// =============================================================================
// MetricsTrace -- Background thread sampling ALL DB metrics at 100ms (10 Hz)
//
// Dual Kafka output:
//   dedup-lab-metrics  -- per-DB system metric snapshots (100ms)
//   dedup-lab-events   -- experiment stage events (start/stop/error)
//
// Both topics run under the lab user. ALL data (test data + metrics) is
// deleted together after export. Metrics are exported to CSV before cleanup.
//
// Grafana integration via Prometheus Pushgateway (when available).
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../config.hpp"
#include "../connectors/db_connector.hpp"

namespace dedup {

// Current time in milliseconds since epoch (used for event timestamps)
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// A single metric data point (produced to Kafka as JSON)
struct MetricPoint {
    int64_t timestamp_ms;       // Unix epoch milliseconds
    std::string system;         // e.g. "postgresql"
    std::string metric_name;    // e.g. "pg_database_size"
    double value;
    std::string unit;           // e.g. "bytes", "count", "ms"

    std::string to_json() const;
};

// An experiment event (produced to dedup-lab-events)
struct ExperimentEvent {
    int64_t timestamp_ms;
    std::string event_type;     // "stage_start", "stage_end", "error", "experiment_start", "experiment_end"
    std::string system;
    std::string payload_type;   // Payload type (doku.tex §6.3)
    std::string dup_grade;
    std::string stage;
    std::string detail;         // JSON payload with extra info

    std::string to_json() const;
};

// Collector function: queries one DB and returns metric points
using MetricCollectorFn = std::function<std::vector<MetricPoint>(const DbConnection& conn)>;

class MetricsTrace {
public:
    MetricsTrace(const MetricsTraceConfig& config, bool dry_run = false);
    ~MetricsTrace();

    // Register a DB system for metric collection
    // The collector_fn is called every sample_interval_ms in the background thread
    void register_system(const DbConnection& conn, MetricCollectorFn collector_fn);

    // Start/stop the background sampling thread
    void start();
    void stop();

    // Publish an experiment event (can be called from main thread)
    void publish_event(const ExperimentEvent& event);

    // Get total metrics published
    int64_t metrics_published() const { return metrics_count_.load(); }
    int64_t events_published() const { return events_count_.load(); }

    bool is_running() const { return running_.load(); }

private:
    void sampling_loop();
    void produce_to_kafka(const std::string& topic, const std::string& key,
                          const std::string& payload);

    MetricsTraceConfig config_;
    bool dry_run_;

    struct SystemEntry {
        DbConnection conn;
        MetricCollectorFn collector;
    };
    std::vector<SystemEntry> systems_;
    std::mutex systems_mutex_;

    std::thread sampling_thread_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> metrics_count_{0};
    std::atomic<int64_t> events_count_{0};

    // Kafka producer handle (opaque, cast to rd_kafka_t* in .cpp)
    void* kafka_producer_ = nullptr;
};

// Built-in metric collectors for each DB system
namespace collectors {
    std::vector<MetricPoint> collect_postgresql(const DbConnection& conn);
    std::vector<MetricPoint> collect_cockroachdb(const DbConnection& conn);
    std::vector<MetricPoint> collect_redis(const DbConnection& conn);
    std::vector<MetricPoint> collect_kafka(const DbConnection& conn);
    std::vector<MetricPoint> collect_minio(const DbConnection& conn);
    std::vector<MetricPoint> collect_mariadb(const DbConnection& conn);
    std::vector<MetricPoint> collect_clickhouse(const DbConnection& conn);
#ifdef HAS_COMDARE_DB
    std::vector<MetricPoint> collect_comdare_db(const DbConnection& conn);
#endif

    // Returns the right collector for a DbSystem enum
    MetricCollectorFn for_system(DbSystem system);
}

} // namespace dedup
