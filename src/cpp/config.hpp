#pragma once
// =============================================================================
// Experiment Configuration -- TU Dresden Research Project
// "Deduplikation in Datenhaltungssystemen"
//
// Defines all types, enums, and configuration structures for the experiment
// as specified in doku.tex Chapter 6 (Experimental Design and Measurement Plan).
//
// Changes from v1:
//   - PayloadType moved here from dataset_generator.hpp (§6.3)
//   - Added real-world data sources: NASA, Blender, Gutenberg, GH Archive
//   - Added UUID_KEYS and JSONB_DOCUMENTS payload types (§6.3)
//   - Added per-DB experiment size limit (CockroachDB: 50 GiB)
//   - Added DataSourceConfig for real-world payload caching
//   - Added DB-internal instrumentation toggle
// =============================================================================

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

namespace dedup {

// ============================================================================
// Duplication grades (doku.tex §6.4.1)
// U0 = unique (0% duplicates), U50 = 50%, U90 = 90%
// ============================================================================

enum class DupGrade { U0, U50, U90 };

inline const char* dup_grade_str(DupGrade g) {
    switch (g) {
        case DupGrade::U0:  return "U0";
        case DupGrade::U50: return "U50";
        case DupGrade::U90: return "U90";
    }
    return "??";
}

inline double dup_grade_ratio(DupGrade g) {
    switch (g) {
        case DupGrade::U0:  return 0.0;
        case DupGrade::U50: return 0.5;
        case DupGrade::U90: return 0.9;
    }
    return 0.0;
}

// ============================================================================
// Experiment stages (doku.tex §6.4.2-6.4.4)
// Stage 1: Bulk insert in "natural" schema
// Stage 2: Per-file insertion as single records/objects/messages
// Stage 3: Per-file deletion (3a) + post-delete maintenance/reclamation (3b)
// ============================================================================

enum class Stage { BULK_INSERT, PERFILE_INSERT, PERFILE_DELETE, MAINTENANCE };

inline const char* stage_str(Stage s) {
    switch (s) {
        case Stage::BULK_INSERT:    return "bulk_insert";
        case Stage::PERFILE_INSERT: return "perfile_insert";
        case Stage::PERFILE_DELETE: return "perfile_delete";
        case Stage::MAINTENANCE:    return "maintenance";
    }
    return "??";
}

// ============================================================================
// Database systems under test (doku.tex §6.2)
// ============================================================================

enum class DbSystem {
    POSTGRESQL,
    COCKROACHDB,
    REDIS,
    KAFKA,
    MINIO,
    MARIADB,
    CLICKHOUSE,
    COMDARE_DB
};

inline const char* db_system_str(DbSystem db) {
    switch (db) {
        case DbSystem::POSTGRESQL:  return "postgresql";
        case DbSystem::COCKROACHDB: return "cockroachdb";
        case DbSystem::REDIS:       return "redis";
        case DbSystem::KAFKA:       return "kafka";
        case DbSystem::MINIO:       return "minio";
        case DbSystem::MARIADB:     return "mariadb";
        case DbSystem::CLICKHOUSE:  return "clickhouse";
        case DbSystem::COMDARE_DB:  return "comdare-db";
    }
    return "??";
}

inline DbSystem parse_db_system(const std::string& s) {
    if (s == "postgresql")  return DbSystem::POSTGRESQL;
    if (s == "cockroachdb") return DbSystem::COCKROACHDB;
    if (s == "redis")       return DbSystem::REDIS;
    if (s == "kafka")       return DbSystem::KAFKA;
    if (s == "minio")       return DbSystem::MINIO;
    if (s == "mariadb")     return DbSystem::MARIADB;
    if (s == "clickhouse")  return DbSystem::CLICKHOUSE;
    if (s == "comdare-db")  return DbSystem::COMDARE_DB;
    return DbSystem::POSTGRESQL;
}

// ============================================================================
// Payload types (doku.tex §6.3 -- Data sets and payload types)
//
// Synthetic payloads are generated in-process by DatasetGenerator.
// Real-world payloads are downloaded once, cached locally, then sliced
// into files with controlled duplication.
// ============================================================================

enum class PayloadType {
    // Synthetic (generated in-process)
    RANDOM_BINARY,        // Pure random bytes -- incompressible (simulates encrypted data)
    STRUCTURED_JSON,      // JSON with known fields -- compressible, semi-structured
    TEXT_DOCUMENT,        // ASCII text with word patterns -- highly compressible
    UUID_KEYS,            // High-entropy UUID/GUID identifiers (§6.3 "Additional SQL data types")
    JSONB_DOCUMENTS,      // Semi-structured JSON/JSONB payloads (§6.3 "Additional SQL data types")

