// =============================================================================
// MetricsTrace -- 100ms background sampling of ALL DB metrics
//
// Implementation: Kafka producer (librdkafka), sampling loop, 7 collectors.
// Each collector opens a short-lived connection per cycle. For K8s-internal
// services this adds ~2-10ms per system (acceptable for 100ms interval).
//
// Collectors query native metric sources:
//   PostgreSQL:  pg_stat_* via libpq
//   CockroachDB: crdb_internal.* via libpq + /_status/vars via HTTP
//   Redis:       INFO ALL via hiredis
//   Kafka:       JMX exporter metrics via HTTP (port 9404)
//   MinIO:       /minio/v2/metrics/cluster via HTTP
//   MariaDB:     SHOW GLOBAL STATUS via libmysqlclient
//   ClickHouse:  system.metrics/events via HTTP API
// =============================================================================

#include "metrics_trace.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <sstream>
#include <nlohmann/json.hpp>
#include <libpq-fe.h>
#include <curl/curl.h>

#ifdef HAS_HIREDIS
#include <hiredis/hiredis.h>
#endif

#ifdef HAS_RDKAFKA
#include <librdkafka/rdkafka.h>
#endif

#ifdef HAS_MYSQL
#include <mysql/mysql.h>
#endif

namespace dedup {

// now_ms() is defined inline in metrics_trace.hpp

// ---------------------------------------------------------------------------
// MetricPoint / ExperimentEvent JSON serialization
// ---------------------------------------------------------------------------

std::string MetricPoint::to_json() const {
    nlohmann::json j;
    j["ts"]     = timestamp_ms;
    j["system"] = system;
    j["metric"] = metric_name;
    j["value"]  = value;
    j["unit"]   = unit;
    return j.dump();
}

std::string ExperimentEvent::to_json() const {
    nlohmann::json j;
    j["ts"]        = timestamp_ms;
    j["event"]     = event_type;
    j["system"]    = system;
    j["dup_grade"] = dup_grade;
    j["stage"]     = stage;
    j["detail"]    = detail;
    return j.dump();
}

// ---------------------------------------------------------------------------
// MetricsTrace lifecycle
// ---------------------------------------------------------------------------

MetricsTrace::MetricsTrace(const MetricsTraceConfig& config, bool dry_run)
    : config_(config), dry_run_(dry_run)
{
#ifdef HAS_RDKAFKA
    if (!dry_run_ && config_.enabled) {
        rd_kafka_conf_t* conf = rd_kafka_conf_new();
        char errstr[512];

        rd_kafka_conf_set(conf, "bootstrap.servers",
            config_.kafka_bootstrap.c_str(), errstr, sizeof(errstr));
        rd_kafka_conf_set(conf, "queue.buffering.max.ms", "10",
            errstr, sizeof(errstr));
        rd_kafka_conf_set(conf, "batch.num.messages", "100",
            errstr, sizeof(errstr));

        kafka_producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
        if (!kafka_producer_) {
            LOG_ERR("[metrics_trace] Kafka producer init failed: %s", errstr);
        } else {
            LOG_INF("[metrics_trace] Kafka producer ready (%s)",
                config_.kafka_bootstrap.c_str());
        }
    }
#endif

    if (dry_run_) {
        LOG_INF("[metrics_trace] DRY RUN -- metrics logged but not sent to Kafka");
    }
}

MetricsTrace::~MetricsTrace() {
    stop();

#ifdef HAS_RDKAFKA
    if (kafka_producer_) {
        rd_kafka_flush(static_cast<rd_kafka_t*>(kafka_producer_), 5000);
        rd_kafka_destroy(static_cast<rd_kafka_t*>(kafka_producer_));
        kafka_producer_ = nullptr;
    }
#endif
}

void MetricsTrace::register_system(const DbConnection& conn,
                                    MetricCollectorFn collector_fn) {
    std::lock_guard<std::mutex> lock(systems_mutex_);
    systems_.push_back({conn, std::move(collector_fn)});
    LOG_INF("[metrics_trace] Registered %s (%s:%u) for %d ms sampling",
        db_system_str(conn.system), conn.host.c_str(), conn.port,
        config_.sample_interval_ms);
}

void MetricsTrace::start() {
    if (running_.load()) return;
    running_.store(true);
    sampling_thread_ = std::thread(&MetricsTrace::sampling_loop, this);
    LOG_INF("[metrics_trace] Started (interval=%d ms, systems=%zu)",
        config_.sample_interval_ms, systems_.size());
}

void MetricsTrace::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (sampling_thread_.joinable()) {
        sampling_thread_.join();
    }
    LOG_INF("[metrics_trace] Stopped (metrics=%lld, events=%lld)",
        static_cast<long long>(metrics_count_.load()),
        static_cast<long long>(events_count_.load()));
}

