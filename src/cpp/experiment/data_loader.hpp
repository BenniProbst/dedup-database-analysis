#pragma once
// Orchestrates the experiment stages as defined in the research paper:
// Stage 1: Bulk Insert
// Stage 2: Per-File Insert
// Stage 3: Per-File Delete + Maintenance (Reclamation)
#include <string>
#include <nlohmann/json.hpp>
#include "../connectors/db_connector.hpp"
#include "native_record.hpp"
#include "native_data_parser.hpp"
#include "metrics_collector.hpp"
#include "schema_manager.hpp"

namespace dedup {

// Result of a full experiment run (one system, one payload type, one dup grade, one stage)
struct ExperimentResult {
    std::string system;
    std::string payload_type;       // Payload type (doku.tex §6.3)
    std::string dup_grade;
    std::string stage;
    int64_t duration_ns = 0;
    int64_t rows_affected = 0;
    int64_t bytes_logical = 0;          // Logical data as reported by connector
    int64_t logical_size_before = 0;    // DB-reported logical size before stage
    int64_t logical_size_after = 0;     // DB-reported logical size after stage
    int64_t phys_size_before = 0;       // Longhorn actual_size_bytes before stage
    int64_t phys_size_after = 0;        // Longhorn actual_size_bytes after stage
    int64_t phys_delta = 0;             // phys_size_after - phys_size_before
    double edr = 0.0;                   // Effective Deduplication Ratio
    double throughput_bytes_per_sec = 0; // Ingest throughput (doku.tex 5.4.1)
    int replica_count = 0;
    std::string volume_name;            // Longhorn volume name used for measurement
    std::string timestamp;
    std::string error;
    std::string insertion_mode = "blob";  // "blob" or "native"

    // DB-internal instrumentation snapshots (doku.tex §6.5)
    // Captured at stage boundaries when db_internal_metrics is enabled.
    nlohmann::json db_internal_before;
    nlohmann::json db_internal_after;

    // Per-file latency stats from perfile_insert / perfile_delete (doku.tex Stage 2/3)
    int64_t latency_count = 0;
    int64_t latency_min_ns = 0;
    int64_t latency_max_ns = 0;
    int64_t latency_p50_ns = 0;
    int64_t latency_p95_ns = 0;
    int64_t latency_p99_ns = 0;
    double  latency_mean_ns = 0.0;

    nlohmann::json to_json() const;
};

class DataLoader {
public:
    DataLoader(SchemaManager& schema_mgr, MetricsCollector& metrics,
               int replica_count, bool db_internal_metrics = true)
        : schema_mgr_(schema_mgr), metrics_(metrics),
          replica_count_(replica_count), db_internal_metrics_(db_internal_metrics) {}

    // Run a full experiment: all stages, all dup grades, for one connector and payload type
    std::vector<ExperimentResult> run_full_experiment(
        DbConnector& connector,
        const DbConnection& db_conn,
        const std::string& data_dir,
        const std::string& lab_schema,
        const std::vector<DupGrade>& grades,
        PayloadType payload_type = PayloadType::MIXED);

    // Run a single stage for one connector, payload type, and dup grade
    ExperimentResult run_stage(
        DbConnector& connector,
        const DbConnection& db_conn,
        Stage stage,
        DupGrade grade,
        const std::string& data_dir,
        const std::string& lab_schema,
        PayloadType payload_type = PayloadType::MIXED);


    // Native insertion mode (Stage 1, doku.tex 5.4)
    // Uses parsed NativeRecords instead of raw files
    ExperimentResult run_native_stage(
        DbConnector& connector,
        const DbConnection& db_conn,
        Stage stage,
        DupGrade grade,
        const std::vector<NativeRecord>& records,
        const std::string& lab_schema,
        PayloadType payload_type);

    std::vector<ExperimentResult> run_native_experiment(
        DbConnector& connector,
        const DbConnection& db_conn,
        const std::string& data_dir,
        const std::string& lab_schema,
        const std::vector<DupGrade>& grades,
        PayloadType payload_type);

private:
    SchemaManager& schema_mgr_;
    MetricsCollector& metrics_;
    int replica_count_;
    bool db_internal_metrics_;

    std::string current_timestamp();
};

} // namespace dedup
