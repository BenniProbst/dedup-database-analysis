// =============================================================================
// DB-Internal Metrics -- Stage-boundary snapshots
// See db_internal_metrics.hpp for documentation.
//
// Each snapshot function opens a short-lived connection, queries
// system-specific internal metrics, and returns a JSON object.
// These are called TWICE per stage (before + after) in data_loader.cpp.
// =============================================================================

#include "db_internal_metrics.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <sstream>
#include <libpq-fe.h>
#include <curl/curl.h>

#ifdef HAS_HIREDIS
#include <hiredis/hiredis.h>
#endif

#ifdef HAS_MYSQL
#include <mysql/mysql.h>
#endif

namespace dedup {
namespace db_internal {

// --- curl helpers (lightweight duplicates from metrics_trace.cpp) ------------

static size_t curl_cb(char* ptr, size_t sz, size_t n, std::string* d) {
    d->append(ptr, sz * n);
    return sz * n;
}

static std::string http_get(const std::string& url, long timeout_s = 5) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return (rc == CURLE_OK) ? resp : "";
}

static std::string http_post(const std::string& url, const std::string& body,
                              long timeout_s = 5) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return (rc == CURLE_OK) ? resp : "";
}

// --- dispatcher --------------------------------------------------------------

nlohmann::json snapshot(const DbConnection& conn) {
    nlohmann::json j;
    j["system"] = db_system_str(conn.system);
    j["timestamp_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

#ifdef DEDUP_DRY_RUN
    j["data"] = nlohmann::json::object();
    j["dry_run"] = true;
    return j;
#endif

    try {
        nlohmann::json data;
        switch (conn.system) {
            case DbSystem::POSTGRESQL:  data = snapshot_postgresql(conn); break;
            case DbSystem::COCKROACHDB: data = snapshot_cockroachdb(conn); break;
            case DbSystem::REDIS:       data = snapshot_redis(conn); break;
            case DbSystem::KAFKA:       data = snapshot_kafka(conn); break;
            case DbSystem::MINIO:       data = snapshot_minio(conn); break;
            case DbSystem::MARIADB:     data = snapshot_mariadb(conn); break;
            case DbSystem::CLICKHOUSE:  data = snapshot_clickhouse(conn); break;
            case DbSystem::COMDARE_DB:  break; // REST API handled by connector
        }
        j["data"] = data;
    } catch (const std::exception& e) {
        j["error"] = e.what();
        LOG_ERR("[db_internal] snapshot %s failed: %s",
            db_system_str(conn.system), e.what());
    }

    return j;
}

// =============================================================================
// PostgreSQL: pg_total_relation_size, pg_statio_all_tables, pg_stat_statements
// =============================================================================

nlohmann::json snapshot_postgresql(const DbConnection& conn) {
    nlohmann::json j;

    std::string cs = "host=" + conn.host + " port=" + std::to_string(conn.port)
        + " dbname=" + (conn.database.empty() ? "postgres" : conn.database)
        + " user=" + conn.user + " connect_timeout=5";
    if (!conn.password.empty()) cs += " password=" + conn.password;

    PGconn* pg = PQconnectdb(cs.c_str());
    if (PQstatus(pg) != CONNECTION_OK) {
        j["error"] = PQerrorMessage(pg);
        PQfinish(pg);
        return j;
    }

    std::string schema = conn.lab_schema.empty() ? "dedup_lab" : conn.lab_schema;

    // pg_total_relation_size for lab schema (includes indexes + TOAST)
    {
        std::string sql = "SELECT COALESCE(SUM(pg_total_relation_size(c.oid)), 0) "
            "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = '" + schema + "' AND c.relkind = 'r'";
        PGresult* r = PQexec(pg, sql.c_str());
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
            j["total_relation_size_bytes"] = std::strtoll(PQgetvalue(r, 0, 0), nullptr, 10);
        PQclear(r);
    }

    // pg_statio_all_tables: heap + TOAST I/O counters for lab schema
    {
        std::string sql = "SELECT COALESCE(SUM(heap_blks_read), 0), "
            "COALESCE(SUM(heap_blks_hit), 0), "
            "COALESCE(SUM(toast_blks_read), 0), "
            "COALESCE(SUM(toast_blks_hit), 0) "
            "FROM pg_statio_all_tables WHERE schemaname = '" + schema + "'";
        PGresult* r = PQexec(pg, sql.c_str());
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            j["heap_blks_read"] = std::strtoll(PQgetvalue(r, 0, 0), nullptr, 10);
            j["heap_blks_hit"] = std::strtoll(PQgetvalue(r, 0, 1), nullptr, 10);
            j["toast_blks_read"] = std::strtoll(PQgetvalue(r, 0, 2), nullptr, 10);
            j["toast_blks_hit"] = std::strtoll(PQgetvalue(r, 0, 3), nullptr, 10);
        }
        PQclear(r);
    }

    // pg_stat_statements: top 5 queries by total_exec_time (requires extension)
    {
        PGresult* r = PQexec(pg,
            "SELECT query, calls, total_exec_time, mean_exec_time "
            "FROM pg_stat_statements ORDER BY total_exec_time DESC LIMIT 5");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            nlohmann::json stmts = nlohmann::json::array();
            for (int i = 0; i < PQntuples(r); i++) {
                stmts.push_back({
                    {"query", PQgetvalue(r, i, 0)},
                    {"calls", std::strtoll(PQgetvalue(r, i, 1), nullptr, 10)},
                    {"total_exec_time_ms", std::strtod(PQgetvalue(r, i, 2), nullptr)},
                    {"mean_exec_time_ms", std::strtod(PQgetvalue(r, i, 3), nullptr)}
                });
            }
            j["top_statements"] = stmts;
        }
        PQclear(r);
    }

    PQfinish(pg);
    return j;
}