    // Real-world (downloaded + cached)
    NASA_IMAGE,           // NASA imagery, e.g. Hubble Ultra Deep Field .tif (§6.3 "Images")
    BLENDER_VIDEO,        // Blender Foundation movies: BBB, Sintel, ToS (§6.3 "Video")
    GUTENBERG_TEXT,       // Project Gutenberg plain text novels (§6.3 "Full text")
    GITHUB_EVENTS,        // GH Archive event streams .json.gz (§6.3 "GitHub logs")

    // Real-world (pre-loaded from NAS onto experiment PVC)
    BANK_TRANSACTIONS,    // bankdataset.xlsx -- financial tabular data (§6.3 "Bank transactions")
    TEXT_CORPUS,          // million_post_corpus -- forum posts, natural language (§6.3 "Full text")
    NUMERIC_DATASET,      // random-numbers -- numeric/statistical data (§6.3 "Synthetic random numbers")

    // Composite
    MIXED                 // Random mix of all available payload types
};

inline const char* payload_type_str(PayloadType t) {
    switch (t) {
        case PayloadType::RANDOM_BINARY:    return "random_binary";
        case PayloadType::STRUCTURED_JSON:  return "structured_json";
        case PayloadType::TEXT_DOCUMENT:    return "text_document";
        case PayloadType::UUID_KEYS:        return "uuid_keys";
        case PayloadType::JSONB_DOCUMENTS:  return "jsonb_documents";
        case PayloadType::NASA_IMAGE:       return "nasa_image";
        case PayloadType::BLENDER_VIDEO:    return "blender_video";
        case PayloadType::GUTENBERG_TEXT:   return "gutenberg_text";
        case PayloadType::GITHUB_EVENTS:    return "github_events";
        case PayloadType::BANK_TRANSACTIONS: return "bank_transactions";
        case PayloadType::TEXT_CORPUS:      return "text_corpus";
        case PayloadType::NUMERIC_DATASET:  return "numeric_dataset";
        case PayloadType::MIXED:            return "mixed";
    }
    return "??";
}

inline PayloadType parse_payload_type(const std::string& s) {
    if (s == "random_binary")   return PayloadType::RANDOM_BINARY;
    if (s == "structured_json") return PayloadType::STRUCTURED_JSON;
    if (s == "text_document")   return PayloadType::TEXT_DOCUMENT;
    if (s == "uuid_keys")       return PayloadType::UUID_KEYS;
    if (s == "jsonb_documents") return PayloadType::JSONB_DOCUMENTS;
    if (s == "nasa_image")      return PayloadType::NASA_IMAGE;
    if (s == "blender_video")   return PayloadType::BLENDER_VIDEO;
    if (s == "gutenberg_text")  return PayloadType::GUTENBERG_TEXT;
    if (s == "github_events")       return PayloadType::GITHUB_EVENTS;
    if (s == "bank_transactions")   return PayloadType::BANK_TRANSACTIONS;
    if (s == "text_corpus")         return PayloadType::TEXT_CORPUS;
    if (s == "numeric_dataset")     return PayloadType::NUMERIC_DATASET;
    if (s == "mixed")               return PayloadType::MIXED;
    return PayloadType::MIXED;
}

inline bool is_real_world_payload(PayloadType t) {
    return t == PayloadType::NASA_IMAGE || t == PayloadType::BLENDER_VIDEO ||
           t == PayloadType::GUTENBERG_TEXT || t == PayloadType::GITHUB_EVENTS ||
           t == PayloadType::BANK_TRANSACTIONS || t == PayloadType::TEXT_CORPUS ||
           t == PayloadType::NUMERIC_DATASET;
}

// NAS-sourced types require pre-loaded data on experiment PVC (no internet download)
inline bool is_nas_payload(PayloadType t) {
    return t == PayloadType::BANK_TRANSACTIONS || t == PayloadType::TEXT_CORPUS ||
           t == PayloadType::NUMERIC_DATASET;
}

// ============================================================================
// Connection info for a single database
// ============================================================================

struct DbConnection {
    DbSystem system;
    std::string host;
    uint16_t port;
    std::string user;           // Lab user from Samba AD LDAP
    std::string password;
    std::string database;       // Lab schema/database name
    std::string lab_schema;     // Schema prefix for isolation
    std::string pvc_name;       // K8s PVC name for Longhorn volume mapping
    std::string k8s_namespace;  // K8s namespace where the PVC lives

