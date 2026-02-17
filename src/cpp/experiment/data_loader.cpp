#include "data_loader.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include <chrono>
#include <ctime>
#include <thread>
#include <fstream>

namespace dedup {

nlohmann::json ExperimentResult::to_json() const {
    nlohmann::json j = {
        {"system", system},
        {"dup_grade", dup_grade},
        {"stage", stage},
        {"duration_ns", duration_ns},
        {"duration_ms", duration_ns / 1000000},
        {"rows_affected", rows_affected},
        {"bytes_logical", bytes_logical},
        {"logical_size_before", logical_size_before},
        {"logical_size_after", logical_size_after},
        {"phys_size_before", phys_size_before},
        {"phys_size_after", phys_size_after},
        {"phys_delta", phys_delta},
        {"edr", edr},
        {"throughput_bytes_per_sec", throughput_bytes_per_sec},
        {"replica_count", replica_count},
        {"volume_name", volume_name},
        {"timestamp", timestamp},
        {"error", error}
    };

    // Per-file latency stats (populated for perfile_insert / perfile_delete stages)
    if (latency_count > 0) {
        j["latency"] = {
            {"count", latency_count},
            {"min_ns", latency_min_ns},
            {"max_ns", latency_max_ns},
            {"p50_ns", latency_p50_ns},
            {"p95_ns", latency_p95_ns},
            {"p99_ns", latency_p99_ns},
            {"mean_ns", latency_mean_ns},
            {"mean_us", latency_mean_ns / 1000.0},
            {"min_us", latency_min_ns / 1000.0},
            {"max_us", latency_max_ns / 1000.0},
            {"p50_us", latency_p50_ns / 1000.0},
            {"p95_us", latency_p95_ns / 1000.0},
            {"p99_us", latency_p99_ns / 1000.0}
        };
    }

    return j;
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
    const DbConnection& db_conn,
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

    // Resolve Longhorn volume name from PVC (once per stage)
    std::string volume_name;
    if (!db_conn.pvc_name.empty()) {
        volume_name = metrics_.get_volume_for_pvc(db_conn.pvc_name, db_conn.k8s_namespace);
        if (volume_name.empty()) {
            LOG_WRN("Could not resolve PVC %s/%s to Longhorn volume -- falling back to logical size",
                db_conn.k8s_namespace.c_str(), db_conn.pvc_name.c_str());
        } else {
            LOG_INF("PVC %s -> Longhorn volume %s", db_conn.pvc_name.c_str(), volume_name.c_str());
        }
    }
    result.volume_name = volume_name;

    // ---- BEFORE measurements ----
    // Logical size: reported by the database connector itself
    result.logical_size_before = connector.get_logical_size_bytes();

    // Physical size: Longhorn actual_size_bytes via Prometheus (doku.tex 5.1)
    // MinIO special case: Direct Disk, no Longhorn PVC -- use MinIO metrics endpoint
    if (!volume_name.empty()) {
        result.phys_size_before = metrics_.get_longhorn_actual_size(volume_name);
        LOG_INF("Longhorn BEFORE: %lld bytes (volume %s)",
            result.phys_size_before, volume_name.c_str());
    } else if (connector.system() == DbSystem::MINIO) {
        std::string minio_url = "http://" + db_conn.host + ":" + std::to_string(db_conn.port);
        result.phys_size_before = metrics_.get_minio_physical_size(minio_url, db_conn.lab_schema);
        LOG_INF("MinIO BEFORE: %lld bytes (via Prometheus endpoint)", result.phys_size_before);
    } else {
        // Fallback: use connector logical size as proxy
        result.phys_size_before = result.logical_size_before;
    }

    // ---- Execute the stage ----
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

    // Compute per-file latency statistics (percentiles, min, max, mean)
    if (!mr.per_file_latencies_ns.empty()) {
        auto& lats = mr.per_file_latencies_ns;
        std::sort(lats.begin(), lats.end());
        result.latency_count = static_cast<int64_t>(lats.size());
        result.latency_min_ns = lats.front();
        result.latency_max_ns = lats.back();
        result.latency_mean_ns = static_cast<double>(
            std::accumulate(lats.begin(), lats.end(), int64_t{0})) / static_cast<double>(lats.size());
        auto pctl = [&](double p) -> int64_t {
            size_t idx = static_cast<size_t>(p * static_cast<double>(lats.size() - 1));
            return lats[idx];
        };
        result.latency_p50_ns = pctl(0.50);
        result.latency_p95_ns = pctl(0.95);
        result.latency_p99_ns = pctl(0.99);

        LOG_INF("Latency stats: n=%lld, p50=%lld us, p95=%lld us, p99=%lld us, min=%lld us, max=%lld us",
            result.latency_count, result.latency_p50_ns / 1000, result.latency_p95_ns / 1000,
            result.latency_p99_ns / 1000, result.latency_min_ns / 1000, result.latency_max_ns / 1000);
    }

    // Compute ingest throughput (doku.tex 5.4.1: bytes/s)
    if (result.duration_ns > 0 && result.bytes_logical > 0) {
        double seconds = static_cast<double>(result.duration_ns) / 1e9;
        result.throughput_bytes_per_sec = static_cast<double>(result.bytes_logical) / seconds;
    }

    // Wait for Longhorn metrics to propagate (storage-level async replication)
    LOG_INF("Waiting 15s for Longhorn metrics to settle...");
    std::this_thread::sleep_for(std::chrono::seconds(15));

    // ---- AFTER measurements ----
    result.logical_size_after = connector.get_logical_size_bytes();

    if (!volume_name.empty()) {
        result.phys_size_after = metrics_.get_longhorn_actual_size(volume_name);
        LOG_INF("Longhorn AFTER: %lld bytes (volume %s)",
            result.phys_size_after, volume_name.c_str());
    } else if (connector.system() == DbSystem::MINIO) {
        std::string minio_url = "http://" + db_conn.host + ":" + std::to_string(db_conn.port);
        result.phys_size_after = metrics_.get_minio_physical_size(minio_url, db_conn.lab_schema);
        LOG_INF("MinIO AFTER: %lld bytes (via Prometheus endpoint)", result.phys_size_after);
    } else {
        result.phys_size_after = result.logical_size_after;
    }

    result.phys_delta = result.phys_size_after - result.phys_size_before;

    // Calculate EDR: B_logical / (B_phys / N), doku.tex 5.4.2
    if (result.bytes_logical > 0 && result.phys_delta > 0) {
        result.edr = MetricsCollector::calculate_edr(
            result.bytes_logical, result.phys_delta, replica_count_);
    }

    // Push metrics to Grafana
    metrics_.push_metric("dedup_duration_ms",
        static_cast<double>(result.duration_ns) / 1e6,
        result.system, result.dup_grade, result.stage);
    metrics_.push_metric("dedup_edr", result.edr,
        result.system, result.dup_grade, result.stage);
    metrics_.push_metric("dedup_phys_delta_bytes",
        static_cast<double>(result.phys_delta),
        result.system, result.dup_grade, result.stage);
    metrics_.push_metric("dedup_throughput_bps", result.throughput_bytes_per_sec,
        result.system, result.dup_grade, result.stage);

    LOG_INF("Result: %lld rows, %lld logical bytes, phys_delta=%lld, %lld ms, EDR=%.3f, %.1f MB/s",
        result.rows_affected, result.bytes_logical, result.phys_delta,
        result.duration_ns / 1000000, result.edr,
        result.throughput_bytes_per_sec / (1024.0 * 1024.0));

    return result;
}

std::vector<ExperimentResult> DataLoader::run_full_experiment(
    DbConnector& connector,
    const DbConnection& db_conn,
    const std::string& data_dir,
    const std::string& lab_schema,
    const std::vector<DupGrade>& grades) {

    std::vector<ExperimentResult> results;

    LOG_INF("=== FULL EXPERIMENT: %s ===", connector.system_name());
    LOG_INF("PVC: %s (namespace: %s)", db_conn.pvc_name.c_str(), db_conn.k8s_namespace.c_str());

    for (auto grade : grades) {
        LOG_INF("--- Grade: %s ---", dup_grade_str(grade));

        // Reset lab schema before each grade
        connector.reset_lab_schema(lab_schema);

        // Stage 1: Bulk Insert
        results.push_back(run_stage(connector, db_conn, Stage::BULK_INSERT, grade, data_dir, lab_schema));

        // Reset for Stage 2
        connector.reset_lab_schema(lab_schema);

        // Stage 2: Per-File Insert
        results.push_back(run_stage(connector, db_conn, Stage::PERFILE_INSERT, grade, data_dir, lab_schema));

        // Stage 3a: Per-File Delete
        results.push_back(run_stage(connector, db_conn, Stage::PERFILE_DELETE, grade, data_dir, lab_schema));

        // Stage 3b: Maintenance (VACUUM FULL / compaction / retention)
        results.push_back(run_stage(connector, db_conn, Stage::MAINTENANCE, grade, data_dir, lab_schema));

        // Reset after each grade run
        connector.reset_lab_schema(lab_schema);
        LOG_INF("Lab schema reset after %s run", dup_grade_str(grade));
    }

    // Save per-system results to JSON
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
