// MariaDB connector stub -- requires libmysqlclient
// TODO: Install MariaDB in K8s cluster, then implement full connector
// Lab schema: CREATE DATABASE IF NOT EXISTS dedup_lab
// Table: files(id UUID PK, mime TEXT, size_bytes BIGINT, sha256 BINARY(32), payload LONGBLOB)
// Maintenance: OPTIMIZE TABLE (InnoDB online defrag)

#include "mariadb_connector.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include "../utils/sha256.hpp"
#include <filesystem>
#include <fstream>

#ifdef HAS_MYSQL
#include <mysql/mysql.h>
#endif

namespace dedup {
namespace fs = std::filesystem;

#ifdef HAS_MYSQL
// Consume any pending result set from mysql_query() -- prevents "Commands out of sync"
static void mysql_consume_result(MYSQL* mysql) {
    MYSQL_RES* r = mysql_store_result(mysql);
    if (r) mysql_free_result(r);
    while (mysql_next_result(mysql) == 0) {
        r = mysql_store_result(mysql);
        if (r) mysql_free_result(r);
    }
}
#endif

bool MariaDBConnector::connect(const DbConnection& conn) {
    schema_ = conn.lab_schema;

#ifdef DEDUP_DRY_RUN
    LOG_INF("[mariadb] DRY RUN: simulating connection to %s:%u", conn.host.c_str(), conn.port);
    connected_ = true;
    return true;
#endif

#ifdef HAS_MYSQL
    MYSQL* mysql = mysql_init(nullptr);
    if (!mysql) {
        LOG_ERR("[mariadb] mysql_init failed");
        return false;
    }

    unsigned int timeout = 10;
    mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!mysql_real_connect(mysql, conn.host.c_str(), conn.user.c_str(),
                            conn.password.c_str(), nullptr,  // connect without DB; create_lab_schema() does CREATE DATABASE + USE
                            conn.port, nullptr, 0)) {
        LOG_ERR("[mariadb] Connection failed: %s", mysql_error(mysql));
        mysql_close(mysql);
        return false;
    }

    conn_ = mysql;
    connected_ = true;
    LOG_INF("[mariadb] Connected to %s:%u/%s (server: %s)",
        conn.host.c_str(), conn.port, conn.database.c_str(),
        mysql_get_server_info(mysql));
    return true;
#else
    LOG_ERR("[mariadb] libmysqlclient not available -- MariaDB connector disabled");
    LOG_ERR("[mariadb] Install: apt-get install libmariadb-dev");
    (void)conn;
    return false;
#endif
}

void MariaDBConnector::disconnect() {
#ifdef HAS_MYSQL
    if (conn_) {
        mysql_close(static_cast<MYSQL*>(conn_));
        conn_ = nullptr;
    }
#endif
    connected_ = false;
}

bool MariaDBConnector::is_connected() const { return connected_; }