// =============================================================================
// CockroachDB: pg_database_size, crdb_internal.kv_store_status, SHOW RANGES
// =============================================================================

nlohmann::json snapshot_cockroachdb(const DbConnection& conn) {
    nlohmann::json j;

    std::string cs = "host=" + conn.host + " port=" + std::to_string(conn.port)
        + " dbname=" + (conn.database.empty() ? "defaultdb" : conn.database)
        + " user=" + conn.user + " sslmode=require connect_timeout=5";
    if (!conn.password.empty()) cs += " password=" + conn.password;

    PGconn* pg = PQconnectdb(cs.c_str());
    if (PQstatus(pg) != CONNECTION_OK) {
        j["error"] = PQerrorMessage(pg);
        PQfinish(pg);
        return j;
    }

    std::string db = conn.database.empty() ? "defaultdb" : conn.database;

    // Database size via pg_database_size (CockroachDB supports this)
    {
        std::string sql = "SELECT pg_database_size('" + db + "')";
        PGresult* r = PQexec(pg, sql.c_str());
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
            j["database_size_bytes"] = std::strtoll(PQgetvalue(r, 0, 0), nullptr, 10);
        PQclear(r);
    }

    // crdb_internal.kv_store_status aggregates (cluster-wide storage metrics)
    {
        PGresult* r = PQexec(pg,
            "SELECT SUM(live_bytes)::bigint, SUM(key_bytes)::bigint, "
            "SUM(val_bytes)::bigint, SUM(intent_bytes)::bigint, "
            "SUM(sys_bytes)::bigint, SUM(range_count)::bigint "
            "FROM crdb_internal.kv_store_status");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            auto get = [&](int col) -> int64_t {
                char* v = PQgetvalue(r, 0, col);
                return (v && v[0]) ? std::strtoll(v, nullptr, 10) : 0;
            };
            j["kv_live_bytes"] = get(0);
            j["kv_key_bytes"] = get(1);
            j["kv_val_bytes"] = get(2);
            j["kv_intent_bytes"] = get(3);
            j["kv_sys_bytes"] = get(4);
            j["kv_range_count"] = get(5);
        }
        PQclear(r);
    }

    // SHOW RANGES for lab database (per-DB range stats, CockroachDB 22.2+)
    {
        std::string sql = "SELECT count(*)::bigint, "
            "COALESCE(sum(range_size_mb), 0)::double precision "
            "FROM [SHOW RANGES FROM DATABASE " + db + "]";
        PGresult* r = PQexec(pg, sql.c_str());
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            char* v0 = PQgetvalue(r, 0, 0);
            char* v1 = PQgetvalue(r, 0, 1);
            if (v0 && v0[0]) j["db_range_count"] = std::strtoll(v0, nullptr, 10);
            if (v1 && v1[0]) j["db_range_size_mb"] = std::strtod(v1, nullptr);
        }
        PQclear(r);
    }

    PQfinish(pg);
    return j;
}

