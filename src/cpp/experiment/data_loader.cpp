#include "data_loader.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include <chrono>
#include <ctime>
#include <thread>
#include <fstream>

namespace dedup {

nlohmann::json ExperimentResult::to_json() const {
    return {
        {"system", system},
        {"dup_grade", dup_grade},
        {"stage", stage},
        {"duration_ns", duration_ns},
        {"duration_ms", duration_ns / 1000000},
        {"rows_affected", rows_affected},
        {"bytes_logical", bytes_logical},
        {"phys_size_before", phys_size_before},
        {"phys_size_after", phys_size_after},
        {"phys_delta", phys_delta},
        {"edr", edr},
        {"replica_count", replica_count},
        {"timestamp", timestamp},
        {"error", error}
    };
}

std::string DataLoader::current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

ExperimentResult DataLoader::run_stage(
    DbConnector& connector,
    Stage stage,
    DupGrade grade,
    const std::string& data_dir,
    const std::string& lab_schema) {

    ExperimentResult result;
    result.system = connector.system_name();
    result.dup_grade = dup_grade_str(grade);
    result.stage = stage_str(stage);
    result.replica_count = replica_count_;
    result.timestamp = current_timestamp();

    LOG_INF("=== %s / %s / %s ===", result.system.c_str(), result.dup_grade.c_str(), result.stage.c_str());

    // Measure Longhorn BEFORE
    // For now, get logical size from the connector
    result.phys_size_before = connector.get_logical_size_bytes();

    // Execute the stage
    MeasureResult mr{};
    switch (stage) {
        case Stage::BULK_INSERT:
            mr = connector.bulk_insert(data_dir, grade);
            break;
        case Stage::PERFILE_INSERT:
            mr = connector.perfile_insert(data_dir, grade);
            break;
        case Stage::PERFILE_DELETE:
            mr = connector.perfile_delete();
            break;
        case Stage::MAINTENANCE:
            mr = connector.run_maintenance();
            break;
    }

    result.duration_ns = mr.duration_ns;
    result.rows_affected = mr.rows_affected;
    result.bytes_logical = mr.bytes_logical;
    result.error = mr.error;

    // Wait for storage metrics to settle (Longhorn propagation)
    LOG_DBG("Waiting 10s for Longhorn metrics to settle...");
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // Measure Longhorn AFTER
    result.phys_size_after = connector.get_logical_size_bytes();
    result.phys_delta = result.phys_size_after - result.phys_size_before;

    // Calculate EDR
    if (result.bytes_logical > 0 && result.phys_delta > 0) {
        result.edr = MetricsCollector::calculate_edr(
            result.bytes_logical, result.phys_delta, replica_count_);
    }

    // Push to Grafana
    metrics_.push_metric("dedup_duration_ms",
        static_cast<double>(result.duration_ns) / 1e6,
        result.system, result.dup_grade, result.stage);
    metrics_.push_metric("dedup_edr", result.edr,
        result.system, result.dup_grade, result.stage);
    metrics_.push_metric("dedup_phys_delta_bytes",
        static_cast<double>(result.phys_delta),
        result.system, result.dup_grade, result.stage);

    LOG_INF("Result: %lld rows, %lld logical bytes, %lld ms, EDR=%.3f",
        result.rows_affected, result.bytes_logical,
        result.duration_ns / 1000000, result.edr);

    return result;
}

std::vector<ExperimentResult> DataLoader::run_full_experiment(
    DbConnector& connector,
    const std::string& data_dir,
    const std::string& lab_schema,
    const std::vector<DupGrade>& grades) {

    std::vector<ExperimentResult> results;

    LOG_INF("=== FULL EXPERIMENT: %s ===", connector.system_name());

    for (auto grade : grades) {
        LOG_INF("--- Grade: %s ---", dup_grade_str(grade));

        // Reset lab schema before each grade
        connector.reset_lab_schema(lab_schema);

        // Stage 1: Bulk Insert
        results.push_back(run_stage(connector, Stage::BULK_INSERT, grade, data_dir, lab_schema));

        // Reset for Stage 2
        connector.reset_lab_schema(lab_schema);

        // Stage 2: Per-File Insert
        results.push_back(run_stage(connector, Stage::PERFILE_INSERT, grade, data_dir, lab_schema));

        // Stage 3a: Per-File Delete
        results.push_back(run_stage(connector, Stage::PERFILE_DELETE, grade, data_dir, lab_schema));

        // Stage 3b: Maintenance
        results.push_back(run_stage(connector, Stage::MAINTENANCE, grade, data_dir, lab_schema));

        // Reset after each grade run
        connector.reset_lab_schema(lab_schema);
        LOG_INF("Lab schema reset after %s run", dup_grade_str(grade));
    }

    // Save results to JSON
    nlohmann::json j_results = nlohmann::json::array();
    for (const auto& r : results) {
        j_results.push_back(r.to_json());
    }

    std::string outpath = "results/" + std::string(connector.system_name()) + "_results.json";
    std::ofstream out(outpath);
    if (out.is_open()) {
        out << j_results.dump(2);
        LOG_INF("Results saved to %s", outpath.c_str());
    }

    return results;
}

} // namespace dedup