bool MariaDBConnector::create_lab_schema(const std::string& schema_name) {
    LOG_INF("[mariadb] Creating lab schema: %s", schema_name.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[mariadb] DRY RUN: would CREATE DATABASE %s", schema_name.c_str());
    return true;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    if (!mysql) return false;

    std::string sql = "CREATE DATABASE IF NOT EXISTS " + schema_name;
    if (mysql_query(mysql, sql.c_str())) {
        LOG_ERR("[mariadb] CREATE DATABASE: %s", mysql_error(mysql));
        return false;
    }
    mysql_consume_result(mysql);

    sql = "USE " + schema_name;
    if (mysql_query(mysql, sql.c_str()) != 0) {
        LOG_ERR("[mariadb] USE: %s", mysql_error(mysql));
        return false;
    }
    mysql_consume_result(mysql);

    // Create files table matching doku.tex Stage 2 schema
    sql = "CREATE TABLE IF NOT EXISTS files ("
          "  id CHAR(36) PRIMARY KEY,"
          "  mime VARCHAR(128) NOT NULL,"
          "  size_bytes BIGINT NOT NULL,"
          "  sha256 CHAR(64) NOT NULL,"
          "  payload LONGBLOB NOT NULL,"
          "  inserted_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
          ") ENGINE=InnoDB";
    if (mysql_query(mysql, sql.c_str())) {
        LOG_ERR("[mariadb] CREATE TABLE: %s", mysql_error(mysql));
        return false;
    }
    mysql_consume_result(mysql);
    return true;
#else
    return false;
#endif
}

bool MariaDBConnector::drop_lab_schema(const std::string& schema_name) {
    LOG_WRN("[mariadb] DROPPING lab schema: %s", schema_name.c_str());

#ifdef DEDUP_DRY_RUN
    return true;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    if (!mysql) return false;
    std::string sql = "DROP DATABASE IF EXISTS " + schema_name;
    bool ok = mysql_query(mysql, sql.c_str()) == 0;
    mysql_consume_result(mysql);
    return ok;
#else
    return false;
#endif
}

bool MariaDBConnector::reset_lab_schema(const std::string& schema_name) {
    return drop_lab_schema(schema_name) && create_lab_schema(schema_name);
}

MeasureResult MariaDBConnector::bulk_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    LOG_INF("[mariadb] Bulk insert from %s", dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[mariadb] DRY RUN: would bulk-insert files");
    result.rows_affected = 42;
    return result;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    if (!mysql) return result;

    Timer timer;
    timer.start();

    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    const char* sql = "INSERT INTO files (id, mime, size_bytes, sha256, payload) "
                      "VALUES (UUID(), ?, ?, ?, ?)";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
        LOG_ERR("[mariadb] Prepare failed: %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return result;
    }
    bool first_err_logged = false;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string sha256 = SHA256::hash_hex(buf.data(), fsize);
        const char* mime = "application/octet-stream";
        long long size_val = static_cast<long long>(fsize);

        MYSQL_BIND bind[4]{};
        unsigned long mime_len = strlen(mime);
        unsigned long sha_len = sha256.size();
        unsigned long payload_len = fsize;

        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = const_cast<char*>(mime);
        bind[0].buffer_length = mime_len;
        bind[0].length = &mime_len;

        bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[1].buffer = &size_val;

        bind[2].buffer_type = MYSQL_TYPE_STRING;
        bind[2].buffer = const_cast<char*>(sha256.data());
        bind[2].buffer_length = sha_len;
        bind[2].length = &sha_len;

        bind[3].buffer_type = MYSQL_TYPE_LONG_BLOB;
        bind[3].buffer = buf.data();
        bind[3].buffer_length = payload_len;
        bind[3].length = &payload_len;

        mysql_stmt_bind_param(stmt, bind);
        if (mysql_stmt_execute(stmt) == 0) {
            result.rows_affected++;
        } else if (!first_err_logged) {
            LOG_ERR("[mariadb] First bulk execute error: %s", mysql_stmt_error(stmt));
            first_err_logged = true;
        }

        result.bytes_logical += fsize;
    }

    mysql_stmt_close(stmt);
    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[mariadb] Bulk insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
#endif
    return result;
}

MeasureResult MariaDBConnector::perfile_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    LOG_INF("[mariadb] Per-file insert from %s", dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[mariadb] DRY RUN: would per-file-insert files");
    result.rows_affected = 42;
    return result;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    if (!mysql) return result;

    Timer total_timer;
    total_timer.start();

    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    const char* sql = "INSERT INTO files (id, mime, size_bytes, sha256, payload) "
                      "VALUES (UUID(), ?, ?, ?, ?)";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
        LOG_ERR("[mariadb] Prepare failed: %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return result;
    }
    bool first_err_logged = false;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string sha256 = SHA256::hash_hex(buf.data(), fsize);
        const char* mime = "application/octet-stream";
        long long size_val = static_cast<long long>(fsize);

        MYSQL_BIND bind[4]{};
        unsigned long mime_len = strlen(mime);
        unsigned long sha_len = sha256.size();
        unsigned long payload_len = fsize;

        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = const_cast<char*>(mime);
        bind[0].buffer_length = mime_len;
        bind[0].length = &mime_len;

        bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[1].buffer = &size_val;

        bind[2].buffer_type = MYSQL_TYPE_STRING;
        bind[2].buffer = const_cast<char*>(sha256.data());
        bind[2].buffer_length = sha_len;
        bind[2].length = &sha_len;

        bind[3].buffer_type = MYSQL_TYPE_LONG_BLOB;
        bind[3].buffer = buf.data();
        bind[3].buffer_length = payload_len;
        bind[3].length = &payload_len;

        mysql_stmt_bind_param(stmt, bind);

        int64_t insert_ns = 0;
        {
            ScopedTimer st(insert_ns);
            if (mysql_stmt_execute(stmt) == 0) {
                result.rows_affected++;
            } else if (!first_err_logged) {
                LOG_ERR("[mariadb] First perfile execute error: %s", mysql_stmt_error(stmt));
                first_err_logged = true;
            }

        }
        result.per_file_latencies_ns.push_back(insert_ns);
        result.bytes_logical += fsize;
    }

    mysql_stmt_close(stmt);
    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[mariadb] Per-file insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, total_timer.elapsed_ms());
