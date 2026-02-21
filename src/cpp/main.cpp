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
//
// Systems under test (doku.tex Chapter 5):
//   PostgreSQL, CockroachDB, Redis, Kafka, MinIO,
//   MariaDB (TODO: cluster install), ClickHouse (TODO: cluster install),
//   comdare-DB (TODO: cluster install)
// =============================================================================

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

#include "config.hpp"
#include "utils/logger.hpp"
#include "utils/sha256.hpp"
#include "connectors/postgres_connector.hpp"
#include "connectors/redis_connector.hpp"
#include "connectors/kafka_connector.hpp"
#include "connectors/minio_connector.hpp"
#include "connectors/mariadb_connector.hpp"
#include "connectors/clickhouse_connector.hpp"
#ifdef HAS_COMDARE_DB
#include "connectors/comdare_connector.hpp"
#endif
#include "experiment/schema_manager.hpp"
#include "experiment/data_loader.hpp"
#include "experiment/metrics_collector.hpp"
#include "experiment/metrics_trace.hpp"
#include "experiment/results_exporter.hpp"
#include "experiment/dataset_generator.hpp"
#include "experiment/checkpoint.hpp"

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
        "                      Valid: postgresql,cockroachdb,redis,kafka,minio,\n"
        "                             mariadb,clickhouse,comdare-db\n"
        "  --grades LIST       Comma-separated grades (default: U0,U50,U90)\n"
        "  --payload-types LIST Comma-separated payload types (default: synthetic)\n"
        "                      Valid: random_binary,structured_json,text_document,\n"
        "                             uuid_keys,jsonb_documents,nasa_image,\n"
        "                             blender_video,gutenberg_text,github_events,\n"
        "                             bank_transactions,text_corpus,numeric_dataset,mixed\n"
        "  --real-world-dir PATH  NAS dataset directory (default: /datasets/real-world)\n"
        "  --lab-schema NAME   Lab schema name (default: dedup_lab)\n"
        "  --generate-data     Generate synthetic test datasets before running\n"
        "  --num-files N       Files per duplication grade (default: 100)\n"
        "  --file-size N       Fixed file size in bytes (default: variable 4K-1M)\n"
        "  --seed N            PRNG seed for reproducible data (default: 42)\n"
        "  --cleanup-only      Only drop lab schemas, then exit (no experiment)\n"
        "  --dry-run           Simulate without actual DB operations\n"
        "  --checkpoint-dir D  Directory for checkpoint files (enables resume)\n"
        "  --run-id N          Run identifier (1,2,3) for checkpoint tracking\n"
        "  --max-retries N     Max retries per system on connection loss (default: 3)\n"
        "  --verbose           Enable debug logging\n"
        "  --help              Show this help\n"
        "\n"
        "SAFETY: This program operates on SEPARATE lab schemas.\n"
        "        Production/customer data is NEVER modified.\n"
        "        Lab schemas are RESET after every run.\n",
        prog);
}

static void print_sha256_selftest() {
    // NIST test vector: SHA-256("abc") = ba7816bf...
    std::string hash = dedup::SHA256::hash_hex("abc", 3);
    std::string expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    if (hash == expected) {
        LOG_INF("SHA-256 self-test: PASS");
    } else {
        LOG_ERR("SHA-256 self-test: FAIL (got %s)", hash.c_str());
        LOG_ERR("  expected: %s", expected.c_str());
    }
}

