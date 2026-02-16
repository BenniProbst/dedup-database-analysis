#pragma once
// Orchestrates the experiment stages as defined in the research paper:
// Stage 1: Bulk Insert
// Stage 2: Per-File Insert
// Stage 3: Per-File Delete + Maintenance (Reclamation)
#include <string>
#include <nlohmann/json.hpp>
#include "../connectors/db_connector.hpp"
#include "metrics_collector.hpp"
#include "schema_manager.hpp"

namespace dedup {

// Result of a full experiment run (one system, one dup grade, one stage)
struct ExperimentResult {
    std::string system;
    std::string dup_grade;
    std::string stage;
    int64_t duration_ns;
    int64_t rows_affected;
    int64_t bytes_logical;
    int64_t phys_size_before;
    int64_t phys_size_after;
    int64_t phys_delta;
    double edr;
    int replica_count;
    std::string timestamp;
    std::string error;

    nlohmann::json to_json() const;
};

class DataLoader {
public:
    DataLoader(SchemaManager& schema_mgr, MetricsCollector& metrics, int replica_count)
        : schema_mgr_(schema_mgr), metrics_(metrics), replica_count_(replica_count) {}

    // Run a full experiment: all stages, all dup grades, for one connector
    std::vector<ExperimentResult> run_full_experiment(
        DbConnector& connector,
        const std::string& data_dir,
        const std::string& lab_schema,
        const std::vector<DupGrade>& grades);

    // Run a single stage for one connector and dup grade
    ExperimentResult run_stage(
        DbConnector& connector,
        Stage stage,
        DupGrade grade,
        const std::string& data_dir,
        const std::string& lab_schema);

private:
    SchemaManager& schema_mgr_;
    MetricsCollector& metrics_;
    int replica_count_;

    std::string current_timestamp();
};

} // namespace dedup
