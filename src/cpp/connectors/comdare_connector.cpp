// comdare-DB connector -- experimental system, black box measurement
// Per doku.tex 5.2: "treated as a black box in measurement and compared empirically"
//
// API surface (assumed REST):
//   POST /api/v1/databases/{name}           -- create database
//   DELETE /api/v1/databases/{name}         -- drop database
//   POST /api/v1/databases/{name}/ingest    -- bulk/per-file insert (multipart)
//   DELETE /api/v1/databases/{name}/objects  -- delete all objects
//   POST /api/v1/databases/{name}/maintain  -- compaction/GC
//   GET  /api/v1/databases/{name}/stats     -- {"logical_size_bytes": N}
//
// TODO: Align endpoints with actual comdare-DB REST API once deployed

#include "comdare_connector.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include "../utils/sha256.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace dedup {
namespace fs = std::filesystem;

static size_t cd_write_cb(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string ComdareConnector::http_get(const std::string& path) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[comdare-db] DRY RUN GET %s", path.c_str());
    return R"({"logical_size_bytes":0})";
#endif
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string url = endpoint_ + path;
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cd_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERR("[comdare-db] GET %s failed: %s", path.c_str(), curl_easy_strerror(res));
        return "";
    }
    if (http_code >= 400) {
        LOG_ERR("[comdare-db] GET %s returned HTTP %ld", path.c_str(), http_code);
        return "";
    }
    return response;
}

std::string ComdareConnector::http_post(const std::string& path, const std::string& body,
                                         const std::string& content_type) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[comdare-db] DRY RUN POST %s (%zu bytes)", path.c_str(), body.size());
    return "{}";
#endif
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string url = endpoint_ + path;
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cd_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERR("[comdare-db] POST %s failed: %s", path.c_str(), curl_easy_strerror(res));
        return "";
    }
    if (http_code >= 400) {
        LOG_ERR("[comdare-db] POST %s returned HTTP %ld: %s",
            path.c_str(), http_code, response.c_str());
        return "";
    }
    return response;
}

bool ComdareConnector::connect(const DbConnection& conn) {
    endpoint_ = "http://" + conn.host + ":" + std::to_string(conn.port);
    database_ = conn.lab_schema.empty() ? "dedup_lab" : conn.lab_schema;

#ifdef DEDUP_DRY_RUN
    LOG_INF("[comdare-db] DRY RUN: simulating connection to %s", endpoint_.c_str());
    connected_ = true;
    return true;
#endif

    // Health check: GET /api/v1/health
    std::string resp = http_get("/api/v1/health");
    if (resp.empty()) {
        LOG_ERR("[comdare-db] Health check failed for %s", endpoint_.c_str());
        return false;
    }

    connected_ = true;
    LOG_INF("[comdare-db] Connected to %s (database: %s)", endpoint_.c_str(), database_.c_str());
    return true;
}

void ComdareConnector::disconnect() {
    connected_ = false;
}

bool ComdareConnector::is_connected() const { return connected_; }

bool ComdareConnector::create_lab_schema(const std::string& schema_name) {
    LOG_INF("[comdare-db] Creating lab database: %s", schema_name.c_str());
#ifdef DEDUP_DRY_RUN
    LOG_INF("[comdare-db] DRY RUN: would create database %s", schema_name.c_str());
    return true;
#endif
    std::string resp = http_post("/api/v1/databases/" + schema_name, "{}");
    return !resp.empty();
}

bool ComdareConnector::drop_lab_schema(const std::string& schema_name) {
    LOG_WRN("[comdare-db] DROPPING lab database: %s", schema_name.c_str());
#ifdef DEDUP_DRY_RUN
    return true;
#endif
    // Use DELETE method via custom request
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = endpoint_ + "/api/v1/databases/" + schema_name;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

bool ComdareConnector::reset_lab_schema(const std::string& schema_name) {
    return drop_lab_schema(schema_name) && create_lab_schema(schema_name);
}

// POST with metadata headers (X-Filename, X-SHA256, X-Size-Bytes)
std::string ComdareConnector::http_post_with_metadata(
    const std::string& path, const char* data, size_t len,
    const std::string& filename, const std::string& sha256, size_t size_bytes) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[comdare-db] DRY RUN POST %s (%zu bytes, file=%s)", path.c_str(), len, filename.c_str());
    return "{}";
#endif
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string url = endpoint_ + path;
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    headers = curl_slist_append(headers, ("X-Filename: " + filename).c_str());
    headers = curl_slist_append(headers, ("X-SHA256: " + sha256).c_str());
    headers = curl_slist_append(headers, ("X-Size-Bytes: " + std::to_string(size_bytes)).c_str());
    headers = curl_slist_append(headers, "X-MIME: application/octet-stream");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(len));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cd_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code >= 400) {
        LOG_ERR("[comdare-db] POST %s failed: curl=%d, http=%ld", path.c_str(), res, http_code);
        return "";
    }
    return response;
}