int main(int argc, char* argv[]) {
    // Parse arguments
    std::string config_path;
    std::string data_dir = "/tmp/datasets";
    std::string results_dir = "results";
    std::string lab_schema = "dedup_lab";
    std::string systems_filter;
    std::string grades_filter;
    std::string payload_types_filter;
    bool dry_run = false;
    bool generate_data = false;
    bool cleanup_only = false;
    size_t num_files = 100;
    size_t file_size = 0;
    uint64_t seed = 42;
    std::string checkpoint_dir;
    std::string real_world_dir;
    int run_id = 0;
    int max_retries = 3;

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
        } else if (std::strcmp(argv[i], "--payload-types") == 0 && i + 1 < argc) {
            payload_types_filter = argv[++i];
        } else if (std::strcmp(argv[i], "--lab-schema") == 0 && i + 1 < argc) {
            lab_schema = argv[++i];
        } else if (std::strcmp(argv[i], "--generate-data") == 0) {
            generate_data = true;
        } else if (std::strcmp(argv[i], "--num-files") == 0 && i + 1 < argc) {
            num_files = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--file-size") == 0 && i + 1 < argc) {
            file_size = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--cleanup-only") == 0) {
            cleanup_only = true;
        } else if (std::strcmp(argv[i], "--checkpoint-dir") == 0 && i + 1 < argc) {
            checkpoint_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--run-id") == 0 && i + 1 < argc) {
            run_id = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--max-retries") == 0 && i + 1 < argc) {
            max_retries = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--real-world-dir") == 0 && i + 1 < argc) {
            real_world_dir = argv[++i];
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

    LOG_INF("=== Dedup Integration Test ===");
    LOG_INF("TU Dresden Research: Deduplikation in Datenhaltungssystemen");

    if (dry_run) {
        LOG_INF("=== DRY RUN MODE === (no actual DB operations)");
    }

    // SHA-256 self-test
    print_sha256_selftest();

    // Load configuration (before dataset generation -- cfg is used by generator)
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

    // Generate datasets if requested (one subdirectory per payload type)
    // Directory structure: data_dir/{payload_type}/{U0,U50,U90}/
    if (generate_data) {
        LOG_INF("=== DATASET GENERATION ===");
        LOG_INF("Generating datasets for %zu payload types", cfg.payload_types.size());

        dedup::DatasetConfig dcfg;
        dcfg.num_files = num_files;
        dcfg.fixed_file_size = file_size;
        dcfg.seed = seed;
        dcfg.data_sources = cfg.data_sources;

        size_t total_all = 0;
        for (auto pt : cfg.payload_types) {
            std::string pt_dir = data_dir + "/" + dedup::payload_type_str(pt);
            LOG_INF("Generating %s datasets in %s", dedup::payload_type_str(pt), pt_dir.c_str());

            dedup::DatasetGenerator gen(dcfg);
            size_t total = gen.generate_all(pt_dir, pt);
            LOG_INF("  %s: %zu files (%zu bytes)",
                dedup::payload_type_str(pt), gen.total_files_written(), gen.total_bytes_written());
            total_all += total;
        }

        if (total_all == 0) {
            LOG_ERR("Dataset generation failed!");
            return 1;
        }
        LOG_INF("Total: %zu files generated across %zu payload types",
            total_all, cfg.payload_types.size());
        return 0;  // generate-data is a standalone operation
    }

    // Initialize checkpoint manager (if --checkpoint-dir specified)
    std::unique_ptr<dedup::Checkpoint> checkpoint;
    if (!checkpoint_dir.empty()) {
        checkpoint = std::make_unique<dedup::Checkpoint>(checkpoint_dir);
        if (run_id > 0) {
            LOG_INF("Resume mode: checkpoint-dir=%s, run-id=%d, max-retries=%d",
                checkpoint_dir.c_str(), run_id, max_retries);
        }
    }

    // Create results directory
    fs::create_directories(results_dir);

    // Initialize metrics collector (Prometheus for Longhorn, Grafana for push)
    dedup::MetricsCollector metrics(cfg.prometheus, cfg.grafana);

    // Create connectors for each configured database
    dedup::SchemaManager schema_mgr;
    struct ConnectorEntry {
        std::shared_ptr<dedup::DbConnector> connector;
        dedup::DbConnection db_conn;
    };
    std::vector<ConnectorEntry> entries;

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
            case dedup::DbSystem::MARIADB:
                conn = std::make_shared<dedup::MariaDBConnector>();
                break;
            case dedup::DbSystem::CLICKHOUSE:
                conn = std::make_shared<dedup::ClickHouseConnector>();
                break;
            case dedup::DbSystem::COMDARE_DB:
#ifdef HAS_COMDARE_DB
                conn = std::make_shared<dedup::ComdareConnector>();
#else
                LOG_WRN("comdare-DB support not compiled in (HAS_COMDARE_DB=0) -- skipping");
                continue;
#endif
                break;
        }

        LOG_INF("Connecting to %s at %s:%u (PVC: %s)...",
            dedup::db_system_str(db.system), db.host.c_str(), db.port,
            db.pvc_name.empty() ? "none" : db.pvc_name.c_str());

        if (conn->connect(db)) {
            entries.push_back({conn, db});
            schema_mgr.add_connector(conn);
        } else {
            LOG_ERR("Failed to connect to %s -- skipping", dedup::db_system_str(db.system));
        }
    }

    if (entries.empty()) {
        LOG_ERR("No database connections established. Exiting.");
        return 1;
    }

    LOG_INF("Connected to %zu databases", entries.size());

    // Cleanup-only mode: drop lab schemas and exit (used by CI cleanup job)
    if (cleanup_only) {
        LOG_INF("=== CLEANUP-ONLY MODE ===");
        LOG_INF("Dropping lab schemas (%s) on all %zu connected databases...",
            lab_schema.c_str(), entries.size());
        LOG_WRN("This removes ONLY lab data (schema: %s). Customer data is NOT affected.",
            lab_schema.c_str());

        schema_mgr.drop_all_lab_schemas(lab_schema);

        for (auto& entry : entries) {
            entry.connector->disconnect();
        }

        LOG_INF("=== CLEANUP COMPLETE ===");
        return 0;
    }

    // Parse dup grades
    std::vector<dedup::DupGrade> grades = cfg.dup_grades;
    if (!grades_filter.empty()) {
        grades.clear();
        if (grades_filter.find("U0") != std::string::npos) grades.push_back(dedup::DupGrade::U0);
        if (grades_filter.find("U50") != std::string::npos) grades.push_back(dedup::DupGrade::U50);
        if (grades_filter.find("U90") != std::string::npos) grades.push_back(dedup::DupGrade::U90);
    }

    // Parse payload types filter (doku.tex §6.3)
    if (!payload_types_filter.empty()) {
        cfg.payload_types.clear();
        // Split by comma and parse each
        std::string token;
        std::istringstream stream(payload_types_filter);
        while (std::getline(stream, token, ',')) {
            cfg.payload_types.push_back(dedup::parse_payload_type(token));
        }
        LOG_INF("Payload types: %zu types selected", cfg.payload_types.size());
    }

    // Override real-world data directory (NAS datasets on experiment PVC)
    if (!real_world_dir.empty()) {
        cfg.data_sources.real_world_dir = real_world_dir;
        LOG_INF("Real-world data directory: %s", real_world_dir.c_str());
    }

    // Create lab schemas
    LOG_INF("Creating lab schemas (%s) on all connected databases...", lab_schema.c_str());
    schema_mgr.create_all_lab_schemas(lab_schema);

    // Initialize 100ms MetricsTrace (background thread + Kafka producer)
    dedup::MetricsTrace trace(cfg.metrics_trace, dry_run);
    if (cfg.metrics_trace.enabled) {
        for (auto& entry : entries) {
            trace.register_system(entry.db_conn,
                dedup::collectors::for_system(entry.db_conn.system));
        }
        trace.start();
        trace.publish_event({dedup::now_ms(), "experiment_start", "", "", "", "",
            "{\"systems\":" + std::to_string(entries.size()) +
            ",\"payload_types\":" + std::to_string(cfg.payload_types.size()) + "}"});
    }

    // Run experiments -- system × payload_type × grade × stage
    // With connection retry (exponential backoff) and per-system checkpointing.
    // On connection loss: retry ALL payload types for that system from scratch.
    // Directory structure: data_dir/{payload_type}/{U0,U50,U90}/
    dedup::DataLoader loader(schema_mgr, metrics, cfg.replica_count, cfg.db_internal_metrics);
    std::vector<dedup::ExperimentResult> all_results;
    int systems_failed = 0;

    LOG_INF("=== EXPERIMENT MATRIX: %zu systems x %zu payload types x %zu grades x %zu stages ===",
        entries.size(), cfg.payload_types.size(), grades.size(), cfg.stages.size());

    for (auto& entry : entries) {
        std::string sys_name = dedup::db_system_str(entry.db_conn.system);

        // Checkpoint: skip if this system+run is already completed
        if (checkpoint && run_id > 0 && checkpoint->is_complete(sys_name, run_id)) {
            LOG_INF("[Resume] %s run %d already complete -- skipping", sys_name.c_str(), run_id);
            continue;
        }

        bool system_ok = false;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            if (attempt > 0) {
                // Exponential backoff between retries: 5s, 10s, 20s
                int backoff_s = 5 * (1 << (attempt - 1));
                if (backoff_s > 60) backoff_s = 60;
                LOG_WRN("[Recovery] %s FAILED -- retry %d/%d in %d s...",
                    sys_name.c_str(), attempt, max_retries, backoff_s);
                std::this_thread::sleep_for(std::chrono::seconds(backoff_s));

                // Reconnect before retry
                if (!entry.connector->ensure_connected(entry.db_conn)) {
                    LOG_ERR("[Recovery] Cannot reconnect to %s -- trying next attempt",
                        sys_name.c_str());
                    continue;
                }

                // Clean lab schema for fresh retry
                entry.connector->reset_lab_schema(lab_schema);
                entry.connector->create_lab_schema(lab_schema);
            }

            std::vector<dedup::ExperimentResult> system_results;
            bool had_conn_error = false;

            for (auto pt : cfg.payload_types) {
                std::string pt_data_dir = data_dir + "/" + dedup::payload_type_str(pt);
                LOG_INF("=== %s / %s (data: %s) ===",
                    sys_name.c_str(), dedup::payload_type_str(pt), pt_data_dir.c_str());

                if (cfg.metrics_trace.enabled) {
                    trace.publish_event({dedup::now_ms(), "system_start", sys_name,
                        dedup::payload_type_str(pt), "", "", ""});
                }

                auto results = loader.run_full_experiment(
                    *entry.connector, entry.db_conn, pt_data_dir, lab_schema, grades, pt);

                // Check for fatal connection loss in results
                for (const auto& r : results) {
                    if (!r.error.empty() &&
                        r.error.find("CONNECTION_LOST") != std::string::npos) {
                        had_conn_error = true;
                        break;
                    }
                }

                if (cfg.metrics_trace.enabled) {
                    trace.publish_event({dedup::now_ms(), "system_end", sys_name,
                        dedup::payload_type_str(pt), "", "",
                        "{\"runs\":" + std::to_string(results.size()) + "}"});
                }

                if (had_conn_error) {
                    LOG_ERR("[Recovery] Connection lost during %s / %s -- will retry system",
                        sys_name.c_str(), dedup::payload_type_str(pt));
                    break;
                }

                system_results.insert(system_results.end(), results.begin(), results.end());
            }

            if (!had_conn_error) {
                // All payload types completed successfully for this system
                all_results.insert(all_results.end(),
                    system_results.begin(), system_results.end());
                system_ok = true;
                break;
            }
        }

        if (system_ok) {
            if (checkpoint && run_id > 0) {
                checkpoint->mark_complete(sys_name, run_id,
                    static_cast<int>(all_results.size()));
            }
            LOG_INF("[Resume] %s run %d: SUCCESS", sys_name.c_str(), run_id);
        } else {
            LOG_ERR("[Resume] %s FAILED after %d retries -- invalidating checkpoints",
                sys_name.c_str(), max_retries + 1);
            if (checkpoint) {
                checkpoint->invalidate_system(sys_name);
            }
            systems_failed++;
        }
    }

    // Stop MetricsTrace background thread
    if (cfg.metrics_trace.enabled) {
        trace.publish_event({dedup::now_ms(), "experiment_end", "", "", "", "",
            "{\"total_runs\":" + std::to_string(all_results.size()) + "}"});
        trace.stop();
        LOG_INF("MetricsTrace: %lld metrics, %lld events published",
            static_cast<long long>(trace.metrics_published()),
            static_cast<long long>(trace.events_published()));
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

    // Export metrics + events from Kafka to CSV, then git push to GitLab
    // CRITICAL: Must complete BEFORE cleanup! Data is deleted after this.
#ifdef DEDUP_DRY_RUN
    LOG_INF("DRY RUN -- skipping export phase (no Kafka data produced)");
#else
    if (cfg.metrics_trace.enabled) {
        LOG_INF("=== EXPORT PHASE (before cleanup) ===");
        dedup::ResultsExporter exporter(cfg.git_export, cfg.metrics_trace, results_dir);
        bool exported = exporter.export_all();
        if (!exported) {
            LOG_ERR("Export failed! Results may not be persisted in GitLab.");
            LOG_WRN("Proceeding with cleanup anyway (lab schemas will be dropped).");
        }
    }
#endif

    // Final cleanup: drop all lab schemas
    LOG_INF("Final cleanup: dropping all lab schemas...");
    schema_mgr.drop_all_lab_schemas(lab_schema);

    // Disconnect all
    for (auto& entry : entries) {
        entry.connector->disconnect();
    }

    LOG_INF("=== EXPERIMENT COMPLETE ===");
    LOG_INF("Total runs: %zu", all_results.size());
    LOG_INF("Results: %s", combined_path.c_str());
    if (systems_failed > 0) {
        LOG_ERR("%d system(s) FAILED after all retries!", systems_failed);
    }

    // Print summary table (doku.tex metrics: payload, duration, logical bytes, physical delta, EDR, throughput, latency)
    std::printf("\n%-13s %-16s %-4s %-15s %10s %12s %12s %7s %10s %10s %10s %10s\n",
        "System", "Payload", "Dup", "Stage", "Time(ms)", "Logical(B)", "PhysDelta", "EDR", "MB/s",
        "p50(us)", "p95(us)", "p99(us)");
    std::printf("%.160s\n",
        "----------------------------------------------------------------------------------------------------------------------------------------------------------------");
    for (const auto& r : all_results) {
        std::printf("%-13s %-16s %-4s %-15s %10lld %12lld %12lld %7.3f %10.1f",
            r.system.c_str(), r.payload_type.c_str(), r.dup_grade.c_str(), r.stage.c_str(),
            r.duration_ns / 1000000, r.bytes_logical, r.phys_delta,
            r.edr, r.throughput_bytes_per_sec / (1024.0 * 1024.0));
        if (r.latency_count > 0) {
            std::printf(" %10lld %10lld %10lld",
                r.latency_p50_ns / 1000, r.latency_p95_ns / 1000, r.latency_p99_ns / 1000);
        } else {
            std::printf(" %10s %10s %10s", "-", "-", "-");
        }
        std::printf("\n");
    }

    return systems_failed > 0 ? 1 : 0;
}
