// =============================================================================
// Dedup Integration Test -- TU Dresden Research Project
// "Deduplikation in Datenhaltungssystemen"
// Betreuer: Dr. Alexander Krause
//
// Measures physical storage behavior across database systems under
// controlled duplication workloads on a K8s cluster with Longhorn storage.
//
// SAFETY: Uses SEPARATE lab schemas on production databases.
// Lab schemas are reset after EVERY run. Customer data is NEVER touched.
//
// Hardware: Intel N97 (Alder Lake-N) -- C++ for precision, not Python.
// =============================================================================

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <filesystem>
#include <fstream>

#include "config.hpp"
#include "utils/logger.hpp"
#include "connectors/postgres_connector.hpp"
#include "connectors/redis_connector.hpp"
#include "connectors/kafka_connector.hpp"
#include "connectors/minio_connector.hpp"
#include "experiment/schema_manager.hpp"
#include "experiment/data_loader.hpp"
#include "experiment/metrics_collector.hpp"

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --config PATH       JSON config file (default: use K8s defaults)\n"
        "  --data-dir PATH     Test data directory (default: /tmp/datasets)\n"
        "  --results-dir PATH  Results output directory (default: results/)\n"
        "  --systems LIST      Comma-separated systems to test (default: all)\n"
        "                      Valid: postgresql,cockroachdb,redis,kafka,minio\n"
        "  --grades LIST       Comma-separated grades (default: U0,U50,U90)\n"
        "  --lab-schema NAME   Lab schema name (default: dedup_lab)\n"
        "  --dry-run           Simulate without actual DB operations\n"
        "  --verbose           Enable debug logging\n"
        "  --help              Show this help\n"
        "\n"
        "SAFETY: This program operates on SEPARATE lab schemas.\n"
        "        Production/customer data is NEVER modified.\n"
        "        Lab schemas are RESET after every run.\n",
        prog);
}

int main(int argc, char* argv[]) {
    // Parse arguments
    std::string config_path;
    std::string data_dir = "/tmp/datasets";
    std::string results_dir = "results";
    std::string lab_schema = "dedup_lab";
    std::string systems_filter;
    std::string grades_filter;
    bool dry_run = false;

#ifdef DEDUP_DRY_RUN
    dry_run = true;
#endif

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--results-dir") == 0 && i + 1 < argc) {
            results_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--systems") == 0 && i + 1 < argc) {
            systems_filter = argv[++i];
        } else if (std::strcmp(argv[i], "--grades") == 0 && i + 1 < argc) {
            grades_filter = argv[++i];
        } else if (std::strcmp(argv[i], "--lab-schema") == 0 && i + 1 < argc) {
            lab_schema = argv[++i];
        } else if (std::strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            dedup::g_log_level = dedup::LogLevel::DEBUG;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (dry_run) {
        LOG_INF("=== DRY RUN MODE === (no actual DB operations)");
    }

    // Load configuration
    dedup::ExperimentConfig cfg;
    if (!config_path.empty()) {
        cfg = dedup::ExperimentConfig::from_json(config_path);
    } else {
        cfg = dedup::ExperimentConfig::default_k8s_config();
    }
    cfg.data_dir = data_dir;
    cfg.results_dir = results_dir;
    cfg.lab_schema = lab_schema;
    cfg.dry_run = dry_run;

    // Create results directory
    fs::create_directories(results_dir);

    // Initialize metrics collector
    dedup::MetricsCollector metrics(cfg.prometheus, cfg.grafana);

    // Create connectors for each configured database
    dedup::SchemaManager schema_mgr;
    std::vector<std::shared_ptr<dedup::DbConnector>> connectors;

    for (const auto& db : cfg.databases) {
        // Apply systems filter if specified
        if (!systems_filter.empty() &&
            systems_filter.find(dedup::db_system_str(db.system)) == std::string::npos) {
            continue;
        }

        std::shared_ptr<dedup::DbConnector> conn;
        switch (db.system) {
            case dedup::DbSystem::POSTGRESQL:
                conn = std::make_shared<dedup::PostgresConnector>(dedup::DbSystem::POSTGRESQL);
                break;
            case dedup::DbSystem::COCKROACHDB:
                conn = std::make_shared<dedup::PostgresConnector>(dedup::DbSystem::COCKROACHDB);
                break;
            case dedup::DbSystem::REDIS:
                conn = std::make_shared<dedup::RedisConnector>();
                break;
            case dedup::DbSystem::KAFKA:
                conn = std::make_shared<dedup::KafkaConnector>();
                break;
            case dedup::DbSystem::MINIO:
                conn = std::make_shared<dedup::MinioConnector>();
                break;
        }

        LOG_INF("Connecting to %s at %s:%u...", dedup::db_system_str(db.system),
            db.host.c_str(), db.port);

        if (conn->connect(db)) {
            connectors.push_back(conn);
            schema_mgr.add_connector(conn);
        } else {
            LOG_ERR("Failed to connect to %s -- skipping", dedup::db_system_str(db.system));
        }
    }

    if (connectors.empty()) {
        LOG_ERR("No database connections established. Exiting.");
        return 1;
    }

    LOG_INF("Connected to %zu databases", connectors.size());

    // Parse dup grades
    std::vector<dedup::DupGrade> grades = cfg.dup_grades;
    if (!grades_filter.empty()) {
        grades.clear();
        if (grades_filter.find("U0") != std::string::npos) grades.push_back(dedup::DupGrade::U0);
        if (grades_filter.find("U50") != std::string::npos) grades.push_back(dedup::DupGrade::U50);
        if (grades_filter.find("U90") != std::string::npos) grades.push_back(dedup::DupGrade::U90);
    }

    // Create lab schemas
    LOG_INF("Creating lab schemas (%s) on all connected databases...", lab_schema.c_str());
    schema_mgr.create_all_lab_schemas(lab_schema);

    // Run experiments
    dedup::DataLoader loader(schema_mgr, metrics, cfg.replica_count);
    std::vector<dedup::ExperimentResult> all_results;

    for (auto& conn : connectors) {
        auto results = loader.run_full_experiment(*conn, data_dir, lab_schema, grades);
        all_results.insert(all_results.end(), results.begin(), results.end());
    }

    // Save combined results
    nlohmann::json combined = nlohmann::json::array();
    for (const auto& r : all_results) {
        combined.push_back(r.to_json());
    }

    std::string combined_path = results_dir + "/combined_results.json";
    std::ofstream out(combined_path);
    if (out.is_open()) {
        out << combined.dump(2);
        LOG_INF("Combined results saved to %s", combined_path.c_str());
    }

    // Final cleanup: drop all lab schemas
    LOG_INF("Final cleanup: dropping all lab schemas...");
    schema_mgr.drop_all_lab_schemas(lab_schema);

    // Disconnect all
    for (auto& conn : connectors) {
        conn->disconnect();
    }

    LOG_INF("=== EXPERIMENT COMPLETE ===");
    LOG_INF("Total runs: %zu", all_results.size());
    LOG_INF("Results: %s", combined_path.c_str());

    // Print summary table
    std::printf("\n%-15s %-5s %-15s %12s %12s %8s\n",
        "System", "Grade", "Stage", "Duration(ms)", "Logical(B)", "EDR");
    std::printf("%.70s\n", "----------------------------------------------------------------------");
    for (const auto& r : all_results) {
        std::printf("%-15s %-5s %-15s %12lld %12lld %8.3f\n",
            r.system.c_str(), r.dup_grade.c_str(), r.stage.c_str(),
            r.duration_ns / 1000000, r.bytes_logical, r.edr);
    }

    return 0;
}