// =============================================================================
// Redis: MEMORY STATS decomposition, DBSIZE
// =============================================================================

nlohmann::json snapshot_redis(const DbConnection& conn) {
    nlohmann::json j;

#ifdef HAS_HIREDIS
    struct timeval tv = {3, 0};
    redisContext* ctx = redisConnectWithTimeout(conn.host.c_str(), conn.port, tv);
    if (!ctx || ctx->err) {
        if (ctx) { j["error"] = ctx->errstr; redisFree(ctx); }
        return j;
    }

    // DBSIZE (total key count)
    {
        redisReply* reply = static_cast<redisReply*>(redisCommand(ctx, "DBSIZE"));
        if (reply && reply->type == REDIS_REPLY_INTEGER)
            j["dbsize"] = reply->integer;
        if (reply) freeReplyObject(reply);
    }

    // MEMORY STATS (Redis 4.0+ memory breakdown)
    {
        redisReply* reply = static_cast<redisReply*>(redisCommand(ctx, "MEMORY STATS"));
        if (reply && reply->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i + 1 < reply->elements; i += 2) {
                if (reply->element[i]->type != REDIS_REPLY_STRING) continue;
                std::string key(reply->element[i]->str, reply->element[i]->len);
                redisReply* val = reply->element[i + 1];

                if (val->type == REDIS_REPLY_INTEGER) {
                    if (key == "peak.allocated" || key == "total.allocated" ||
                        key == "startup.allocated" || key == "replication.backlog" ||
                        key == "clients.slaves" || key == "clients.normal" ||
                        key == "aof.buffer" || key == "dataset.bytes" ||
                        key == "overhead.total" || key == "keys.count" ||
                        key == "keys.bytes-per-key" || key == "fragmentation.bytes") {
                        j[key] = val->integer;
                    }
                } else if (val->type == REDIS_REPLY_STRING) {
                    if (key == "allocator.fragmentation.ratio" ||
                        key == "rss-overhead.ratio") {
                        j[key] = std::strtod(val->str, nullptr);
                    }
                }
            }
        }
        if (reply) freeReplyObject(reply);
    }

    redisFree(ctx);
#endif

    return j;
}

// =============================================================================
// Kafka: JMX RequestMetrics timing, log sizes (via Prometheus exporter)
// =============================================================================

nlohmann::json snapshot_kafka(const DbConnection& conn) {
    nlohmann::json j;

    // JMX RequestMetrics from Strimzi Prometheus exporter (port 9404)
    std::string body = http_get("http://" + conn.host + ":9404/metrics", 5);
    if (body.empty()) return j;

    std::istringstream iss(body);
    std::string line;

    double produce_total = 0, produce_queue = 0, produce_local = 0;
    double produce_remote = 0, produce_response_send = 0;
    double fetch_total = 0;
    double log_size = 0;
    int64_t messages_in = 0;

    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t sp = line.rfind(' ');
        if (sp == std::string::npos) continue;
        double value = std::strtod(line.c_str() + sp + 1, nullptr);
        std::string metric = line.substr(0, sp);

        // 5-component timing decomposition for Produce requests (p50)
        bool is_produce = metric.find("request=\"Produce\"") != std::string::npos;
        bool is_fetch = metric.find("request=\"Fetch\"") != std::string::npos;
        bool is_p50 = metric.find("quantile=\"0.5\"") != std::string::npos;

        if (is_produce && is_p50) {
            if (metric.find("TotalTimeMs") != std::string::npos)
                produce_total = value;
            else if (metric.find("RequestQueueTimeMs") != std::string::npos)
                produce_queue = value;
            else if (metric.find("LocalTimeMs") != std::string::npos)
                produce_local = value;
            else if (metric.find("RemoteTimeMs") != std::string::npos)
                produce_remote = value;
            else if (metric.find("ResponseSendTimeMs") != std::string::npos)
                produce_response_send = value;
        }

        if (is_fetch && is_p50) {
            if (metric.find("TotalTimeMs") != std::string::npos)
                fetch_total = value;
        }

        // Log sizes and message counts
        size_t brace = metric.find('{');
        std::string bare = (brace != std::string::npos) ? metric.substr(0, brace) : metric;

        if (bare == "kafka_log_Log_Size")
            log_size += value;
        else if (bare == "kafka_server_BrokerTopicMetrics_MessagesInPerSec_Count")
            messages_in = static_cast<int64_t>(value);
    }

    j["produce_total_time_p50_ms"] = produce_total;
    j["produce_queue_time_p50_ms"] = produce_queue;
    j["produce_local_time_p50_ms"] = produce_local;
    j["produce_remote_time_p50_ms"] = produce_remote;
    j["produce_response_send_time_p50_ms"] = produce_response_send;
    j["fetch_total_time_p50_ms"] = fetch_total;
    j["total_log_size_bytes"] = static_cast<int64_t>(log_size);
    j["messages_in_total"] = messages_in;

    return j;
}

