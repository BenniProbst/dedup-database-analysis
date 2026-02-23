#include "postgres_connector.hpp"
#include "../utils/timer.hpp"
#include "../utils/logger.hpp"
#include "../utils/sha256.hpp"
#include <cstring>
#include "../experiment/native_record.hpp"
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
        "host=%s port=%u dbname=postgres user=%s password=%s "
        "connect_timeout=10 application_name=dedup-test",
        conn.host.c_str(), conn.port,
        conn.user.c_str(), conn.password.c_str());

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

bool PostgresConnector::reconnect(const DbConnection& conn) {
    if (conn_) {
        // PQreset reuses existing connection parameters -- faster than full reconnect
        PQreset(conn_);
        if (PQstatus(conn_) == CONNECTION_OK) {
            LOG_INF("[%s] PQreset successful (server %s)",
                system_name(), PQparameterStatus(conn_, "server_version"));
            return true;
        }
        LOG_WRN("[%s] PQreset failed: %s -- falling back to full reconnect",
            system_name(), PQerrorMessage(conn_));
    }
    // Full reconnect: disconnect + connect
    disconnect();
    return connect(conn);
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

    // For each file in the data directory, read and insert via parameterized query
    exec("BEGIN");

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

        // Compute SHA-256 fingerprint for dedup detection
        std::string sha256_hex = SHA256::hash_hex(buf.data(), fsize);
        std::string fsize_str = std::to_string(fsize);

        const char* values[4] = {
            "application/octet-stream",
            fsize_str.c_str(),
            sha256_hex.c_str(),
            buf.data()
        };
        int lengths[4] = {0, 0, 0, static_cast<int>(fsize)};
        int formats[4] = {0, 0, 0, 1};  // payload is binary

        if (conn_) {
            PGresult* res = PQexecParams(conn_, insert_sql, 4,
                nullptr, values, lengths, formats, 0);
            if (PQresultStatus(res) == PGRES_COMMAND_OK) {
                result.rows_affected++;
            } else {
                LOG_ERR("[%s] Bulk insert row error: %s", system_name(), PQerrorMessage(conn_));
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

        std::string sha256_hex = SHA256::hash_hex(buf.data(), fsize);
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
        result.per_file_latencies_ns.push_back(insert_ns);
        result.bytes_logical += fsize;
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();

    LOG_INF("[%s] Per-file insert: %lld rows, %lld bytes, %lld ms",
        system_name(), result.rows_affected, result.bytes_logical, total_timer.elapsed_ms());

    return result;
}

MeasureResult PostgresConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[%s] Per-file delete from %s.files (row-by-row)", system_name(), schema_.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[%s] DRY RUN: would delete all rows individually from lab schema", system_name());
    return result;
#endif

    Timer total_timer;
    total_timer.start();

    // Fetch all row IDs first
    char select_sql[256];
    std::snprintf(select_sql, sizeof(select_sql),
        "SELECT id::text FROM %s.files", schema_.c_str());
    PGresult* ids_res = query(select_sql);

    if (ids_res) {
        int nrows = PQntuples(ids_res);
        LOG_INF("[%s] Deleting %d rows individually", system_name(), nrows);

        char delete_sql[512];
        std::snprintf(delete_sql, sizeof(delete_sql),
            "DELETE FROM %s.files WHERE id = $1::uuid", schema_.c_str());

        for (int i = 0; i < nrows; ++i) {
            const char* id_val = PQgetvalue(ids_res, i, 0);
            const char* values[1] = { id_val };

            int64_t del_ns = 0;
            {
                ScopedTimer st(del_ns);
                PGresult* del_res = PQexecParams(conn_, delete_sql, 1,
                    nullptr, values, nullptr, nullptr, 0);
                if (PQresultStatus(del_res) == PGRES_COMMAND_OK) {
                    result.rows_affected++;
                }
                PQclear(del_res);
            }
            result.per_file_latencies_ns.push_back(del_ns);
        }
        PQclear(ids_res);
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[%s] Per-file delete: %lld rows, %lld ms",
        system_name(), result.rows_affected, total_timer.elapsed_ms());
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

// ============================================================================
// Native insertion mode methods (Stage 1, doku.tex 5.4)
// Appended to postgres_connector.cpp
// ============================================================================

std::string PostgresConnector::pg_type_for(const ColumnDef& col) const {
    // Map generic type hints to PostgreSQL-specific types
    const std::string& t = col.type_hint;

    if (t == "SERIAL") return "SERIAL";
    if (t == "BIGINT") return "BIGINT";
    if (t == "INT") return "INTEGER";
    if (t == "DOUBLE") return "DOUBLE PRECISION";
    if (t == "TEXT") return "TEXT";
    if (t == "BOOLEAN") return "BOOLEAN";
    if (t == "BYTEA") return "BYTEA";
    if (t == "UUID") return "UUID";
    if (t == "TIMESTAMPTZ") return "TIMESTAMPTZ";
    if (t == "JSONB") {
        // CockroachDB also supports JSONB
        return "JSONB";
    }
    if (t.substr(0, 4) == "CHAR") return t;  // CHAR(3) etc.

    return "TEXT";  // Fallback
}

std::string PostgresConnector::build_create_table_sql(const NativeSchema& ns) const {
    std::string sql = "CREATE TABLE IF NOT EXISTS " + schema_ + "." + ns.table_name + " (\n";

    for (size_t i = 0; i < ns.columns.size(); ++i) {
        const auto& col = ns.columns[i];
        if (i > 0) sql += ",\n";
        sql += "  " + col.name + " " + pg_type_for(col);

        if (col.is_primary_key && col.type_hint == "SERIAL") {
            sql += " PRIMARY KEY";
        } else if (col.is_primary_key && col.type_hint == "UUID") {
            sql += " PRIMARY KEY";
            if (!col.default_expr.empty())
                sql += " DEFAULT " + col.default_expr;
        } else if (col.is_primary_key) {
            sql += " PRIMARY KEY";
        }

        if (col.is_not_null && !col.is_primary_key)
            sql += " NOT NULL";

        if (!col.default_expr.empty() && !col.is_primary_key)
            sql += " DEFAULT " + col.default_expr;
    }

    sql += "\n)";
    return sql;
}

std::string PostgresConnector::build_insert_sql(const NativeSchema& ns) const {
    // Build INSERT with $N placeholders, skipping SERIAL columns
    std::string cols;
    std::string params;
    int param_idx = 0;

    for (const auto& col : ns.columns) {
        if (col.type_hint == "SERIAL") continue;  // Auto-generated

        if (param_idx > 0) { cols += ", "; params += ", "; }
        cols += col.name;
        params += "$" + std::to_string(++param_idx);
    }

    return "INSERT INTO " + schema_ + "." + ns.table_name +
           " (" + cols + ") VALUES (" + params + ")";
}

bool PostgresConnector::bind_and_exec_native(const std::string& sql,
                                               const NativeSchema& ns,
                                               const NativeRecord& record) {
    if (!conn_) return false;

    // Collect non-SERIAL columns
    std::vector<const ColumnDef*> insert_cols;
    for (const auto& col : ns.columns) {
        if (col.type_hint != "SERIAL") insert_cols.push_back(&col);
    }

    int nparams = static_cast<int>(insert_cols.size());
    std::vector<const char*> values(nparams, nullptr);
    std::vector<int> lengths(nparams, 0);
    std::vector<int> formats(nparams, 0);  // 0=text, 1=binary
    std::vector<std::string> str_bufs(nparams);  // Keep strings alive

    for (int i = 0; i < nparams; ++i) {
        const auto& col_name = insert_cols[i]->name;
        auto it = record.columns.find(col_name);

        if (it == record.columns.end() || std::holds_alternative<std::monostate>(it->second)) {
            values[i] = nullptr;  // NULL
            continue;
        }

        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                values[i] = nullptr;
            } else if constexpr (std::is_same_v<T, bool>) {
                str_bufs[i] = v ? "t" : "f";
                values[i] = str_bufs[i].c_str();
            } else if constexpr (std::is_same_v<T, int64_t>) {
                str_bufs[i] = std::to_string(v);
                values[i] = str_bufs[i].c_str();
            } else if constexpr (std::is_same_v<T, double>) {
                str_bufs[i] = std::to_string(v);
                values[i] = str_bufs[i].c_str();
            } else if constexpr (std::is_same_v<T, std::string>) {
                values[i] = v.c_str();
                lengths[i] = static_cast<int>(v.size());
            } else if constexpr (std::is_same_v<T, std::vector<char>>) {
                values[i] = v.data();
                lengths[i] = static_cast<int>(v.size());
                formats[i] = 1;  // Binary format for BYTEA
            }
        }, it->second);
    }

    PGresult* res = PQexecParams(conn_, sql.c_str(), nparams,
        nullptr, values.data(), lengths.data(), formats.data(), 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        LOG_ERR("[%s] Native insert error: %s", system_name(), PQerrorMessage(conn_));
    }
    PQclear(res);
    return ok;
}