    // Per-DB experiment constraint: max logical bytes to insert.
    // 0 = unlimited (use full PVC capacity).
    // CockroachDB: 50 GiB (125 GiB PVC shared with production data).
    int64_t max_experiment_bytes = 0;
};

// ============================================================================
// Real-world data source configuration (doku.tex §6.3)
// Downloaded once, cached in cache_dir, then sliced for experiment use.
// ============================================================================

struct DataSourceConfig {
    // NASA imagery (§6.3 "Images (large binary objects)")
    std::string nasa_hudf_url =
        "https://stsci-opo.org/STScI-01EVST5CZG5BK2MAJQFCY2QHVS.tif";
    std::string nasa_api_url = "https://images-api.nasa.gov";

    // Blender Foundation movies (§6.3 "Video (very large binary objects)")
    std::vector<std::string> blender_urls = {
        "https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_320x180.mp4",
        "https://download.blender.org/durian/movies/Sintel.2010.1080p.mkv",
        "https://download.blender.org/mango/download/ToS-4k-1920.mov"
    };

    // Project Gutenberg (§6.3 "Full text (large textual payloads)")
    std::vector<int> gutenberg_ids = {1342, 2701};  // Pride & Prejudice, Moby-Dick
    std::string gutenberg_mirror = "https://www.gutenberg.org/cache/epub";

    // GH Archive (§6.3 "GitHub logs")
    std::string gharchive_base_url = "https://data.gharchive.org/";

    // Local cache directory for downloaded real-world data
    std::string cache_dir = "/tmp/dedup-datasets-cache";

    // Pre-loaded NAS datasets directory (experiment PVC mount)
    // Structure: real_world_dir/{bankdataset,million_post,random_numbers}/
    std::string real_world_dir = "/datasets/real-world";
};

// ============================================================================
// Infrastructure configuration structs
// ============================================================================

// Prometheus endpoint for Longhorn metrics
struct PrometheusConfig {
    std::string url = "http://kube-prometheus-stack-prometheus.monitoring.svc.cluster.local:9090";
};

// Grafana push endpoint
struct GrafanaConfig {
    std::string url;          // Grafana HTTP push endpoint
    std::string api_key;
    std::string dashboard_uid;
};

// Kafka metrics trace configuration (100ms sampling)
// Kafka serves DUAL ROLE: DB under test AND metrics log
struct MetricsTraceConfig {
    std::string kafka_bootstrap = "kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local:9092";
    std::string metrics_topic = "dedup-lab-metrics";   // 100ms DB metric snapshots
    std::string events_topic = "dedup-lab-events";     // Experiment stage events/results
    int sample_interval_ms = 100;                      // 10 Hz sampling rate
    bool enabled = true;
};

// Git export configuration (commit+push results before cleanup)
struct GitExportConfig {
    std::string remote_name = "gitlab";
    std::string branch = "development";
    bool auto_push = true;                             // Push to GitLab after export
    bool ssl_verify = false;                           // GitLab uses self-signed cert
};

// ============================================================================
// Full experiment configuration
// ============================================================================

struct ExperimentConfig {
    // Lab isolation
    std::string lab_user = "dedup-lab";
    std::string lab_schema = "dedup_lab";