#endif
    return result;
}

MeasureResult MariaDBConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[mariadb] Per-file delete from %s.files (row-by-row)", schema_.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[mariadb] DRY RUN: would delete all rows individually");
    return result;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    if (!mysql) return result;

    Timer total_timer;
    total_timer.start();

    // Fetch all IDs
    if (mysql_query(mysql, "SELECT id FROM files") != 0) {
        result.error = mysql_error(mysql);
        return result;
    }

    MYSQL_RES* res = mysql_store_result(mysql);
    if (!res) return result;

    std::vector<std::string> ids;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        if (row[0]) ids.emplace_back(row[0]);
    }
    mysql_free_result(res);

    LOG_INF("[mariadb] Deleting %zu rows individually", ids.size());

    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    const char* del_sql = "DELETE FROM files WHERE id = ?";
    mysql_stmt_prepare(stmt, del_sql, strlen(del_sql));

    for (const auto& id : ids) {
        MYSQL_BIND bind[1]{};
        unsigned long id_len = id.size();
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = const_cast<char*>(id.data());
        bind[0].buffer_length = id_len;
        bind[0].length = &id_len;

        mysql_stmt_bind_param(stmt, bind);

        int64_t del_ns = 0;
        {
            ScopedTimer st(del_ns);
            if (mysql_stmt_execute(stmt) == 0) {
                result.rows_affected++;
            }
        }
        result.per_file_latencies_ns.push_back(del_ns);
    }

    mysql_stmt_close(stmt);
    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[mariadb] Per-file delete: %lld rows, %lld ms",
        result.rows_affected, total_timer.elapsed_ms());
#endif
    return result;
}

MeasureResult MariaDBConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[mariadb] Running maintenance (OPTIMIZE TABLE)");

#ifdef DEDUP_DRY_RUN
    LOG_INF("[mariadb] DRY RUN: would run OPTIMIZE TABLE");
    return result;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    if (!mysql) return result;
    Timer timer;
    timer.start();
    // OPTIMIZE TABLE = InnoDB online defrag (rebuilds table + indexes)
    if (mysql_query(mysql, "OPTIMIZE TABLE files") != 0) {
        LOG_ERR("[mariadb] OPTIMIZE TABLE: %s", mysql_error(mysql));
    }
    mysql_consume_result(mysql);
    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[mariadb] Maintenance complete: %lld ms", timer.elapsed_ms());
#endif
    return result;
}

int64_t MariaDBConnector::get_logical_size_bytes() {
#ifdef DEDUP_DRY_RUN
    return 0;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    if (!mysql) return -1;

    std::string sql = "SELECT data_length + index_length FROM information_schema.tables "
                      "WHERE table_schema = '" + schema_ + "' AND table_name = 'files'";
    if (mysql_query(mysql, sql.c_str())) return -1;

    MYSQL_RES* res = mysql_store_result(mysql);
    if (!res) return -1;

    MYSQL_ROW row = mysql_fetch_row(res);
    int64_t size = row ? std::strtoll(row[0], nullptr, 10) : -1;
    mysql_free_result(res);
    return size;
#else
    return -1;
#endif
}

// ============================================================================
// Native insertion mode (Stage 1)
// ============================================================================

