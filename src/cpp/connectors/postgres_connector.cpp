#include "postgres_connector.hpp"
#include "../utils/timer.hpp"
#include "../utils/logger.hpp"
#include "../utils/sha256.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dedup {
namespace fs = std::filesystem;

bool PostgresConnector::connect(const DbConnection& conn) {
    schema_ = conn.lab_schema;

    // Build connection string
    // For CockroachDB: same PG wire protocol, works with libpq
    char conninfo[512];
    std::snprintf(conninfo, sizeof(conninfo),
        "host=%s port=%u dbname=%s user=%s password=%s "
        "connect_timeout=10 application_name=dedup-test",
        conn.host.c_str(), conn.port,
        conn.database.c_str(), conn.user.c_str(), conn.password.c_str());

    conn_ = PQconnectdb(conninfo);
    if (PQstatus(conn_) != CONNECTION_OK) {
        LOG_ERR("[%s] Connection failed: %s", system_name(), PQerrorMessage(conn_));
#ifdef DEDUP_DRY_RUN
        LOG_INF("[%s] DRY RUN: simulating connection", system_name());
        PQfinish(conn_);
        conn_ = nullptr;
        return true;
#endif
        PQfinish(conn_);
        conn_ = nullptr;
        return false;
    }

    LOG_INF("[%s] Connected to %s:%u/%s (server %s)",
        system_name(), conn.host.c_str(), conn.port,
        conn.database.c_str(), PQparameterStatus(conn_, "server_version"));
    return true;
}

void PostgresConnector::disconnect() {
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

bool PostgresConnector::is_connected() const {
#ifdef DEDUP_DRY_RUN
    return true;
#endif
    return conn_ && PQstatus(conn_) == CONNECTION_OK;
}

bool PostgresConnector::exec(const char* sql) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[%s] DRY RUN SQL: %s", system_name(), sql);
    return true;
#endif
    if (!conn_) return false;
    PGresult* res = PQexec(conn_, sql);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK ||
               PQresultStatus(res) == PGRES_TUPLES_OK);
    if (!ok) {
        LOG_ERR("[%s] SQL error: %s\n  SQL: %s", system_name(), PQerrorMessage(conn_), sql);
    }
    PQclear(res);
    return ok;
}

PGresult* PostgresConnector::query(const char* sql) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[%s] DRY RUN query: %s", system_name(), sql);
    return nullptr;
#endif
    if (!conn_) return nullptr;
    PGresult* res = PQexec(conn_, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERR("[%s] Query error: %s", system_name(), PQerrorMessage(conn_));
        PQclear(res);
        return nullptr;
    }
    return res;
}

// --- Lab schema management (CRITICAL: production safety!) ---

bool PostgresConnector::create_lab_schema(const std::string& schema_name) {
    LOG_INF("[%s] Creating lab schema: %s", system_name(), schema_name.c_str());

    char sql[256];
    if (system_ == DbSystem::COCKROACHDB) {
        // CockroachDB: use CREATE DATABASE instead of schema
        std::snprintf(sql, sizeof(sql),
            "CREATE DATABASE IF NOT EXISTS %s", schema_name.c_str());
    } else {
        std::snprintf(sql, sizeof(sql),
            "CREATE SCHEMA IF NOT EXISTS %s", schema_name.c_str());
    }
    if (!exec(sql)) return false;

    // Create the files table in lab schema
    // Schema: files(id UUID PK, mime TEXT, size_bytes BIGINT, sha256 BYTEA, payload BYTEA)
    if (system_ == DbSystem::COCKROACHDB) {
        std::snprintf(sql, sizeof(sql),
            "CREATE TABLE IF NOT EXISTS %s.files ("
            "  id UUID PRIMARY KEY DEFAULT gen_random_uuid(),"
            "  mime TEXT NOT NULL,"
            "  size_bytes BIGINT NOT NULL,"
            "  sha256 BYTEA NOT NULL,"
            "  payload BYTEA NOT NULL,"
            "  inserted_at TIMESTAMPTZ DEFAULT now()"
            ")", schema_name.c_str());
    } else {
        std::snprintf(sql, sizeof(sql),
            "CREATE TABLE IF NOT EXISTS %s.files ("
            "  id UUID PRIMARY KEY DEFAULT gen_random_uuid(),"
            "  mime TEXT NOT NULL,"
            "  size_bytes BIGINT NOT NULL,"
            "  sha256 BYTEA NOT NULL,"
            "  payload BYTEA NOT NULL,"
            "  inserted_at TIMESTAMPTZ DEFAULT now()"
            ")", schema_name.c_str());
    }
    return exec(sql);
}

bool PostgresConnector::drop_lab_schema(const std::string& schema_name) {
    LOG_WRN("[%s] DROPPING lab schema: %s (all lab data will be lost!)",
        system_name(), schema_name.c_str());

    char sql[256];
    if (system_ == DbSystem::COCKROACHDB) {
        std::snprintf(sql, sizeof(sql),
            "DROP DATABASE IF EXISTS %s CASCADE", schema_name.c_str());
    } else {
        std::snprintf(sql, sizeof(sql),
            "DROP SCHEMA IF EXISTS %s CASCADE", schema_name.c_str());
    }
    return exec(sql);
}

bool PostgresConnector::reset_lab_schema(const std::string& schema_name) {
    LOG_INF("[%s] Resetting lab schema: %s", system_name(), schema_name.c_str());
    return drop_lab_schema(schema_name) && create_lab_schema(schema_name);
}

// --- Data operations ---