    // Systems to test
    std::vector<DbConnection> databases;

    // Experiment parameters (doku.tex §6.4)
    std::vector<DupGrade> dup_grades = {DupGrade::U0, DupGrade::U50, DupGrade::U90};
    std::vector<Stage> stages = {Stage::BULK_INSERT, Stage::PERFILE_INSERT,
                                  Stage::PERFILE_DELETE, Stage::MAINTENANCE};

    // Payload types to test (doku.tex §6.3)
    // Default: synthetic types only. Add real-world types via config or CLI.
    std::vector<PayloadType> payload_types = {
        PayloadType::RANDOM_BINARY,
        PayloadType::STRUCTURED_JSON,
        PayloadType::TEXT_DOCUMENT,
        PayloadType::UUID_KEYS,
        PayloadType::JSONB_DOCUMENTS
    };

    // Real-world data source configuration (doku.tex §6.3)
    DataSourceConfig data_sources;

    // Data paths
    std::string data_dir = "/tmp/datasets";
    std::string results_dir = "results";

    // Longhorn storage measurement (doku.tex §6.1: "replica count N=4")
    int replica_count = 4;
    PrometheusConfig prometheus;

    // Grafana metrics push
    GrafanaConfig grafana;

    // Kafka metrics trace (100ms sampling to Kafka + Grafana)
    MetricsTraceConfig metrics_trace;

    // Git export (commit+push results before cleanup)
    GitExportConfig git_export;

    // Behavior
    bool dry_run = false;
    bool reset_schema_after_run = true;  // ALWAYS reset lab schema!

    // DB-internal instrumentation (doku.tex §6.5 "Database-internal instrumentation")
    // When true, query DB-native metrics (pg_total_relation_size, system.columns, etc.)
    // at stage boundaries in addition to Longhorn-level measurements.
    bool db_internal_metrics = true;