bool MariaDBConnector::create_native_schema(const std::string& schema_name, PayloadType type) {
    schema_ = schema_name;
    LOG_INF("[mariadb] Creating native schema for %s in %s",
        payload_type_str(type), schema_name.c_str());

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    std::string sql = "CREATE DATABASE IF NOT EXISTS " + schema_name;
    if (mysql_query(mysql, sql.c_str()) != 0) {
        LOG_ERR("[mariadb] CREATE DATABASE error: %s", mysql_error(mysql));
        return false;
    }
    mysql_consume_result(mysql);

    sql = "USE " + schema_name;
    if (mysql_query(mysql, sql.c_str()) != 0) {
        LOG_ERR("[mariadb] USE error: %s", mysql_error(mysql));
        return false;
    }
    mysql_consume_result(mysql);

    auto ns = get_native_schema(type);
    sql = "CREATE TABLE IF NOT EXISTS " + ns.table_name + " (\n";

    for (size_t i = 0; i < ns.columns.size(); ++i) {
        const auto& col = ns.columns[i];
        if (i > 0) sql += ",\n";
        sql += "  " + col.name + " ";

        // MariaDB type mapping
        if (col.type_hint == "SERIAL") sql += "BIGINT AUTO_INCREMENT";
        else if (col.type_hint == "BIGINT") sql += "BIGINT";
        else if (col.type_hint == "INT") sql += "INT";
        else if (col.type_hint == "DOUBLE") sql += "DOUBLE";
        else if (col.type_hint == "TEXT") sql += "TEXT";
        else if (col.type_hint == "BOOLEAN") sql += "TINYINT(1)";
        else if (col.type_hint == "BYTEA") sql += "LONGBLOB";
        else if (col.type_hint == "UUID") sql += "CHAR(36)";
        else if (col.type_hint == "TIMESTAMPTZ") sql += "DATETIME(6) DEFAULT CURRENT_TIMESTAMP(6)";
        else if (col.type_hint == "JSONB") sql += "JSON";
        else if (col.type_hint.substr(0, 4) == "CHAR") sql += col.type_hint;
        else sql += "TEXT";

        if (col.is_primary_key) sql += " PRIMARY KEY";
        if (col.is_not_null && !col.is_primary_key) sql += " NOT NULL";
        if (!col.default_expr.empty() && !col.is_primary_key &&
            col.type_hint != "TIMESTAMPTZ" && col.type_hint != "SERIAL") {
            sql += " DEFAULT " + col.default_expr;
        }
    }
    sql += "\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (mysql_query(mysql, sql.c_str()) != 0) {
        LOG_ERR("[mariadb] CREATE TABLE error: %s", mysql_error(mysql));
        return false;
    }
    mysql_consume_result(mysql);
    return true;
#else
    return create_lab_schema(schema_name);
#endif
}

bool MariaDBConnector::drop_native_schema(const std::string& schema_name, PayloadType type) {
#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    auto ns = get_native_schema(type);
    std::string sql = "DROP TABLE IF EXISTS " + schema_name + "." + ns.table_name;
    mysql_query(mysql, sql.c_str());
    mysql_consume_result(mysql);
    return true;
#else
    return drop_lab_schema(schema_name);
#endif
}