void MetricsTrace::publish_event(const ExperimentEvent& event) {
    std::string payload = event.to_json();
    std::string key = event.system + "." + event.event_type;

    produce_to_kafka(config_.events_topic, key, payload);
    events_count_.fetch_add(1);

    LOG_INF("[metrics_trace] Event: %s %s %s/%s",
        event.event_type.c_str(), event.system.c_str(),
        event.dup_grade.c_str(), event.stage.c_str());
}

// ---------------------------------------------------------------------------
// Sampling loop (runs on background thread)
// ---------------------------------------------------------------------------

void MetricsTrace::sampling_loop() {
    LOG_DBG("[metrics_trace] Sampling loop running");

    while (running_.load()) {
        auto cycle_start = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(systems_mutex_);
            for (auto& sys : systems_) {
                try {
                    auto points = sys.collector(sys.conn);
                    for (const auto& pt : points) {
                        produce_to_kafka(config_.metrics_topic,
                            pt.system + "." + pt.metric_name, pt.to_json());
                        metrics_count_.fetch_add(1);
                    }
                } catch (const std::exception& e) {
                    LOG_ERR("[metrics_trace] Collector %s error: %s",
                        db_system_str(sys.conn.system), e.what());
                }
            }
        }

        auto elapsed = std::chrono::steady_clock::now() - cycle_start;
        auto target  = std::chrono::milliseconds(config_.sample_interval_ms);
        if (elapsed < target) {
            std::this_thread::sleep_for(target - elapsed);
        }
    }
}

// ---------------------------------------------------------------------------
// Kafka produce
// ---------------------------------------------------------------------------

void MetricsTrace::produce_to_kafka(const std::string& topic,
                                     const std::string& key,
                                     const std::string& payload) {
    if (dry_run_) {
        LOG_DBG("[metrics_trace] DRY %s | %s", topic.c_str(), key.c_str());
        return;
    }

#ifdef HAS_RDKAFKA
    if (!kafka_producer_) return;

    rd_kafka_t* rk = static_cast<rd_kafka_t*>(kafka_producer_);
    rd_kafka_resp_err_t err = rd_kafka_producev(
        rk,
        RD_KAFKA_V_TOPIC(topic.c_str()),
        RD_KAFKA_V_KEY(key.c_str(), key.size()),
        RD_KAFKA_V_VALUE(const_cast<char*>(payload.c_str()), payload.size()),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
        RD_KAFKA_V_END
    );

    if (err) {
        LOG_ERR("[metrics_trace] Kafka produce error: %s", rd_kafka_err2str(err));
    }

    rd_kafka_poll(rk, 0);
#else
    LOG_DBG("[metrics_trace] No librdkafka -- skip %s", topic.c_str());
#endif
}

// =============================================================================
// Built-in metric collectors
// =============================================================================

namespace collectors {

// --- helpers ----------------------------------------------------------------

static MetricPoint mp(const char* sys, const char* metric,
                       double value, const char* unit) {
    return {now_ms(), sys, metric, value, unit};
}

static size_t curl_write_cb(char* ptr, size_t sz, size_t n, std::string* d) {
    d->append(ptr, sz * n);
    return sz * n;
}

static std::string http_get(const std::string& url, long timeout_s = 3) {
    CURL* c = curl_easy_init();
    if (!c) return "";

    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 2L);

    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return (rc == CURLE_OK) ? resp : "";
}

static std::string http_post(const std::string& url, const std::string& body,
                              long timeout_s = 3) {
    CURL* c = curl_easy_init();
    if (!c) return "";

    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 2L);

    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return (rc == CURLE_OK) ? resp : "";
}

// parse a "key:value\r\n" line from Redis INFO output
static double parse_info_val(const std::string& info, const std::string& key) {
    size_t pos = info.find(key + ":");
    if (pos == std::string::npos) return -1;
    return std::strtod(info.c_str() + pos + key.size() + 1, nullptr);
}