    static ExperimentConfig from_json(const std::string& path);
    static ExperimentConfig default_k8s_config();
};

// ============================================================================
// Default K8s cluster configuration (production DB endpoints)
// ============================================================================

inline ExperimentConfig ExperimentConfig::default_k8s_config() {
    ExperimentConfig cfg;

    // 50 GiB CockroachDB experiment limit (125 GiB PVC shared with production)
    constexpr int64_t CRDB_EXPERIMENT_LIMIT = static_cast<int64_t>(50) * 1024 * 1024 * 1024;

    // Read passwords from CI/CD environment variables (masked in GitLab)
    auto env_or = [](const char* name, const char* fallback) -> std::string {
        const char* v = std::getenv(name);
        return (v && v[0]) ? v : fallback;
    };

    std::string pg_pass    = env_or("DEDUP_PG_PASSWORD", "");
    std::string crdb_pass  = env_or("DEDUP_CRDB_PASSWORD", "");
    std::string redis_pass = env_or("DEDUP_REDIS_PASSWORD", "");
    std::string maria_pass = env_or("DEDUP_MARIADB_PASSWORD", "");
    std::string minio_pass = env_or("DEDUP_MINIO_PASSWORD", "");

    cfg.databases = {
        // PostgreSQL: 50 GiB Longhorn PVC, replica 4
        {DbSystem::POSTGRESQL, "postgres-lb.databases.svc.cluster.local", 5432,
         cfg.lab_user, pg_pass, "postgres", "dedup_lab",
         "data-postgres-ha-0", "databases", 0},

        // CockroachDB: 125 GiB PVC (production!), experiments limited to 50 GiB
        {DbSystem::COCKROACHDB, "cockroachdb-public.cockroach-operator-system.svc.cluster.local", 26257,
         "dedup_lab", crdb_pass, "dedup_lab", "dedup_lab",
         "datadir-cockroachdb-0", "cockroach-operator-system", CRDB_EXPERIMENT_LIMIT},

        // Redis: 50 GiB Longhorn PVC, standalone mode, ACL auth, key prefix dedup:*
        {DbSystem::REDIS, "redis-standalone.redis.svc.cluster.local", 6379,
         "dedup-lab", redis_pass, "", "",
         "data-redis-cluster-0", "redis", 0},

        // Kafka: 50 GiB Longhorn PVC per broker, topic prefix dedup-lab-*
        {DbSystem::KAFKA, "kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local", 9092,
         "", "", "", "dedup-lab",
         "data-kafka-cluster-broker-0", "kafka", 0},

        // MinIO: Direct Disk (NO Longhorn PVC), LDAP Access Key
        {DbSystem::MINIO, "minio-lb.minio.svc.cluster.local", 9000,
         "dedup-lab-s3", minio_pass, "", "dedup-lab",
         "", "minio", 0},

        // MariaDB: 50 GiB Longhorn PVC, replica 4
        {DbSystem::MARIADB, "mariadb-lb.databases.svc.cluster.local", 3306,
         cfg.lab_user, maria_pass, "dedup_lab", "dedup_lab",
         "data-mariadb-0", "databases", 0},

        // ClickHouse: 50 GiB Longhorn PVC, replica 4, HTTP API
        {DbSystem::CLICKHOUSE, "clickhouse-lb.databases.svc.cluster.local", 8123,
         cfg.lab_user, "", "dedup_lab", "dedup_lab",
         "data-clickhouse-0", "databases", 0},
    };

    return cfg;
}

// ============================================================================
// JSON configuration parser
// ============================================================================

inline ExperimentConfig ExperimentConfig::from_json(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return default_k8s_config();
    }

    nlohmann::json j;
    f >> j;

    ExperimentConfig cfg;
    cfg.lab_user = j.value("lab_user", cfg.lab_user);
    cfg.lab_schema = j.value("lab_schema", cfg.lab_schema);
    cfg.data_dir = j.value("data_dir", cfg.data_dir);
    cfg.results_dir = j.value("results_dir", cfg.results_dir);
    cfg.replica_count = j.value("replica_count", cfg.replica_count);
    cfg.dry_run = j.value("dry_run", cfg.dry_run);
    cfg.db_internal_metrics = j.value("db_internal_metrics", cfg.db_internal_metrics);

    if (j.contains("prometheus")) {
        cfg.prometheus.url = j["prometheus"].value("url", cfg.prometheus.url);
    }

    if (j.contains("grafana")) {
        cfg.grafana.url = j["grafana"].value("url", cfg.grafana.url);
        cfg.grafana.api_key = j["grafana"].value("api_key", cfg.grafana.api_key);
        cfg.grafana.dashboard_uid = j["grafana"].value("dashboard_uid", cfg.grafana.dashboard_uid);
    }

    // Environment variable fallback for passwords (CI/CD masked variables)
    auto env_or = [](const char* name, const std::string& fallback) -> std::string {
        const char* v = std::getenv(name);
        return (v && v[0]) ? v : fallback;
    };