MeasureResult MariaDBConnector::native_bulk_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {
    MeasureResult result{};
    LOG_INF("[mariadb] Native bulk insert: %zu records (type: %s)",
        records.size(), payload_type_str(type));

#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    auto ns = get_native_schema(type);

    // Build INSERT SQL with ? placeholders
    std::string cols, params;
    std::vector<std::string> insert_col_names;
    for (const auto& col : ns.columns) {
        if (col.type_hint == "SERIAL") continue;
        if (!cols.empty()) { cols += ", "; params += ", "; }
        cols += col.name;
        params += "?";
        insert_col_names.push_back(col.name);
    }
    std::string sql = "INSERT INTO " + ns.table_name +
        " (" + cols + ") VALUES (" + params + ")";

    Timer timer;
    timer.start();

    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size()) != 0) {
        LOG_ERR("[mariadb] Prepare error: %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        result.error = "prepare failed";
        return result;
    }

    for (const auto& rec : records) {
        int nparams = static_cast<int>(insert_col_names.size());
        std::vector<MYSQL_BIND> binds(nparams);
        std::vector<std::string> str_bufs(nparams);
        std::vector<unsigned long> str_lens(nparams);
        std::vector<my_bool> null_flags(nparams, 0);
        memset(binds.data(), 0, sizeof(MYSQL_BIND) * nparams);

        for (int i = 0; i < nparams; ++i) {
            auto it = rec.columns.find(insert_col_names[i]);
            if (it == rec.columns.end() || std::holds_alternative<std::monostate>(it->second)) {
                null_flags[i] = 1;
                binds[i].is_null = &null_flags[i];
                binds[i].buffer_type = MYSQL_TYPE_NULL;
                continue;
            }

            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int64_t>) {
                    binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                    str_bufs[i].resize(sizeof(int64_t));
                    memcpy(&str_bufs[i][0], &v, sizeof(int64_t));
                    binds[i].buffer = &str_bufs[i][0];
                } else if constexpr (std::is_same_v<T, double>) {
                    binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                    str_bufs[i].resize(sizeof(double));
                    memcpy(&str_bufs[i][0], &v, sizeof(double));
                    binds[i].buffer = &str_bufs[i][0];
                } else if constexpr (std::is_same_v<T, bool>) {
                    binds[i].buffer_type = MYSQL_TYPE_TINY;
                    str_bufs[i] = v ? "\x01" : "\x00";
                    binds[i].buffer = &str_bufs[i][0];
                } else if constexpr (std::is_same_v<T, std::string>) {
                    binds[i].buffer_type = MYSQL_TYPE_STRING;
                    str_bufs[i] = v;
                    str_lens[i] = v.size();
                    binds[i].buffer = &str_bufs[i][0];
                    binds[i].buffer_length = str_lens[i];
                    binds[i].length = &str_lens[i];
                } else if constexpr (std::is_same_v<T, std::vector<char>>) {
                    binds[i].buffer_type = MYSQL_TYPE_BLOB;
                    str_bufs[i].assign(v.begin(), v.end());
                    str_lens[i] = v.size();
                    binds[i].buffer = &str_bufs[i][0];
                    binds[i].buffer_length = str_lens[i];
                    binds[i].length = &str_lens[i];
                } else {
                    null_flags[i] = 1;
                    binds[i].is_null = &null_flags[i];
                    binds[i].buffer_type = MYSQL_TYPE_NULL;
                }
            }, it->second);
        }

        mysql_stmt_bind_param(stmt, binds.data());
        if (mysql_stmt_execute(stmt) == 0) {
            result.rows_affected++;
            result.bytes_logical += static_cast<int64_t>(rec.estimated_size_bytes());
        }
    }

    mysql_stmt_close(stmt);
    timer.stop();
    result.duration_ns = timer.elapsed_ns();

    LOG_INF("[mariadb] Native bulk insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
#else
    result.error = "MariaDB not compiled (HAS_MYSQL not defined)";
#endif
    return result;
}

