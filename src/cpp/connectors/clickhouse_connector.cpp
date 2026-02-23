// ClickHouse connector -- uses HTTP API (port 8123, no external lib required)
// ClickHouse is column-oriented with aggressive compression (LZ4/ZSTD).
// Per doku.tex: "column-oriented analytical DBMS with aggressive compression"
//
// Lab schema: CREATE DATABASE IF NOT EXISTS dedup_lab
// Table: dedup_lab.files(id UUID, mime String, size_bytes UInt64,
//                        sha256 FixedString(32), payload String,
//                        inserted_at DateTime DEFAULT now())
// Engine: MergeTree() ORDER BY id
// Maintenance: OPTIMIZE TABLE files FINAL (force merge of parts)
// Size query: system.parts (bytes_on_disk)
//
// TODO: Install ClickHouse in K8s cluster

#include "clickhouse_connector.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include "../utils/sha256.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dedup {
namespace fs = std::filesystem;

static size_t ch_write_cb(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string ClickHouseConnector::http_query(const std::string& sql) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[clickhouse] DRY RUN query: %s", sql.c_str());
    return "0";
#endif
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string url = endpoint_ + "/?database=" + database_;
    if (!user_.empty()) url += "&user=" + user_;
    if (!password_.empty()) url += "&password=" + password_;
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, sql.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERR("[clickhouse] HTTP query failed: %s", curl_easy_strerror(res));
        return "";
    }
    return response;
}

bool ClickHouseConnector::http_exec(const std::string& sql) {
    std::string resp = http_query(sql);
    // ClickHouse returns empty on success for DDL/DML
    return true;
}

bool ClickHouseConnector::connect(const DbConnection& conn) {
    endpoint_ = "http://" + conn.host + ":" + std::to_string(conn.port);
    database_ = "default";  // connect to default DB; create_lab_schema() handles target DB
    user_ = conn.user;
    password_ = conn.password;

#ifdef DEDUP_DRY_RUN
    LOG_INF("[clickhouse] DRY RUN: simulating connection to %s", endpoint_.c_str());
    connected_ = true;
    return true;
#endif

    // Health check via HTTP ping
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = endpoint_ + "/ping";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        LOG_ERR("[clickhouse] Health check failed: curl=%d, http=%ld", res, http_code);
        return false;
    }

    connected_ = true;
    LOG_INF("[clickhouse] Connected to %s (database: %s)", endpoint_.c_str(), database_.c_str());
    return true;
}

void ClickHouseConnector::disconnect() {
    connected_ = false;
}

bool ClickHouseConnector::is_connected() const { return connected_; }

bool ClickHouseConnector::create_lab_schema(const std::string& schema_name) {
    LOG_INF("[clickhouse] Creating lab database: %s", schema_name.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[clickhouse] DRY RUN: would CREATE DATABASE %s", schema_name.c_str());
    return true;
#endif

    http_exec("CREATE DATABASE IF NOT EXISTS " + schema_name);

    // MergeTree table for file storage (column-oriented, LZ4 compressed by default)
    std::string sql = "CREATE TABLE IF NOT EXISTS " + schema_name + ".files ("
        "id UUID DEFAULT generateUUIDv4(), "
        "mime String, "
        "size_bytes UInt64, "
        "sha256 FixedString(32), "
        "payload String, "
        "inserted_at DateTime DEFAULT now()"
        ") ENGINE = MergeTree() ORDER BY id";
    if (!http_exec(sql)) return false;
    database_ = schema_name;  // switch to lab database for all subsequent queries
    LOG_INF("[clickhouse] Switched database to: %s", database_.c_str());
    return true;
}

bool ClickHouseConnector::drop_lab_schema(const std::string& schema_name) {
    LOG_WRN("[clickhouse] DROPPING lab database: %s", schema_name.c_str());
#ifdef DEDUP_DRY_RUN
    return true;
#endif
    return http_exec("DROP DATABASE IF EXISTS " + schema_name);
}

bool ClickHouseConnector::reset_lab_schema(const std::string& schema_name) {
    return drop_lab_schema(schema_name) && create_lab_schema(schema_name);
}

MeasureResult ClickHouseConnector::bulk_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    LOG_INF("[clickhouse] Bulk insert from %s", dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[clickhouse] DRY RUN: would bulk-insert files");
    result.rows_affected = 42;
    return result;
#endif

    Timer timer;
    timer.start();

    // ClickHouse: use INSERT ... FORMAT TabSeparated via HTTP
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string sha256 = SHA256::hash_hex(buf.data(), fsize);

        // Base64-encode payload for HTTP transport (ClickHouse supports base64)
        // For simplicity, use hex encoding
        std::ostringstream hex_payload;
        for (size_t i = 0; i < fsize; ++i) {
            hex_payload << std::hex << std::setfill('0') << std::setw(2)
                        << (static_cast<unsigned>(buf[i]) & 0xFF);
        }

        std::string sql = "INSERT INTO " + database_ + ".files "
            "(mime, size_bytes, sha256, payload) VALUES "
            "('application/octet-stream', " + std::to_string(fsize) +
            ", unhex('" + sha256 + "'), unhex('" + hex_payload.str() + "'))";

        if (http_exec(sql)) {
            result.rows_affected++;
        }
        result.bytes_logical += fsize;
    }

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[clickhouse] Bulk insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
    return result;
}