    if (j.contains("databases")) {
        cfg.databases.clear();
        for (const auto& db : j["databases"]) {
            DbConnection conn;
            conn.system = parse_db_system(db.value("system", "postgresql"));
            conn.host = db.value("host", "localhost");
            conn.port = db.value("port", static_cast<uint16_t>(5432));
            conn.user = db.value("user", cfg.lab_user);
            conn.password = db.value("password", "");
            conn.database = db.value("database", "");
            conn.lab_schema = db.value("lab_schema", cfg.lab_schema);
            conn.pvc_name = db.value("pvc_name", "");
            conn.k8s_namespace = db.value("k8s_namespace", "");
            conn.max_experiment_bytes = db.value("max_experiment_bytes", static_cast<int64_t>(0));

            // Fallback to CI/CD environment variables if JSON password is empty
            if (conn.password.empty()) {
                switch (conn.system) {
                    case DbSystem::POSTGRESQL:  conn.password = env_or("DEDUP_PG_PASSWORD", ""); break;
                    case DbSystem::COCKROACHDB: conn.password = env_or("DEDUP_CRDB_PASSWORD", ""); break;
                    case DbSystem::REDIS:       conn.password = env_or("DEDUP_REDIS_PASSWORD", ""); break;
                    case DbSystem::MARIADB:     conn.password = env_or("DEDUP_MARIADB_PASSWORD", ""); break;
                    case DbSystem::MINIO:       conn.password = env_or("DEDUP_MINIO_PASSWORD", ""); break;
                    default: break;
                }
            }

            cfg.databases.push_back(conn);
        }
    } else {
        cfg.databases = default_k8s_config().databases;
    }

    if (j.contains("dup_grades")) {
        cfg.dup_grades.clear();
        for (const auto& g : j["dup_grades"]) {
            std::string gs = g.get<std::string>();
            if (gs == "U0")  cfg.dup_grades.push_back(DupGrade::U0);
            else if (gs == "U50") cfg.dup_grades.push_back(DupGrade::U50);
            else if (gs == "U90") cfg.dup_grades.push_back(DupGrade::U90);
        }
    }

    if (j.contains("payload_types")) {
        cfg.payload_types.clear();
        for (const auto& pt : j["payload_types"]) {
            cfg.payload_types.push_back(parse_payload_type(pt.get<std::string>()));
        }
    }

    if (j.contains("data_sources")) {
        auto& ds = j["data_sources"];
        cfg.data_sources.cache_dir = ds.value("cache_dir", cfg.data_sources.cache_dir);
        cfg.data_sources.nasa_hudf_url = ds.value("nasa_hudf_url", cfg.data_sources.nasa_hudf_url);
        cfg.data_sources.nasa_api_url = ds.value("nasa_api_url", cfg.data_sources.nasa_api_url);
        cfg.data_sources.gharchive_base_url = ds.value("gharchive_base_url", cfg.data_sources.gharchive_base_url);
        cfg.data_sources.gutenberg_mirror = ds.value("gutenberg_mirror", cfg.data_sources.gutenberg_mirror);
        if (ds.contains("gutenberg_ids")) {
            cfg.data_sources.gutenberg_ids.clear();
            for (const auto& id : ds["gutenberg_ids"]) {
                cfg.data_sources.gutenberg_ids.push_back(id.get<int>());
            }
        }
        if (ds.contains("blender_urls")) {
            cfg.data_sources.blender_urls.clear();
            for (const auto& url : ds["blender_urls"]) {
                cfg.data_sources.blender_urls.push_back(url.get<std::string>());
            }
        }
    }

    if (j.contains("metrics_trace")) {
        auto& mt = j["metrics_trace"];
        cfg.metrics_trace.kafka_bootstrap = mt.value("kafka_bootstrap", cfg.metrics_trace.kafka_bootstrap);
        cfg.metrics_trace.metrics_topic = mt.value("kafka_topic", cfg.metrics_trace.metrics_topic);
        cfg.metrics_trace.events_topic = mt.value("events_topic", cfg.metrics_trace.events_topic);
        cfg.metrics_trace.sample_interval_ms = mt.value("sample_interval_ms", cfg.metrics_trace.sample_interval_ms);
        cfg.metrics_trace.enabled = mt.value("enabled", cfg.metrics_trace.enabled);
    }

    if (j.contains("git_export")) {
        auto& ge = j["git_export"];
        cfg.git_export.remote_name = ge.value("remote_name", cfg.git_export.remote_name);
        cfg.git_export.branch = ge.value("branch", cfg.git_export.branch);
        cfg.git_export.auto_push = ge.value("auto_push", cfg.git_export.auto_push);
        cfg.git_export.ssl_verify = ge.value("ssl_verify", cfg.git_export.ssl_verify);
    }

    return cfg;
}

} // namespace dedup