// =============================================================================
// MinIO: TTFB distribution, per-bucket sizes (via Prometheus endpoint)
// =============================================================================

nlohmann::json snapshot_minio(const DbConnection& conn) {
    nlohmann::json j;

    std::string url = "http://" + conn.host + ":" + std::to_string(conn.port)
                      + "/minio/v2/metrics/cluster";
    std::string body = http_get(url, 5);
    if (body.empty()) return j;

    std::istringstream iss(body);
    std::string line;

    double bucket_total_bytes = 0, bucket_objects = 0;
    double ttfb_sum = 0, ttfb_count = 0;
    double rx_total = 0, tx_total = 0;

    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t sp = line.rfind(' ');
        if (sp == std::string::npos) continue;
        double value = std::strtod(line.c_str() + sp + 1, nullptr);
        std::string metric = line.substr(0, sp);

        size_t brace = metric.find('{');
        std::string bare = (brace != std::string::npos) ? metric.substr(0, brace) : metric;

        if (bare == "minio_bucket_usage_total_bytes") bucket_total_bytes += value;
        else if (bare == "minio_bucket_usage_object_total") bucket_objects += value;
        else if (bare == "minio_s3_ttfb_seconds_distribution_sum") ttfb_sum += value;
        else if (bare == "minio_s3_ttfb_seconds_distribution_count") ttfb_count += value;
        else if (bare == "minio_s3_rx_bytes_total") rx_total += value;
        else if (bare == "minio_s3_tx_bytes_total") tx_total += value;
    }

    j["bucket_total_bytes"] = static_cast<int64_t>(bucket_total_bytes);
    j["bucket_objects"] = static_cast<int64_t>(bucket_objects);
    j["s3_rx_bytes_total"] = static_cast<int64_t>(rx_total);
    j["s3_tx_bytes_total"] = static_cast<int64_t>(tx_total);
    if (ttfb_count > 0) {
        j["s3_ttfb_mean_seconds"] = ttfb_sum / ttfb_count;
        j["s3_ttfb_count"] = static_cast<int64_t>(ttfb_count);
    }

    return j;
}

// =============================================================================
// MariaDB: INNODB_TABLESPACES, SHOW TABLE STATUS, performance_schema IO waits
// =============================================================================

nlohmann::json snapshot_mariadb(const DbConnection& conn) {
    nlohmann::json j;

#ifdef HAS_MYSQL
    MYSQL* my = mysql_init(nullptr);
    if (!my) return j;

    unsigned int timeout = 3;
    mysql_options(my, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!mysql_real_connect(my, conn.host.c_str(), conn.user.c_str(),
                            conn.password.c_str(), conn.database.c_str(),
                            conn.port, nullptr, 0)) {
        j["error"] = mysql_error(my);
        mysql_close(my);
        return j;
    }

    std::string db = conn.database.empty() ? "dedup_lab" : conn.database;

    // INNODB_TABLESPACES: file sizes for lab database tables
    {
        std::string sql = "SELECT COALESCE(SUM(FILE_SIZE), 0), "
            "COALESCE(SUM(ALLOCATED_SIZE), 0) "
            "FROM information_schema.INNODB_TABLESPACES "
            "WHERE NAME LIKE '" + db + "/%'";
        if (mysql_query(my, sql.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(my);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row) {
                    j["innodb_file_size_bytes"] = std::strtoll(row[0], nullptr, 10);
                    j["innodb_allocated_size_bytes"] = std::strtoll(row[1], nullptr, 10);
                }
                mysql_free_result(res);
            }
        }
    }

    // SHOW TABLE STATUS: per-table size breakdown
    {
        std::string sql = "SHOW TABLE STATUS FROM `" + db + "`";
        if (mysql_query(my, sql.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(my);
            if (res) {
                nlohmann::json tables = nlohmann::json::array();
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    nlohmann::json t;
                    t["name"] = row[0] ? row[0] : "";
                    t["rows"] = row[4] ? std::strtoll(row[4], nullptr, 10) : 0;
                    t["data_length"] = row[6] ? std::strtoll(row[6], nullptr, 10) : 0;
                    t["index_length"] = row[8] ? std::strtoll(row[8], nullptr, 10) : 0;
                    t["data_free"] = row[9] ? std::strtoll(row[9], nullptr, 10) : 0;
                    tables.push_back(t);
                }
                j["tables"] = tables;
                mysql_free_result(res);
            }
        }
    }

    // performance_schema: InnoDB IO wait events (top 5)
    {
        if (mysql_query(my,
                "SELECT EVENT_NAME, "
                "SUM_TIMER_WAIT / 1000000000 AS total_ms, "
                "COUNT_STAR "
                "FROM performance_schema.events_waits_summary_global_by_event_name "
                "WHERE EVENT_NAME LIKE 'wait/io/file/innodb%' "
                "ORDER BY SUM_TIMER_WAIT DESC LIMIT 5") == 0) {
            MYSQL_RES* res = mysql_store_result(my);
            if (res) {
                nlohmann::json waits = nlohmann::json::array();
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    waits.push_back({
                        {"event", row[0] ? row[0] : ""},
                        {"total_ms", row[1] ? std::strtod(row[1], nullptr) : 0.0},
                        {"count", row[2] ? std::strtoll(row[2], nullptr, 10) : 0}
                    });
                }
                if (!waits.empty()) j["innodb_wait_events"] = waits;
                mysql_free_result(res);
            }
        }
    }

    mysql_close(my);