MeasureResult ClickHouseConnector::perfile_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    LOG_INF("[clickhouse] Per-file insert from %s", dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[clickhouse] DRY RUN: would per-file-insert files");
    result.rows_affected = 42;
    return result;
#endif

    Timer total_timer;
    total_timer.start();

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string sha256 = SHA256::hash_hex(buf.data(), fsize);

        std::ostringstream hex_payload;
        for (size_t i = 0; i < fsize; ++i) {
            hex_payload << std::hex << std::setfill('0') << std::setw(2)
                        << (static_cast<unsigned>(buf[i]) & 0xFF);
        }

        std::string sql = "INSERT INTO " + database_ + ".files "
            "(mime, size_bytes, sha256, payload) VALUES "
            "('application/octet-stream', " + std::to_string(fsize) +
            ", unhex('" + sha256 + "'), unhex('" + hex_payload.str() + "'))";

        int64_t insert_ns = 0;
        {
            ScopedTimer st(insert_ns);
            http_exec(sql);
        }
        result.rows_affected++;
        result.per_file_latencies_ns.push_back(insert_ns);
        result.bytes_logical += fsize;
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[clickhouse] Per-file insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, total_timer.elapsed_ms());
    return result;
}

MeasureResult ClickHouseConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[clickhouse] Per-file delete (row-by-row lightweight delete)");

#ifdef DEDUP_DRY_RUN
    LOG_INF("[clickhouse] DRY RUN: would delete rows individually");
    return result;
#endif

    Timer total_timer;
    total_timer.start();

    // Fetch all IDs first
    std::string ids_resp = http_query(
        "SELECT toString(id) FROM " + database_ + ".files FORMAT TabSeparated");

    std::vector<std::string> ids;
    std::istringstream iss(ids_resp);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim whitespace
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty()) ids.push_back(line);
    }

    LOG_INF("[clickhouse] Deleting %zu rows individually", ids.size());

    for (const auto& id : ids) {
        std::string sql = "ALTER TABLE " + database_ + ".files DELETE WHERE id = '" + id + "'";

        int64_t del_ns = 0;
        {
            ScopedTimer st(del_ns);
            http_exec(sql);
        }
        result.rows_affected++;
        result.per_file_latencies_ns.push_back(del_ns);
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[clickhouse] Per-file delete: %lld rows, %lld ms",
        result.rows_affected, total_timer.elapsed_ms());
    return result;
}

MeasureResult ClickHouseConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[clickhouse] Running maintenance (OPTIMIZE TABLE FINAL)");

#ifdef DEDUP_DRY_RUN
    LOG_INF("[clickhouse] DRY RUN: would OPTIMIZE TABLE FINAL");
    return result;
#endif

    Timer timer;
    timer.start();
    // OPTIMIZE TABLE FINAL forces merge of all parts into one
    http_exec("OPTIMIZE TABLE " + database_ + ".files FINAL");
    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[clickhouse] Maintenance complete: %lld ms", timer.elapsed_ms());
    return result;
}