MeasureResult ComdareConnector::bulk_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    LOG_INF("[comdare-db] Bulk insert from %s", dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[comdare-db] DRY RUN: would bulk-insert files");
    result.rows_affected = 42;
    return result;
#endif

    Timer timer;
    timer.start();

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string sha256 = SHA256::hash_hex(buf.data(), fsize);
        std::string filename = entry.path().filename().string();

        std::string path = "/api/v1/databases/" + database_ + "/ingest";
        std::string resp = http_post_with_metadata(path, buf.data(), fsize,
                                                    filename, sha256, fsize);
        if (!resp.empty()) {
            result.rows_affected++;
        }
        result.bytes_logical += fsize;
    }

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[comdare-db] Bulk insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
    return result;
}

MeasureResult ComdareConnector::perfile_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    LOG_INF("[comdare-db] Per-file insert from %s", dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[comdare-db] DRY RUN: would per-file-insert files");
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
        std::string filename = entry.path().filename().string();
        std::string path = "/api/v1/databases/" + database_ + "/ingest";

        int64_t insert_ns = 0;
        {
            ScopedTimer st(insert_ns);
            std::string resp = http_post_with_metadata(path, buf.data(), fsize,
                                                        filename, sha256, fsize);
            if (!resp.empty()) {
                result.rows_affected++;
            }
        }
        result.per_file_latencies_ns.push_back(insert_ns);
        result.bytes_logical += fsize;
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[comdare-db] Per-file insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, total_timer.elapsed_ms());
    return result;
}

MeasureResult ComdareConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[comdare-db] Per-file delete (individual object deletion)");

#ifdef DEDUP_DRY_RUN
    LOG_INF("[comdare-db] DRY RUN: would delete objects individually");
    return result;
#endif

    Timer total_timer;
    total_timer.start();

    // Get list of object IDs via stats endpoint
    std::string list_resp = http_get("/api/v1/databases/" + database_ + "/objects");
    if (list_resp.empty()) {
        // Fallback: bulk delete without per-object latency
        LOG_WRN("[comdare-db] Could not list objects -- falling back to bulk delete");
        CURL* curl = curl_easy_init();
        if (curl) {
            std::string url = endpoint_ + "/api/v1/databases/" + database_ + "/objects";
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }
        total_timer.stop();
        result.duration_ns = total_timer.elapsed_ns();
        return result;
    }

    // Parse object IDs from JSON array
    std::vector<std::string> object_ids;
    try {
        auto j = nlohmann::json::parse(list_resp);
        if (j.is_array()) {
            for (const auto& item : j) {
                if (item.is_string()) {
                    object_ids.push_back(item.get<std::string>());
                } else if (item.contains("id")) {
                    object_ids.push_back(item["id"].get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WRN("[comdare-db] Could not parse object list: %s", e.what());
    }

    LOG_INF("[comdare-db] Deleting %zu objects individually", object_ids.size());

    for (const auto& obj_id : object_ids) {
        int64_t del_ns = 0;
        {
            ScopedTimer st(del_ns);
            CURL* curl = curl_easy_init();
            if (curl) {
                std::string url = endpoint_ + "/api/v1/databases/" + database_ + "/objects/" + obj_id;
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

                CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                curl_easy_cleanup(curl);

                if (res == CURLE_OK && http_code < 400) {
                    result.rows_affected++;
                }
            }
        }
        result.per_file_latencies_ns.push_back(del_ns);
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[comdare-db] Per-file delete: %lld objects, %lld ms",
        result.rows_affected, total_timer.elapsed_ms());
    return result;
}

MeasureResult ComdareConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[comdare-db] Running maintenance (compaction/GC)");

#ifdef DEDUP_DRY_RUN
    LOG_INF("[comdare-db] DRY RUN: would trigger maintenance");
    return result;
#endif

    Timer timer;
    timer.start();
    http_post("/api/v1/databases/" + database_ + "/maintain", "{}");
    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[comdare-db] Maintenance complete: %lld ms", timer.elapsed_ms());
    return result;
}

int64_t ComdareConnector::get_logical_size_bytes() {
#ifdef DEDUP_DRY_RUN
    return 0;
#endif

    std::string resp = http_get("/api/v1/databases/" + database_ + "/stats");
    if (resp.empty()) return -1;

    try {
        auto j = nlohmann::json::parse(resp);
        return j.value("logical_size_bytes", static_cast<int64_t>(-1));
    } catch (const std::exception& e) {
        LOG_ERR("[comdare-db] Stats JSON parse error: %s", e.what());
        return -1;
    }
}

} // namespace dedup
