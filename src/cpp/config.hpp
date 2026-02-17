#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

namespace dedup {

// Duplication grades as defined in the research paper
enum class DupGrade { U0, U50, U90 };

inline const char* dup_grade_str(DupGrade g) {
    switch (g) {
        case DupGrade::U0:  return "U0";
        case DupGrade::U50: return "U50";
        case DupGrade::U90: return "U90";
    }
    return "??";
}

// Experiment stages as defined in the research paper
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

// Database system under test
enum class DbSystem {
    POSTGRESQL,
    COCKROACHDB,
    REDIS,
    KAFKA,
    MINIO
    // MariaDB, ClickHouse: TODO when deployed in cluster
};

inline const char* db_system_str(DbSystem db) {
    switch (db) {
        case DbSystem::POSTGRESQL:  return "postgresql";
        case DbSystem::COCKROACHDB: return "cockroachdb";
        case DbSystem::REDIS:       return "redis";
        case DbSystem::KAFKA:       return "kafka";
        case DbSystem::MINIO:       return "minio";
    }
    return "??";
}

// Connection info for a single database
struct DbConnection {
    DbSystem system;
    std::string host;
    uint16_t port;
    std::string user;         // Lab user from Samba AD LDAP
    std::string password;
    std::string database;     // Lab schema/database name
    std::string lab_schema;   // Schema prefix for isolation
};

// Prometheus endpoint for Longhorn metrics
struct PrometheusConfig {
    std::string url = "http://prometheus.monitoring.svc.cluster.local:9090";
};

// Grafana push endpoint
struct GrafanaConfig {
    std::string url;          // Grafana HTTP push endpoint
    std::string api_key;
    std::string dashboard_uid;
};

// Full experiment configuration
struct ExperimentConfig {
    // Lab isolation
    std::string lab_user = "dedup-lab";
    std::string lab_schema = "dedup_lab";

    // Systems to test
    std::vector<DbConnection> databases;

    // Experiment parameters
    std::vector<DupGrade> dup_grades = {DupGrade::U0, DupGrade::U50, DupGrade::U90};
    std::vector<Stage> stages = {Stage::BULK_INSERT, Stage::PERFILE_INSERT,
                                  Stage::PERFILE_DELETE, Stage::MAINTENANCE};

    // Data paths
    std::string data_dir = "/tmp/datasets";
    std::string results_dir = "results";

    // Longhorn storage measurement
    int replica_count = 4;
    PrometheusConfig prometheus;

    // Grafana metrics push
    GrafanaConfig grafana;

    // Behavior
    bool dry_run = false;
    bool reset_schema_after_run = true;  // ALWAYS reset lab schema!

    static ExperimentConfig from_json(const std::string& path);
    static ExperimentConfig default_k8s_config();
};

// Default K8s cluster configuration (production DB endpoints)
inline ExperimentConfig ExperimentConfig::default_k8s_config() {
    ExperimentConfig cfg;

    cfg.databases = {
        {DbSystem::POSTGRESQL, "postgres-lb.databases.svc.cluster.local", 5432,
         cfg.lab_user, "", "postgres", "dedup_lab"},

        {DbSystem::COCKROACHDB, "cockroachdb-public.cockroach-operator-system.svc.cluster.local", 26257,
         cfg.lab_user, "", "dedup_lab", "dedup_lab"},

        {DbSystem::REDIS, "redis-standalone.redis.svc.cluster.local", 6379,
         "", "", "", ""},  // Redis: cluster mode, key-prefix "dedup:*" for lab

        {DbSystem::KAFKA, "kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local", 9092,
         "", "", "", "dedup-lab"},  // Kafka: topic prefix dedup-lab-*

        {DbSystem::MINIO, "minio-lb.minio.svc.cluster.local", 9000,
         cfg.lab_user, "", "", "dedup-lab"},  // MinIO: bucket prefix dedup-lab-*
    };

    return cfg;
}

inline DbSystem parse_db_system(const std::string& s) {
    if (s == "postgresql") return DbSystem::POSTGRESQL;
    if (s == "cockroachdb") return DbSystem::COCKROACHDB;
    if (s == "redis") return DbSystem::REDIS;
    if (s == "kafka") return DbSystem::KAFKA;
    if (s == "minio") return DbSystem::MINIO;
    return DbSystem::POSTGRESQL;
}

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

    if (j.contains("prometheus")) {
        cfg.prometheus.url = j["prometheus"].value("url", cfg.prometheus.url);
    }

    if (j.contains("grafana")) {
        cfg.grafana.url = j["grafana"].value("url", cfg.grafana.url);
        cfg.grafana.api_key = j["grafana"].value("api_key", cfg.grafana.api_key);
        cfg.grafana.dashboard_uid = j["grafana"].value("dashboard_uid", cfg.grafana.dashboard_uid);
    }

    if (j.contains("databases")) {
        cfg.databases.clear();
        for (const auto& db : j["databases"]) {
            DbConnection conn;
            conn.system = parse_db_system(db.value("system", "postgresql"));
            conn.host = db.value("host", "localhost");
            conn.port = db.value("port", 5432);
            conn.user = db.value("user", cfg.lab_user);
            conn.password = db.value("password", "");
            conn.database = db.value("database", "");
            conn.lab_schema = db.value("lab_schema", cfg.lab_schema);
            cfg.databases.push_back(conn);
        }
    } else {
        cfg.databases = default_k8s_config().databases;
    }

    if (j.contains("dup_grades")) {
        cfg.dup_grades.clear();
        for (const auto& g : j["dup_grades"]) {
            std::string gs = g.get<std::string>();
            if (gs == "U0") cfg.dup_grades.push_back(DupGrade::U0);
            else if (gs == "U50") cfg.dup_grades.push_back(DupGrade::U50);
            else if (gs == "U90") cfg.dup_grades.push_back(DupGrade::U90);
        }
    }

    return cfg;
}

} // namespace dedup