// parse Prometheus exposition format: "metric_name{labels} value"
// returns bare name and value
static bool parse_prom_line(const std::string& line,
                             std::string& bare_name, double& value) {
    if (line.empty() || line[0] == '#') return false;
    size_t sp = line.rfind(' ');
    if (sp == std::string::npos) return false;

    std::string full = line.substr(0, sp);
    value = std::strtod(line.c_str() + sp + 1, nullptr);

    size_t brace = full.find('{');
    bare_name = (brace != std::string::npos) ? full.substr(0, brace) : full;
    return true;
}

// --- PostgreSQL -------------------------------------------------------------

std::vector<MetricPoint> collect_postgresql(const DbConnection& conn) {
    std::vector<MetricPoint> pts;

    std::string cs = "host=" + conn.host + " port=" + std::to_string(conn.port)
        + " dbname=" + (conn.database.empty() ? "postgres" : conn.database)
        + " user=" + conn.user + " connect_timeout=3";
    if (!conn.password.empty()) cs += " password=" + conn.password;

    PGconn* pg = PQconnectdb(cs.c_str());
    if (PQstatus(pg) != CONNECTION_OK) { PQfinish(pg); return pts; }

    // pg_database_size
    {
        std::string sql = "SELECT pg_database_size('" + conn.database + "')";
        PGresult* r = PQexec(pg, sql.c_str());
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
            pts.push_back(mp("postgresql", "pg_database_size",
                std::strtod(PQgetvalue(r, 0, 0), nullptr), "bytes"));
        PQclear(r);
    }

    // pg_stat_database
    {
        PGresult* r = PQexec(pg,
            "SELECT xact_commit, xact_rollback, tup_inserted, tup_updated, "
            "tup_deleted, blks_hit, blks_read "
            "FROM pg_stat_database WHERE datname = current_database()");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            static const char* names[] = {
                "xact_commit", "xact_rollback", "tup_inserted",
                "tup_updated", "tup_deleted", "blks_hit", "blks_read"};
            for (int i = 0; i < 7; i++)
                pts.push_back(mp("postgresql", names[i],
                    std::strtod(PQgetvalue(r, 0, i), nullptr), "count"));
        }
        PQclear(r);
    }

    // pg_stat_bgwriter
    {
        PGresult* r = PQexec(pg,
            "SELECT buffers_checkpoint, buffers_clean, buffers_backend "
            "FROM pg_stat_bgwriter");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            pts.push_back(mp("postgresql", "buffers_checkpoint",
                std::strtod(PQgetvalue(r, 0, 0), nullptr), "count"));
            pts.push_back(mp("postgresql", "buffers_clean",
                std::strtod(PQgetvalue(r, 0, 1), nullptr), "count"));
            pts.push_back(mp("postgresql", "buffers_backend",
                std::strtod(PQgetvalue(r, 0, 2), nullptr), "count"));
        }
        PQclear(r);
    }

    // pg_stat_wal (PostgreSQL 14+)
    {
        PGresult* r = PQexec(pg, "SELECT wal_bytes FROM pg_stat_wal");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
            pts.push_back(mp("postgresql", "wal_bytes",
                std::strtod(PQgetvalue(r, 0, 0), nullptr), "bytes"));
        PQclear(r);
    }

    PQfinish(pg);
    return pts;
}

// --- CockroachDB ------------------------------------------------------------

std::vector<MetricPoint> collect_cockroachdb(const DbConnection& conn) {
    std::vector<MetricPoint> pts;

    // SQL via PG wire (TLS required for production CockroachDB)
    std::string cs = "host=" + conn.host + " port=" + std::to_string(conn.port)
        + " dbname=" + (conn.database.empty() ? "defaultdb" : conn.database)
        + " user=" + conn.user + " sslmode=require connect_timeout=3";
    if (!conn.password.empty()) cs += " password=" + conn.password;

    PGconn* pg = PQconnectdb(cs.c_str());
    if (PQstatus(pg) == CONNECTION_OK) {
        PGresult* r = PQexec(pg,
            "SELECT SUM(range_count)::bigint, SUM(replica_count)::bigint, "
            "SUM(lease_holder_count)::bigint, SUM(available)::bigint, "
            "SUM(live_bytes)::bigint, SUM(key_bytes)::bigint "
            "FROM crdb_internal.kv_store_status");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            static const char* names[] = {
                "ranges", "replicas", "leaseholders",
                "available_bytes", "live_bytes", "key_bytes"};
            static const char* units[] = {
                "count", "count", "count", "bytes", "bytes", "bytes"};
            for (int i = 0; i < 6; i++) {
                char* val = PQgetvalue(r, 0, i);
                if (val && val[0])
                    pts.push_back(mp("cockroachdb", names[i],
                        std::strtod(val, nullptr), units[i]));
            }
        }
        PQclear(r);
        PQfinish(pg);
    }

    // HTTP /_status/vars (CockroachDB admin UI port 8080)
    std::string vars = http_get("http://" + conn.host + ":8080/_status/vars", 2);
    if (!vars.empty()) {
        std::istringstream iss(vars);
        std::string line;
        std::string name;
        double value;
        while (std::getline(iss, line)) {
            if (!parse_prom_line(line, name, value)) continue;
            if (name == "sql_query_count")
                pts.push_back(mp("cockroachdb", "queries_total", value, "count"));
        }
    }

    return pts;
}