MeasureResult MariaDBConnector::native_perfile_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {
    // Same as bulk but with per-record latency tracking
    MeasureResult result{};
#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    auto ns = get_native_schema(type);

    std::string cols, params;
    std::vector<std::string> insert_col_names;
    for (const auto& col : ns.columns) {
        if (col.type_hint == "SERIAL") continue;
        if (!cols.empty()) { cols += ", "; params += ", "; }
        cols += col.name;
        params += "?";
        insert_col_names.push_back(col.name);
    }
    std::string sql = "INSERT INTO " + ns.table_name +
        " (" + cols + ") VALUES (" + params + ")";

    Timer total_timer;
    total_timer.start();

    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    mysql_stmt_prepare(stmt, sql.c_str(), sql.size());

    for (const auto& rec : records) {
        int nparams = static_cast<int>(insert_col_names.size());
        std::vector<MYSQL_BIND> binds(nparams);
        std::vector<std::string> str_bufs(nparams);
        std::vector<unsigned long> str_lens(nparams);
        std::vector<my_bool> null_flags(nparams, 0);
        memset(binds.data(), 0, sizeof(MYSQL_BIND) * nparams);

        for (int i = 0; i < nparams; ++i) {
            auto it = rec.columns.find(insert_col_names[i]);
            if (it == rec.columns.end() || std::holds_alternative<std::monostate>(it->second)) {
                null_flags[i] = 1;
                binds[i].is_null = &null_flags[i];
                binds[i].buffer_type = MYSQL_TYPE_NULL;
                continue;
            }
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int64_t>) {
                    binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                    str_bufs[i].resize(sizeof(int64_t));
                    memcpy(&str_bufs[i][0], &v, sizeof(int64_t));
                    binds[i].buffer = &str_bufs[i][0];
                } else if constexpr (std::is_same_v<T, double>) {
                    binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                    str_bufs[i].resize(sizeof(double));
                    memcpy(&str_bufs[i][0], &v, sizeof(double));
                    binds[i].buffer = &str_bufs[i][0];
                } else if constexpr (std::is_same_v<T, bool>) {
                    binds[i].buffer_type = MYSQL_TYPE_TINY;
                    str_bufs[i] = v ? "\x01" : "\x00";
                    binds[i].buffer = &str_bufs[i][0];
                } else if constexpr (std::is_same_v<T, std::string>) {
                    binds[i].buffer_type = MYSQL_TYPE_STRING;
                    str_bufs[i] = v;
                    str_lens[i] = v.size();
                    binds[i].buffer = &str_bufs[i][0];
                    binds[i].buffer_length = str_lens[i];
                    binds[i].length = &str_lens[i];
                } else if constexpr (std::is_same_v<T, std::vector<char>>) {
                    binds[i].buffer_type = MYSQL_TYPE_BLOB;
                    str_bufs[i].assign(v.begin(), v.end());
                    str_lens[i] = v.size();
                    binds[i].buffer = &str_bufs[i][0];
                    binds[i].buffer_length = str_lens[i];
                    binds[i].length = &str_lens[i];
                } else {
                    null_flags[i] = 1;
                    binds[i].is_null = &null_flags[i];
                    binds[i].buffer_type = MYSQL_TYPE_NULL;
                }
            }, it->second);
        }

        mysql_stmt_bind_param(stmt, binds.data());

        int64_t insert_ns = 0;
        {
            ScopedTimer st(insert_ns);
            if (mysql_stmt_execute(stmt) == 0) result.rows_affected++;
        }
        result.per_file_latencies_ns.push_back(insert_ns);
        result.bytes_logical += static_cast<int64_t>(rec.estimated_size_bytes());
    }

    mysql_stmt_close(stmt);
    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
#else
    result.error = "MariaDB not compiled";
#endif
    return result;
}

MeasureResult MariaDBConnector::native_perfile_delete(PayloadType type) {
    MeasureResult result{};
#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    auto ns = get_native_schema(type);
    const auto* pk = ns.primary_key();
    std::string pk_col = pk ? pk->name : "id";

    Timer total_timer;
    total_timer.start();

    std::string sel = "SELECT " + pk_col + " FROM " + ns.table_name;
    if (mysql_query(mysql, sel.c_str()) == 0) {
        MYSQL_RES* res = mysql_store_result(mysql);
        if (res) {
            MYSQL_ROW row;
            std::vector<std::string> ids;
            while ((row = mysql_fetch_row(res))) {
                if (row[0]) ids.push_back(row[0]);
            }
            mysql_free_result(res);

            for (const auto& id : ids) {
                std::string del = "DELETE FROM " + ns.table_name +
                    " WHERE " + pk_col + " = '" + id + "' LIMIT 1";
                int64_t del_ns = 0;
                {
                    ScopedTimer st(del_ns);
                    if (mysql_query(mysql, del.c_str()) == 0) result.rows_affected++;
                    mysql_consume_result(mysql);
                }
                result.per_file_latencies_ns.push_back(del_ns);
            }
        }
    }
    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
#else
    return perfile_delete();
#endif
    return result;
}

int64_t MariaDBConnector::get_native_logical_size_bytes(PayloadType type) {
#ifdef HAS_MYSQL
    auto* mysql = static_cast<MYSQL*>(conn_);
    auto ns = get_native_schema(type);
    std::string sql = "SELECT data_length + index_length FROM information_schema.tables "
        "WHERE table_schema = '" + schema_ + "' AND table_name = '" + ns.table_name + "'";
    if (mysql_query(mysql, sql.c_str()) == 0) {
        MYSQL_RES* res = mysql_store_result(mysql);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            int64_t size = row && row[0] ? std::strtoll(row[0], nullptr, 10) : 0;
            mysql_free_result(res);
            return size;
        }
    }
#endif
    return get_logical_size_bytes();
}

} // namespace dedup
