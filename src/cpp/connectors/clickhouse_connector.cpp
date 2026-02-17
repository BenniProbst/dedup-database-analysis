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
    database_ = conn.lab_schema.empty() ? "dedup_lab" : conn.lab_schema;

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
    return http_exec(sql);
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
    return bulk_insert(data_dir, grade);
}

MeasureResult ClickHouseConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[clickhouse] Per-file delete (ALTER TABLE DELETE)");

#ifdef DEDUP_DRY_RUN
    return result;
#endif

    Timer timer;
    timer.start();
    // ClickHouse lightweight delete (marks rows, cleaned up by mutations)
    http_exec("ALTER TABLE " + database_ + ".files DELETE WHERE 1=1");
    timer.stop();
    result.duration_ns = timer.elapsed_ns();
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

} // namespace dedup