bool PostgresConnector::create_native_schema(const std::string& schema_name, PayloadType type) {
    schema_ = schema_name;
    LOG_INF("[%s] Creating native schema for %s in %s",
        system_name(), payload_type_str(type), schema_name.c_str());

    // Create schema/database if needed
    char sql[256];
    if (system_ == DbSystem::COCKROACHDB) {
        std::snprintf(sql, sizeof(sql),
            "CREATE DATABASE IF NOT EXISTS %s", schema_name.c_str());
    } else {
        std::snprintf(sql, sizeof(sql),
            "CREATE SCHEMA IF NOT EXISTS %s", schema_name.c_str());
    }
    if (!exec(sql)) return false;

    // Create typed table based on NativeSchema
    auto ns = get_native_schema(type);
    std::string create_sql = build_create_table_sql(ns);
    LOG_DBG("[%s] CREATE TABLE SQL: %s", system_name(), create_sql.c_str());
    return exec(create_sql.c_str());
}

bool PostgresConnector::drop_native_schema(const std::string& schema_name, PayloadType type) {
    auto ns = get_native_schema(type);
    char sql[256];
    std::snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS %s.%s CASCADE",
        schema_name.c_str(), ns.table_name.c_str());
    return exec(sql);
}

MeasureResult PostgresConnector::native_bulk_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {

    MeasureResult result{};
    LOG_INF("[%s] Native bulk insert: %zu records (type: %s)",
        system_name(), records.size(), payload_type_str(type));

#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

    auto ns = get_native_schema(type);
    std::string insert_sql = build_insert_sql(ns);

    Timer timer;
    timer.start();

    exec("BEGIN");

    for (const auto& rec : records) {
        if (bind_and_exec_native(insert_sql, ns, rec)) {
            result.rows_affected++;
            result.bytes_logical += static_cast<int64_t>(rec.estimated_size_bytes());
        }
    }

    exec("COMMIT");

    timer.stop();
    result.duration_ns = timer.elapsed_ns();

    LOG_INF("[%s] Native bulk insert: %lld rows, %lld bytes, %lld ms",
        system_name(), result.rows_affected, result.bytes_logical, timer.elapsed_ms());
    return result;
}