#endif

    return j;
}

// =============================================================================
// ClickHouse: system.columns compression ratio, system.parts
// =============================================================================

nlohmann::json snapshot_clickhouse(const DbConnection& conn) {
    nlohmann::json j;

    std::string base = "http://" + conn.host + ":" + std::to_string(conn.port);
    std::string db = conn.database.empty() ? "dedup_lab" : conn.database;

    // system.columns: compressed vs uncompressed per table
    {
        std::string sql = "SELECT table, "
            "sum(data_compressed_bytes) AS compressed, "
            "sum(data_uncompressed_bytes) AS uncompressed "
            "FROM system.columns WHERE database = '" + db + "' "
            "GROUP BY table FORMAT JSONCompact";
        std::string resp = http_post(base + "/", sql, 5);
        if (!resp.empty()) {
            try {
                auto parsed = nlohmann::json::parse(resp);
                nlohmann::json tables = nlohmann::json::array();
                int64_t total_compressed = 0, total_uncompressed = 0;
                for (const auto& row : parsed["data"]) {
                    int64_t c = row[1].get<int64_t>();
                    int64_t u = row[2].get<int64_t>();
                    tables.push_back({
                        {"table", row[0].get<std::string>()},
                        {"compressed_bytes", c},
                        {"uncompressed_bytes", u},
                        {"ratio", c > 0 ? static_cast<double>(u) / c : 0.0}
                    });
                    total_compressed += c;
                    total_uncompressed += u;
                }
                j["column_tables"] = tables;
                j["total_compressed_bytes"] = total_compressed;
                j["total_uncompressed_bytes"] = total_uncompressed;
                if (total_compressed > 0)
                    j["compression_ratio"] = static_cast<double>(total_uncompressed) / total_compressed;
            } catch (...) {}
        }
    }

    // system.parts: active parts count and sizes per table
    {
        std::string sql = "SELECT table, "
            "count() AS parts, "
            "sum(rows) AS total_rows, "
            "sum(bytes_on_disk) AS bytes_on_disk "
            "FROM system.parts WHERE database = '" + db + "' AND active "
            "GROUP BY table FORMAT JSONCompact";
        std::string resp = http_post(base + "/", sql, 5);
        if (!resp.empty()) {
            try {
                auto parsed = nlohmann::json::parse(resp);
                nlohmann::json parts = nlohmann::json::array();
                for (const auto& row : parsed["data"]) {
                    parts.push_back({
                        {"table", row[0].get<std::string>()},
                        {"active_parts", row[1].get<int64_t>()},
                        {"total_rows", row[2].get<int64_t>()},
                        {"bytes_on_disk", row[3].get<int64_t>()}
                    });
                }
                j["parts"] = parts;
            } catch (...) {}
        }
    }

    return j;
}

} // namespace db_internal
} // namespace dedup