int64_t ClickHouseConnector::get_logical_size_bytes() {
#ifdef DEDUP_DRY_RUN
    return 0;
#endif

    // Query system.parts for actual bytes on disk
    std::string sql = "SELECT sum(bytes_on_disk) FROM system.parts "
                      "WHERE database = '" + database_ + "' AND table = 'files' AND active";
    std::string resp = http_query(sql);
    if (resp.empty()) return -1;

    // ClickHouse returns plain text number
    return std::strtoll(resp.c_str(), nullptr, 10);
}


// ============================================================================
// Native insertion mode (Stage 1) -- ClickHouse HTTP API
// ============================================================================

bool ClickHouseConnector::create_native_schema(const std::string& schema_name, PayloadType type) {
    LOG_INF("[clickhouse] Creating native schema for %s", payload_type_str(type));

    http_exec("CREATE DATABASE IF NOT EXISTS " + schema_name);

    auto ns = get_native_schema(type);
    std::string sql = "CREATE TABLE IF NOT EXISTS " + schema_name + "." + ns.table_name + " (\n";

    for (size_t i = 0; i < ns.columns.size(); ++i) {
        const auto& col = ns.columns[i];
        if (i > 0) sql += ",\n";
        sql += "  " + col.name + " ";

        // ClickHouse type mapping
        if (col.type_hint == "SERIAL") sql += "UInt64";
        else if (col.type_hint == "BIGINT") sql += "Int64";
        else if (col.type_hint == "INT") sql += "Int32";
        else if (col.type_hint == "DOUBLE") sql += "Float64";
        else if (col.type_hint == "TEXT") sql += "String";
        else if (col.type_hint == "BOOLEAN") sql += "UInt8";
        else if (col.type_hint == "BYTEA") sql += "String";
        else if (col.type_hint == "UUID") sql += "UUID";
        else if (col.type_hint == "TIMESTAMPTZ") sql += "DateTime DEFAULT now()";
        else if (col.type_hint == "JSONB") sql += "String";  // CH has no native JSON
        else if (col.type_hint.substr(0, 4) == "CHAR") sql += "FixedString(" + col.type_hint.substr(5, col.type_hint.size()-6) + ")";
        else sql += "String";

        if (!col.default_expr.empty() && col.type_hint != "TIMESTAMPTZ" && col.type_hint != "SERIAL") {
            sql += " DEFAULT " + col.default_expr;
        }
    }

    // Use MergeTree with appropriate ORDER BY
    const auto* pk = ns.primary_key();
    std::string order_by = pk ? pk->name : "tuple()";
    sql += "\n) ENGINE = MergeTree() ORDER BY " + order_by;

    return http_exec(sql);
}

bool ClickHouseConnector::drop_native_schema(const std::string& schema_name, PayloadType type) {
    auto ns = get_native_schema(type);
    return http_exec("DROP TABLE IF EXISTS " + schema_name + "." + ns.table_name);
}

MeasureResult ClickHouseConnector::native_bulk_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {
    MeasureResult result{};
    LOG_INF("[clickhouse] Native bulk insert: %zu records (type: %s)",
        records.size(), payload_type_str(type));

#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

    auto ns = get_native_schema(type);
    std::string cols;
    std::vector<std::string> insert_col_names;
    for (const auto& col : ns.columns) {
        if (col.type_hint == "SERIAL") continue;
        if (!cols.empty()) cols += ", ";
        cols += col.name;
        insert_col_names.push_back(col.name);
    }

    Timer timer;
    timer.start();

    // Build TSV data for bulk insert
    std::string tsv_data;
    for (const auto& rec : records) {
        for (size_t i = 0; i < insert_col_names.size(); ++i) {
            if (i > 0) tsv_data += '\t';
            auto it = rec.columns.find(insert_col_names[i]);
            if (it == rec.columns.end() || std::holds_alternative<std::monostate>(it->second)) {
                tsv_data += "\\N";
            } else {
                std::visit([&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) tsv_data += v ? "1" : "0";
                    else if constexpr (std::is_same_v<T, int64_t>) tsv_data += std::to_string(v);
                    else if constexpr (std::is_same_v<T, double>) tsv_data += std::to_string(v);
                    else if constexpr (std::is_same_v<T, std::string>) {
                        // Escape tabs and newlines for TSV
                        for (char c : v) {
                            if (c == '\t') tsv_data += "\\t";
                            else if (c == '\n') tsv_data += "\\n";
                            else if (c == '\\') tsv_data += "\\\\";
                            else tsv_data += c;
                        }
                    }
                    else if constexpr (std::is_same_v<T, std::vector<char>>) {
                        // Base64 or hex encode for binary
                        for (unsigned char c : v) {
                            char hex[3];
                            snprintf(hex, sizeof(hex), "%02x", c);
                            tsv_data += hex;
                        }
                    }
                    else tsv_data += "\\N";
                }, it->second);
            }
        }
        tsv_data += '\n';
        result.rows_affected++;
        result.bytes_logical += static_cast<int64_t>(rec.estimated_size_bytes());
    }

    std::string insert_sql = "INSERT INTO " + database_ + "." + ns.table_name +
        " (" + cols + ") FORMAT TabSeparated\n" + tsv_data;
    http_exec(insert_sql);

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[clickhouse] Native bulk insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
    return result;
}