MeasureResult PostgresConnector::native_perfile_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {

    MeasureResult result{};
    LOG_INF("[%s] Native per-file insert: %zu records (type: %s)",
        system_name(), records.size(), payload_type_str(type));

#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

    auto ns = get_native_schema(type);
    std::string insert_sql = build_insert_sql(ns);

    Timer total_timer;
    total_timer.start();

    for (const auto& rec : records) {
        int64_t insert_ns = 0;
        {
            ScopedTimer st(insert_ns);
            if (bind_and_exec_native(insert_sql, ns, rec)) {
                result.rows_affected++;
            }
        }
        result.per_file_latencies_ns.push_back(insert_ns);
        result.bytes_logical += static_cast<int64_t>(rec.estimated_size_bytes());
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();

    LOG_INF("[%s] Native per-file insert: %lld rows, %lld bytes, %lld ms",
        system_name(), result.rows_affected, result.bytes_logical, total_timer.elapsed_ms());
    return result;
}

MeasureResult PostgresConnector::native_perfile_delete(PayloadType type) {
    MeasureResult result{};
    auto ns = get_native_schema(type);
    const auto* pk = ns.primary_key();

    LOG_INF("[%s] Native per-file delete from %s.%s",
        system_name(), schema_.c_str(), ns.table_name.c_str());

#ifdef DEDUP_DRY_RUN
    return result;
#endif

    Timer total_timer;
    total_timer.start();

    // Fetch all primary key values
    std::string pk_col = pk ? pk->name : "id";
    char select_sql[256];
    std::snprintf(select_sql, sizeof(select_sql),
        "SELECT %s::text FROM %s.%s",
        pk_col.c_str(), schema_.c_str(), ns.table_name.c_str());

    PGresult* ids_res = query(select_sql);
    if (ids_res) {
        int nrows = PQntuples(ids_res);
        LOG_INF("[%s] Deleting %d native records individually", system_name(), nrows);

        // Build delete SQL based on PK type
        std::string delete_sql = "DELETE FROM " + schema_ + "." + ns.table_name +
            " WHERE " + pk_col + " = $1";
        if (pk && (pk->type_hint == "UUID")) {
            delete_sql += "::uuid";
        } else if (pk && (pk->type_hint == "SERIAL" || pk->type_hint == "INT" || pk->type_hint == "BIGINT")) {
            delete_sql += "::bigint";
        }

        for (int i = 0; i < nrows; ++i) {
            const char* id_val = PQgetvalue(ids_res, i, 0);
            const char* values[1] = { id_val };

            int64_t del_ns = 0;
            {
                ScopedTimer st(del_ns);
                PGresult* del_res = PQexecParams(conn_, delete_sql.c_str(), 1,
                    nullptr, values, nullptr, nullptr, 0);
                if (PQresultStatus(del_res) == PGRES_COMMAND_OK) {
                    result.rows_affected++;
                }
                PQclear(del_res);
            }
            result.per_file_latencies_ns.push_back(del_ns);
        }
        PQclear(ids_res);
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[%s] Native per-file delete: %lld rows, %lld ms",
        system_name(), result.rows_affected, total_timer.elapsed_ms());
    return result;
}

int64_t PostgresConnector::get_native_logical_size_bytes(PayloadType type) {
#ifdef DEDUP_DRY_RUN
    return 0;
#endif
    auto ns = get_native_schema(type);
    char sql[256];
    if (system_ == DbSystem::COCKROACHDB) {
        std::snprintf(sql, sizeof(sql),
            "SELECT sum(range_size) FROM crdb_internal.ranges "
            "WHERE database_name = '%s'", schema_.c_str());
    } else {
        std::snprintf(sql, sizeof(sql),
            "SELECT pg_total_relation_size('%s.%s')",
            schema_.c_str(), ns.table_name.c_str());
    }

    PGresult* res = query(sql);
    if (!res) return -1;

    int64_t size = 0;
    if (PQntuples(res) > 0 && PQntuples(res) > 0) {
        const char* val = PQgetvalue(res, 0, 0);
        if (val && val[0]) size = std::strtoll(val, nullptr, 10);
    }
    PQclear(res);
    return size;
}

} // namespace dedup