// --- Redis ------------------------------------------------------------------

std::vector<MetricPoint> collect_redis(const DbConnection& conn) {
    std::vector<MetricPoint> pts;

#ifdef HAS_HIREDIS
    struct timeval tv = {2, 0};
    redisContext* ctx = redisConnectWithTimeout(conn.host.c_str(), conn.port, tv);
    if (!ctx || ctx->err) { if (ctx) redisFree(ctx); return pts; }

    redisReply* reply = static_cast<redisReply*>(redisCommand(ctx, "INFO ALL"));
    if (reply && reply->type == REDIS_REPLY_STRING) {
        std::string info(reply->str, reply->len);
        double v;
        if ((v = parse_info_val(info, "used_memory")) >= 0)
            pts.push_back(mp("redis", "used_memory", v, "bytes"));
        if ((v = parse_info_val(info, "used_memory_rss")) >= 0)
            pts.push_back(mp("redis", "used_memory_rss", v, "bytes"));
        if ((v = parse_info_val(info, "keyspace_hits")) >= 0)
            pts.push_back(mp("redis", "keyspace_hits", v, "count"));
        if ((v = parse_info_val(info, "keyspace_misses")) >= 0)
            pts.push_back(mp("redis", "keyspace_misses", v, "count"));
        if ((v = parse_info_val(info, "instantaneous_ops_per_sec")) >= 0)
            pts.push_back(mp("redis", "instantaneous_ops_per_sec", v, "rate"));
        if ((v = parse_info_val(info, "total_connections_received")) >= 0)
            pts.push_back(mp("redis", "total_connections_received", v, "count"));
        if ((v = parse_info_val(info, "connected_clients")) >= 0)
            pts.push_back(mp("redis", "connected_clients", v, "count"));
    }
    if (reply) freeReplyObject(reply);
    redisFree(ctx);
#endif

    return pts;
}

// --- Kafka ------------------------------------------------------------------

std::vector<MetricPoint> collect_kafka(const DbConnection& conn) {
    std::vector<MetricPoint> pts;

    // Strimzi Kafka exposes JMX metrics via Prometheus exporter on port 9404
    std::string body = http_get("http://" + conn.host + ":9404/metrics", 2);
    if (body.empty()) return pts;

    std::istringstream iss(body);
    std::string line, name;
    double value;
    while (std::getline(iss, line)) {
        if (!parse_prom_line(line, name, value)) continue;

        if (name == "kafka_server_BrokerTopicMetrics_BytesInPerSec_Count")
            pts.push_back(mp("kafka", "bytes_in_per_sec", value, "bytes/s"));
        else if (name == "kafka_server_BrokerTopicMetrics_BytesOutPerSec_Count")
            pts.push_back(mp("kafka", "bytes_out_per_sec", value, "bytes/s"));
        else if (name == "kafka_server_ReplicaManager_UnderReplicatedPartitions_Value")
            pts.push_back(mp("kafka", "under_replicated_partitions", value, "count"));
        else if (name == "kafka_log_Log_Size")
            pts.push_back(mp("kafka", "log_size", value, "bytes"));
    }

    return pts;
}

// --- MinIO ------------------------------------------------------------------

std::vector<MetricPoint> collect_minio(const DbConnection& conn) {
    std::vector<MetricPoint> pts;

    std::string url = "http://" + conn.host + ":" + std::to_string(conn.port)
                      + "/minio/v2/metrics/cluster";
    std::string body = http_get(url, 2);
    if (body.empty()) return pts;

    std::istringstream iss(body);
    std::string line, name;
    double value;
    while (std::getline(iss, line)) {
        if (!parse_prom_line(line, name, value)) continue;

        if (name == "minio_bucket_usage_total_bytes")
            pts.push_back(mp("minio", "bucket_usage_total_bytes", value, "bytes"));
        else if (name == "minio_s3_requests_total")
            pts.push_back(mp("minio", "s3_requests_total", value, "count"));
        else if (name == "minio_s3_rx_bytes_total")
            pts.push_back(mp("minio", "s3_rx_bytes_total", value, "bytes"));
        else if (name == "minio_s3_tx_bytes_total")
            pts.push_back(mp("minio", "s3_tx_bytes_total", value, "bytes"));
    }

    return pts;
}