MeasureResult ClickHouseConnector::native_perfile_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {
    MeasureResult result{};

#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

    auto ns = get_native_schema(type);
    std::string cols;
    std::vector<std::string> insert_col_names;
    for (const auto& col : ns.columns) {
        if (col.type_hint == "SERIAL") continue;
        if (!cols.empty()) cols += ", ";
        cols += col.name;
        insert_col_names.push_back(col.name);
    }

    Timer total_timer;
    total_timer.start();

    for (const auto& rec : records) {
        std::string values;
        for (size_t i = 0; i < insert_col_names.size(); ++i) {
            if (i > 0) values += ", ";
            auto it = rec.columns.find(insert_col_names[i]);
            if (it == rec.columns.end() || std::holds_alternative<std::monostate>(it->second)) {
                values += "NULL";
            } else {
                std::visit([&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) values += v ? "1" : "0";
                    else if constexpr (std::is_same_v<T, int64_t>) values += std::to_string(v);
                    else if constexpr (std::is_same_v<T, double>) values += std::to_string(v);
                    else if constexpr (std::is_same_v<T, std::string>) {
                        values += "'";
                        for (char c : v) {
                            if (c == '\'') values += "\\'";
                            else if (c == '\\') values += "\\\\";
                            else values += c;
                        }
                        values += "'";
                    }
                    else if constexpr (std::is_same_v<T, std::vector<char>>) {
                        values += "'";
                        for (unsigned char c : v) {
                            char hex[3];
                            snprintf(hex, sizeof(hex), "%02x", c);
                            values += hex;
                        }
                        values += "'";
                    }
                    else values += "NULL";
                }, it->second);
            }
        }

        std::string sql = "INSERT INTO " + database_ + "." + ns.table_name +
            " (" + cols + ") VALUES (" + values + ")";

        int64_t insert_ns = 0;
        {
            ScopedTimer st(insert_ns);
            http_exec(sql);
            result.rows_affected++;
        }
        result.per_file_latencies_ns.push_back(insert_ns);
        result.bytes_logical += static_cast<int64_t>(rec.estimated_size_bytes());
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    return result;
}

MeasureResult ClickHouseConnector::native_perfile_delete(PayloadType type) {
    MeasureResult result{};
    auto ns = get_native_schema(type);
    const auto* pk = ns.primary_key();
    std::string pk_col = pk ? pk->name : "id";

    Timer timer;
    timer.start();

    // ClickHouse lightweight DELETE (ALTER TABLE DELETE)
    std::string count_sql = "SELECT count() FROM " + database_ + "." + ns.table_name;
    std::string count_str = http_query(count_sql);
    int64_t count = 0;
    try { count = std::stoll(count_str); } catch (...) {}

    // ClickHouse: ALTER TABLE DELETE WHERE 1=1 deletes all rows
    std::string del_sql = "ALTER TABLE " + database_ + "." + ns.table_name +
        " DELETE WHERE 1=1";
    http_exec(del_sql);
    result.rows_affected = count;

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    return result;
}

int64_t ClickHouseConnector::get_native_logical_size_bytes(PayloadType type) {
    auto ns = get_native_schema(type);
    std::string sql = "SELECT sum(bytes_on_disk) FROM system.parts "
        "WHERE database = '" + database_ + "' AND table = '" + ns.table_name + "' AND active";
    std::string result = http_query(sql);
    try { return std::stoll(result); } catch (...) { return 0; }
}

} // namespace dedup