MeasureResult PostgresConnector::bulk_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);

    LOG_INF("[%s] Bulk insert from %s", system_name(), dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[%s] DRY RUN: would bulk-insert files from %s", system_name(), dir.c_str());
    result.duration_ns = 0;
    result.rows_affected = 42;
    return result;
#endif

    Timer timer;
    timer.start();

    // Use COPY for maximum PostgreSQL bulk-load performance
    char copy_sql[512];
    std::snprintf(copy_sql, sizeof(copy_sql),
        "COPY %s.files (mime, size_bytes, sha256, payload) FROM STDIN WITH (FORMAT binary)",
        schema_.c_str());

    // For each file in the data directory, read and insert via COPY
    // TODO: implement binary COPY protocol for maximum throughput
    // For now, use prepared statements as fallback
    exec("BEGIN");

    char insert_sql[512];
    std::snprintf(insert_sql, sizeof(insert_sql),
        "INSERT INTO %s.files (mime, size_bytes, sha256, payload) "
        "VALUES ($1, $2, $3, $4)", schema_.c_str());

    if (conn_) {
        PGresult* prep = PQprepare(conn_, "bulk_insert", insert_sql, 4, nullptr);
        PQclear(prep);
    }

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;

        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        // Compute SHA-256 fingerprint for dedup detection
        std::string sha256_hex = SHA256::hash_hex(buf.data(), fsize);

        const char* values[4] = {
            "application/octet-stream",
            std::to_string(fsize).c_str(),
            sha256_hex.c_str(),
            buf.data()
        };
        int lengths[4] = {0, 0, 0, static_cast<int>(fsize)};
        int formats[4] = {0, 0, 0, 1};  // payload is binary

        if (conn_) {
            PGresult* res = PQexecPrepared(conn_, "bulk_insert", 4,
                values, lengths, formats, 0);
            if (PQresultStatus(res) == PGRES_COMMAND_OK) {
                result.rows_affected++;
            }
            PQclear(res);
        }
        result.bytes_logical += fsize;
    }

    exec("COMMIT");

    timer.stop();
    result.duration_ns = timer.elapsed_ns();

    LOG_INF("[%s] Bulk insert complete: %lld rows, %lld bytes, %lld ms",
        system_name(), result.rows_affected, result.bytes_logical, timer.elapsed_ms());

    return result;
}

MeasureResult PostgresConnector::perfile_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);

    LOG_INF("[%s] Per-file insert from %s", system_name(), dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[%s] DRY RUN: would per-file-insert from %s", system_name(), dir.c_str());
    result.rows_affected = 42;
    return result;
#endif

    Timer total_timer;
    total_timer.start();

    char insert_sql[512];
    std::snprintf(insert_sql, sizeof(insert_sql),
        "INSERT INTO %s.files (mime, size_bytes, sha256, payload) "
        "VALUES ($1, $2, $3, $4)", schema_.c_str());

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;

        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string sha256_hex(64, '0');
        std::string fsize_str = std::to_string(fsize);

        const char* values[4] = {
            "application/octet-stream",
            fsize_str.c_str(),
            sha256_hex.c_str(),
            buf.data()
        };
        int lengths[4] = {0, 0, 0, static_cast<int>(fsize)};
        int formats[4] = {0, 0, 0, 1};

        int64_t insert_ns = 0;
        {
            ScopedTimer st(insert_ns);
            if (conn_) {
                PGresult* res = PQexecParams(conn_, insert_sql, 4,
                    nullptr, values, lengths, formats, 0);
                if (PQresultStatus(res) == PGRES_COMMAND_OK) {
                    result.rows_affected++;
                }
                PQclear(res);
            }
        }
        result.bytes_logical += fsize;
        // TODO: record per-file latency for histogram
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();

    LOG_INF("[%s] Per-file insert: %lld rows, %lld bytes, %lld ms",
        system_name(), result.rows_affected, result.bytes_logical, total_timer.elapsed_ms());

    return result;
}

MeasureResult PostgresConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[%s] Per-file delete from %s.files", system_name(), schema_.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[%s] DRY RUN: would delete all from lab schema", system_name());
    return result;
#endif

    Timer timer;
    timer.start();

    char sql[256];
    std::snprintf(sql, sizeof(sql), "DELETE FROM %s.files", schema_.c_str());
    exec(sql);

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    return result;
}

MeasureResult PostgresConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[%s] Running maintenance (VACUUM FULL)", system_name());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[%s] DRY RUN: would run VACUUM FULL", system_name());
    return result;
#endif

    Timer timer;
    timer.start();

    char sql[256];
    std::snprintf(sql, sizeof(sql), "VACUUM FULL %s.files", schema_.c_str());
    exec(sql);

    // Also REINDEX
    std::snprintf(sql, sizeof(sql), "REINDEX TABLE %s.files", schema_.c_str());
    exec(sql);

    // Checkpoint to flush WAL
    exec("CHECKPOINT");

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[%s] Maintenance complete: %lld ms", system_name(), timer.elapsed_ms());
    return result;
}

int64_t PostgresConnector::get_logical_size_bytes() {
#ifdef DEDUP_DRY_RUN
    return 0;
#endif
    char sql[256];
    if (system_ == DbSystem::COCKROACHDB) {
        // CockroachDB: no pg_total_relation_size, use crdb_internal
        std::snprintf(sql, sizeof(sql),
            "SELECT sum(range_size) FROM crdb_internal.ranges "
            "WHERE database_name = '%s'", schema_.c_str());
    } else {
        std::snprintf(sql, sizeof(sql),
            "SELECT pg_total_relation_size('%s.files')", schema_.c_str());
    }

    PGresult* res = query(sql);
    if (!res) return -1;

    int64_t size = 0;
    if (PQntuples(res) > 0) {
        size = std::strtoll(PQgetvalue(res, 0, 0), nullptr, 10);
    }
    PQclear(res);
    return size;
}

} // namespace dedup