// --- MariaDB ----------------------------------------------------------------

std::vector<MetricPoint> collect_mariadb(const DbConnection& conn) {
    std::vector<MetricPoint> pts;

#ifdef HAS_MYSQL
    MYSQL* my = mysql_init(nullptr);
    if (!my) return pts;

    unsigned int timeout = 2;
    mysql_options(my, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!mysql_real_connect(my, conn.host.c_str(), conn.user.c_str(),
                            conn.password.c_str(), conn.database.c_str(),
                            conn.port, nullptr, 0)) {
        mysql_close(my);
        return pts;
    }

    if (mysql_query(my, "SHOW GLOBAL STATUS") == 0) {
        MYSQL_RES* res = mysql_store_result(my);
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                std::string n(row[0]);
                double v = std::strtod(row[1], nullptr);

                if (n == "Innodb_buffer_pool_reads")
                    pts.push_back(mp("mariadb", "innodb_buffer_pool_reads", v, "count"));
                else if (n == "Innodb_data_written")
                    pts.push_back(mp("mariadb", "innodb_data_written", v, "bytes"));
                else if (n == "Threads_connected")
                    pts.push_back(mp("mariadb", "threads_connected", v, "count"));
                else if (n == "Bytes_received")
                    pts.push_back(mp("mariadb", "bytes_received", v, "bytes"));
                else if (n == "Bytes_sent")
                    pts.push_back(mp("mariadb", "bytes_sent", v, "bytes"));
                else if (n == "Com_insert")
                    pts.push_back(mp("mariadb", "com_insert", v, "count"));
                else if (n == "Com_delete")
                    pts.push_back(mp("mariadb", "com_delete", v, "count"));
            }
            mysql_free_result(res);
        }
    }

    mysql_close(my);
#endif

    return pts;
}

// --- ClickHouse -------------------------------------------------------------

std::vector<MetricPoint> collect_clickhouse(const DbConnection& conn) {
    std::vector<MetricPoint> pts;

    std::string base = "http://" + conn.host + ":" + std::to_string(conn.port);

    // system.metrics
    {
        std::string resp = http_post(base + "/",
            "SELECT metric, value FROM system.metrics WHERE metric IN "
            "('Query', 'Merge', 'ReplicatedFetch') FORMAT JSONCompact", 2);
        if (!resp.empty()) {
            try {
                auto j = nlohmann::json::parse(resp);
                for (const auto& row : j["data"]) {
                    pts.push_back(mp("clickhouse",
                        ("metrics_" + row[0].get<std::string>()).c_str(),
                        row[1].get<double>(), "count"));
                }
            } catch (...) {}
        }
    }

    // system.events
    {
        std::string resp = http_post(base + "/",
            "SELECT event, value FROM system.events WHERE event IN "
            "('InsertedRows', 'InsertedBytes') FORMAT JSONCompact", 2);
        if (!resp.empty()) {
            try {
                auto j = nlohmann::json::parse(resp);
                for (const auto& row : j["data"]) {
                    std::string ev = row[0].get<std::string>();
                    const char* unit = (ev == "InsertedBytes") ? "bytes" : "count";
                    pts.push_back(mp("clickhouse",
                        ("events_" + ev).c_str(), row[1].get<double>(), unit));
                }
            } catch (...) {}
        }
    }

    return pts;
}

// --- Factory ----------------------------------------------------------------

MetricCollectorFn for_system(DbSystem system) {
    switch (system) {
        case DbSystem::POSTGRESQL:  return collect_postgresql;
        case DbSystem::COCKROACHDB: return collect_cockroachdb;
        case DbSystem::REDIS:       return collect_redis;
        case DbSystem::KAFKA:       return collect_kafka;
        case DbSystem::MINIO:       return collect_minio;
        case DbSystem::MARIADB:     return collect_mariadb;
        case DbSystem::CLICKHOUSE:  return collect_clickhouse;
    }
    return collect_postgresql;
}

} // namespace collectors
} // namespace dedup
