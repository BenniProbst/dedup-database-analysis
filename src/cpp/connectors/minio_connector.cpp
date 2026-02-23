#include "minio_connector.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include "../utils/sha256.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>

namespace dedup {
namespace fs = std::filesystem;

// ---- curl write callback ----
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

// ---- HMAC-SHA256 for AWS Signature V4 ----
static std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, size_t key_len,
                                             const void* msg, size_t msg_len) {
    // HMAC: H((K ^ opad) || H((K ^ ipad) || message))
    uint8_t k_pad[64]{};
    if (key_len > 64) {
        auto h = SHA256::hash(key, key_len);
        std::memcpy(k_pad, h.data(), 32);
    } else {
        std::memcpy(k_pad, key, key_len);
    }

    uint8_t i_pad[64], o_pad[64];
    for (int i = 0; i < 64; ++i) {
        i_pad[i] = k_pad[i] ^ 0x36;
        o_pad[i] = k_pad[i] ^ 0x5c;
    }

    // Inner hash: H(ipad || message)
    SHA256 inner;
    inner.update(i_pad, 64);
    inner.update(msg, msg_len);
    auto inner_hash = inner.finalize();

    // Outer hash: H(opad || inner_hash)
    SHA256 outer;
    outer.update(o_pad, 64);
    outer.update(inner_hash.data(), 32);
    return outer.finalize();
}

static std::array<uint8_t, 32> hmac_sha256_str(const uint8_t* key, size_t key_len,
                                                 const std::string& msg) {
    return hmac_sha256(key, key_len, msg.data(), msg.size());
}

// ---- AWS Signature V4 signing ----
static std::string get_utc_date() {
    time_t t = time(nullptr);
    struct tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm_buf);
    return buf;
}

static std::string get_utc_datetime() {
    time_t t = time(nullptr);
    struct tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm_buf);
    return buf;
}

// Generate Authorization header for S3 request
std::string MinioConnector::s3_sign_request(const std::string& method,
                                             const std::string& path,
                                             const std::string& payload_hash) {
    std::string date = get_utc_date();
    std::string datetime = get_utc_datetime();
    std::string region = "us-east-1";
    std::string service = "s3";
    std::string scope = date + "/" + region + "/" + service + "/aws4_request";

    // Canonical request
    std::string canonical = method + "\n"
        + path + "\n"
        + "\n"  // no query string
        + "host:" + s3_host_ + "\n"
        + "x-amz-content-sha256:" + payload_hash + "\n"
        + "x-amz-date:" + datetime + "\n"
        + "\n"
        + "host;x-amz-content-sha256;x-amz-date\n"
        + payload_hash;

    std::string canonical_hash = SHA256::hash_hex(canonical.data(), canonical.size());

    // String to sign
    std::string string_to_sign = "AWS4-HMAC-SHA256\n" + datetime + "\n" + scope + "\n" + canonical_hash;

    // Signing key: HMAC chain
    std::string key_str = "AWS4" + secret_key_;
    auto k_date = hmac_sha256_str(reinterpret_cast<const uint8_t*>(key_str.data()),
                                   key_str.size(), date);
    auto k_region = hmac_sha256_str(k_date.data(), 32, region);
    auto k_service = hmac_sha256_str(k_region.data(), 32, service);
    auto k_signing = hmac_sha256_str(k_service.data(), 32, "aws4_request");

    auto signature = hmac_sha256_str(k_signing.data(), 32, string_to_sign);
    std::string sig_hex = SHA256::to_hex(signature);

    datetime_ = datetime;  // Store for header use

    return "AWS4-HMAC-SHA256 Credential=" + access_key_ + "/" + scope
         + ", SignedHeaders=host;x-amz-content-sha256;x-amz-date"
         + ", Signature=" + sig_hex;
}

// ---- Connection ----

bool MinioConnector::connect(const DbConnection& conn) {
    endpoint_ = "http://" + conn.host + ":" + std::to_string(conn.port);
    s3_host_ = conn.host + ":" + std::to_string(conn.port);
    access_key_ = conn.user;
    secret_key_ = conn.password;
    bucket_prefix_ = conn.lab_schema.empty() ? "dedup-lab" : conn.lab_schema;

#ifdef DEDUP_DRY_RUN
    LOG_INF("[minio] DRY RUN: simulating connection to %s", endpoint_.c_str());
    connected_ = true;
    return true;
#endif

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERR("[minio] curl_easy_init failed");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, (endpoint_ + "/minio/health/live").c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        LOG_ERR("[minio] Health check failed: curl=%d, http=%ld", res, http_code);
        return false;
    }

    connected_ = true;
    LOG_INF("[minio] Connected to %s, bucket prefix: %s-*", endpoint_.c_str(), bucket_prefix_.c_str());
    return true;
}

void MinioConnector::disconnect() {
    connected_ = false;
}

bool MinioConnector::is_connected() const { return connected_; }

// ---- S3 API helpers ----

void* MinioConnector::s3_setup_request(const std::string& method, const std::string& path,
                                        const std::string& payload_hash,
                                        struct curl_slist** out_headers) {
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;

    std::string url = endpoint_ + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    std::string auth = s3_sign_request(method, path, payload_hash);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + auth).c_str());
    headers = curl_slist_append(headers, ("x-amz-date: " + datetime_).c_str());
    headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payload_hash).c_str());
    headers = curl_slist_append(headers, ("Host: " + s3_host_).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    *out_headers = headers;
    return curl;
}

bool MinioConnector::s3_create_bucket(const std::string& bucket) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[minio] DRY RUN: would create bucket %s", bucket.c_str());
    return true;
#endif
    std::string empty_hash = SHA256::hash_hex("", 0);
    struct curl_slist* headers = nullptr;
    CURL* curl = s3_setup_request("PUT", "/" + bucket, empty_hash, &headers);
    if (!curl) return false;

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    bool ok = res == CURLE_OK && (http_code == 200 || http_code == 409);
    if (ok) LOG_INF("[minio] Bucket created: %s (http %ld)", bucket.c_str(), http_code);
    else LOG_ERR("[minio] Bucket creation failed: %s (curl=%d, http=%ld)", bucket.c_str(), res, http_code);
    return ok;
}

bool MinioConnector::s3_put_object(const std::string& bucket, const std::string& key,
                                    const char* data, size_t len) {
#ifdef DEDUP_DRY_RUN
    (void)bucket; (void)key; (void)data; (void)len;
    return true;
#endif
    std::string payload_hash = SHA256::hash_hex(data, len);
    struct curl_slist* headers = nullptr;
    CURL* curl = s3_setup_request("PUT", "/" + bucket + "/" + key, payload_hash, &headers);
    if (!curl) return false;

    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(len));

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK && http_code == 200;
}

bool MinioConnector::s3_delete_object(const std::string& bucket, const std::string& key) {
#ifdef DEDUP_DRY_RUN
    return true;
#endif
    std::string empty_hash = SHA256::hash_hex("", 0);
    struct curl_slist* headers = nullptr;
    CURL* curl = s3_setup_request("DELETE", "/" + bucket + "/" + key, empty_hash, &headers);
    if (!curl) return false;

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK && (http_code == 200 || http_code == 204);
}

bool MinioConnector::s3_delete_bucket(const std::string& bucket) {
#ifdef DEDUP_DRY_RUN
    return true;
#endif
    std::string empty_hash = SHA256::hash_hex("", 0);
    struct curl_slist* headers = nullptr;
    CURL* curl = s3_setup_request("DELETE", "/" + bucket, empty_hash, &headers);
    if (!curl) return false;

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK && (http_code == 200 || http_code == 204);
}

std::vector<std::string> MinioConnector::s3_list_objects(const std::string& bucket) {
    std::vector<std::string> keys;
#ifdef DEDUP_DRY_RUN
    return keys;
#endif

    std::string empty_hash = SHA256::hash_hex("", 0);
    struct curl_slist* headers = nullptr;
    CURL* curl = s3_setup_request("GET", "/" + bucket + "?list-type=2", empty_hash, &headers);
    if (!curl) return keys;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return keys;

    // Simple XML parsing for <Key>...</Key> tags
    size_t pos = 0;
    while ((pos = response.find("<Key>", pos)) != std::string::npos) {
        pos += 5;
        size_t end = response.find("</Key>", pos);
        if (end == std::string::npos) break;
        keys.push_back(response.substr(pos, end - pos));
        pos = end + 6;
    }

    return keys;
}

// ---- Schema management ----

bool MinioConnector::create_lab_schema(const std::string&) {
    LOG_INF("[minio] Creating lab buckets: %s-u0, %s-u50, %s-u90, %s-results",
        bucket_prefix_.c_str(), bucket_prefix_.c_str(),
        bucket_prefix_.c_str(), bucket_prefix_.c_str());
    bool ok = true;
    ok &= s3_create_bucket(bucket_prefix_ + "-u0");
    ok &= s3_create_bucket(bucket_prefix_ + "-u50");
    ok &= s3_create_bucket(bucket_prefix_ + "-u90");
    ok &= s3_create_bucket(bucket_prefix_ + "-results");
    return ok;
}

bool MinioConnector::drop_lab_schema(const std::string&) {
    LOG_WRN("[minio] Dropping lab buckets: %s-*", bucket_prefix_.c_str());

    const char* suffixes[] = {"u0", "u50", "u90", "results"};
    for (const auto& suffix : suffixes) {
        std::string bucket = bucket_prefix_ + "-" + suffix;
        // List and delete all objects first
        auto keys = s3_list_objects(bucket);
        for (const auto& key : keys) {
            s3_delete_object(bucket, key);
        }
        s3_delete_bucket(bucket);
    }
    return true;
}

bool MinioConnector::reset_lab_schema(const std::string& s) {
    return drop_lab_schema(s) && create_lab_schema(s);
}

// ---- Data operations ----

MeasureResult MinioConnector::bulk_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    std::string grade_lower = dup_grade_str(grade);
    std::transform(grade_lower.begin(), grade_lower.end(), grade_lower.begin(), ::tolower);
    const std::string bucket = bucket_prefix_ + "-" + grade_lower;

    LOG_INF("[minio] Bulk upload to bucket %s from %s", bucket.c_str(), dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[minio] DRY RUN: would upload files to %s", bucket.c_str());
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

        std::string key = entry.path().filename().string();
        if (s3_put_object(bucket, key, buf.data(), fsize)) {
            result.rows_affected++;
        }
        result.bytes_logical += fsize;
    }

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[minio] Bulk upload: %lld objects, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
    return result;
}

MeasureResult MinioConnector::perfile_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    std::string grade_lower = dup_grade_str(grade);
    std::transform(grade_lower.begin(), grade_lower.end(), grade_lower.begin(), ::tolower);
    const std::string bucket = bucket_prefix_ + "-" + grade_lower;

    LOG_INF("[minio] Per-file upload to bucket %s from %s", bucket.c_str(), dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[minio] DRY RUN: would per-file upload to %s", bucket.c_str());
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

        std::string key = entry.path().filename().string();

        int64_t put_ns = 0;
        {
            ScopedTimer st(put_ns);
            if (s3_put_object(bucket, key, buf.data(), fsize)) {
                result.rows_affected++;
            }
        }
        result.per_file_latencies_ns.push_back(put_ns);
        result.bytes_logical += fsize;
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[minio] Per-file upload: %lld objects, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, total_timer.elapsed_ms());
    return result;
}

MeasureResult MinioConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[minio] Per-file delete across all lab buckets (with per-object latency)");

#ifdef DEDUP_DRY_RUN
    LOG_INF("[minio] DRY RUN: would delete all objects individually");
    return result;
#endif

    Timer total_timer;
    total_timer.start();

    const char* suffixes[] = {"u0", "u50", "u90"};
    for (const auto& suffix : suffixes) {
        std::string bucket = bucket_prefix_ + "-" + suffix;
        auto keys = s3_list_objects(bucket);
        for (const auto& key : keys) {
            int64_t del_ns = 0;
            {
                ScopedTimer st(del_ns);
                if (s3_delete_object(bucket, key)) {
                    result.rows_affected++;
                }
            }
            result.per_file_latencies_ns.push_back(del_ns);
        }
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[minio] Per-file delete: %lld objects, %lld ms",
        result.rows_affected, total_timer.elapsed_ms());
    return result;
}

MeasureResult MinioConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[minio] No explicit maintenance needed (erasure coding is automatic)");
    return result;
}

int64_t MinioConnector::get_logical_size_bytes() {
#ifdef DEDUP_DRY_RUN
    return 0;
#endif
    // List all objects in lab buckets and sum sizes
    // We use HEAD requests to get Content-Length for each object
    int64_t total = 0;
    const char* suffixes[] = {"u0", "u50", "u90"};

    for (const auto& suffix : suffixes) {
        std::string bucket = bucket_prefix_ + "-" + suffix;
        auto keys = s3_list_objects(bucket);

        for (const auto& key : keys) {
            std::string empty_hash = SHA256::hash_hex("", 0);
            struct curl_slist* headers = nullptr;
            CURL* curl = s3_setup_request("HEAD", "/" + bucket + "/" + key, empty_hash, &headers);
            if (!curl) continue;

            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            CURLcode res = curl_easy_perform(curl);

            if (res == CURLE_OK) {
                double cl = 0;
                curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl);
                if (cl > 0) total += static_cast<int64_t>(cl);
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
    }

    LOG_INF("[minio] Lab bucket total logical size: %lld bytes", total);
    return total;
}


// ============================================================================
// Native insertion mode (Stage 1) -- MinIO JSON/typed objects
// ============================================================================

bool MinioConnector::create_native_schema(const std::string& schema_name, PayloadType type) {
    auto ns = get_native_schema(type);
    std::string bucket = bucket_prefix_ + "-" + ns.table_name;
    LOG_INF("[minio] Creating native bucket: %s", bucket.c_str());
    return s3_create_bucket(bucket);
}

bool MinioConnector::drop_native_schema(const std::string& schema_name, PayloadType type) {
    auto ns = get_native_schema(type);
    std::string bucket = bucket_prefix_ + "-" + ns.table_name;
    // Delete all objects first, then the bucket
    auto keys = s3_list_objects(bucket);
    for (const auto& key : keys) {
        s3_delete_object(bucket, key);
    }
    return s3_delete_bucket(bucket);
}

MeasureResult MinioConnector::native_bulk_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {
    MeasureResult result{};
    LOG_INF("[minio] Native bulk insert: %zu records (type: %s)",
        records.size(), payload_type_str(type));

#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

    auto ns = get_native_schema(type);
    std::string bucket = bucket_prefix_ + "-" + ns.table_name;

    Timer timer;
    timer.start();

    int64_t idx = 0;
    for (const auto& rec : records) {
        // Determine content type and object key
        std::string content_type = "application/json";
        std::string object_key = std::to_string(idx);
        std::string body;

        // Check if this is a binary type
        auto payload_it = rec.columns.find("payload");
        if (payload_it != rec.columns.end() &&
            std::holds_alternative<std::vector<char>>(payload_it->second)) {
            // Binary object
            auto mime_it = rec.columns.find("mime");
            if (mime_it != rec.columns.end() && std::holds_alternative<std::string>(mime_it->second))
                content_type = std::get<std::string>(mime_it->second);
            auto fn_it = rec.columns.find("filename");
            if (fn_it != rec.columns.end() && std::holds_alternative<std::string>(fn_it->second))
                object_key = std::get<std::string>(fn_it->second);
            else
                object_key = "obj_" + std::to_string(idx) + ".bin";

            const auto& payload = std::get<std::vector<char>>(payload_it->second);
            body.assign(payload.begin(), payload.end());
        } else {
            // JSON serialization
            object_key = "record_" + std::to_string(idx) + ".json";
            body = "{";
            bool first = true;
            for (const auto& [col_name, val] : rec.columns) {
                if (!first) body += ",";
                first = false;
                body += "\"" + col_name + "\":";
                std::visit([&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) body += "null";
                    else if constexpr (std::is_same_v<T, bool>) body += v ? "true" : "false";
                    else if constexpr (std::is_same_v<T, int64_t>) body += std::to_string(v);
                    else if constexpr (std::is_same_v<T, double>) body += std::to_string(v);
                    else if constexpr (std::is_same_v<T, std::string>) {
                        body += "\"";
                        for (char c : v) {
                            if (c == '"') body += "\\\"";
                            else if (c == '\\') body += "\\\\";
                            else if (c == '\n') body += "\\n";
                            else body += c;
                        }
                        body += "\"";
                    }
                    else if constexpr (std::is_same_v<T, std::vector<char>>) {
                        body += "\"(binary:" + std::to_string(v.size()) + "bytes)\"";
                    }
                }, val);
            }
            body += "}";
        }

        if (s3_put_object(bucket, object_key, body.data(), body.size())) {
            result.rows_affected++;
        }
        result.bytes_logical += static_cast<int64_t>(body.size());
        idx++;
    }

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[minio] Native bulk insert: %lld rows, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
    return result;
}

MeasureResult MinioConnector::native_perfile_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {
    MeasureResult result{};
#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

    auto ns = get_native_schema(type);
    std::string bucket = bucket_prefix_ + "-" + ns.table_name;

    Timer total_timer;
    total_timer.start();

    int64_t idx = 0;
    for (const auto& rec : records) {
        std::string content_type = "application/json";
        std::string object_key = "record_" + std::to_string(idx) + ".json";
        std::string body;

        auto payload_it = rec.columns.find("payload");
        if (payload_it != rec.columns.end() &&
            std::holds_alternative<std::vector<char>>(payload_it->second)) {
            auto mime_it = rec.columns.find("mime");
            if (mime_it != rec.columns.end() && std::holds_alternative<std::string>(mime_it->second))
                content_type = std::get<std::string>(mime_it->second);
            auto fn_it = rec.columns.find("filename");
            if (fn_it != rec.columns.end() && std::holds_alternative<std::string>(fn_it->second))
                object_key = std::get<std::string>(fn_it->second);
            else
                object_key = "obj_" + std::to_string(idx) + ".bin";
            const auto& payload = std::get<std::vector<char>>(payload_it->second);
            body.assign(payload.begin(), payload.end());
        } else {
            body = "{";
            bool first = true;
            for (const auto& [col_name, val] : rec.columns) {
                if (!first) body += ",";
                first = false;
                body += "\"" + col_name + "\":";
                std::visit([&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) body += "null";
                    else if constexpr (std::is_same_v<T, bool>) body += v ? "true" : "false";
                    else if constexpr (std::is_same_v<T, int64_t>) body += std::to_string(v);
                    else if constexpr (std::is_same_v<T, double>) body += std::to_string(v);
                    else if constexpr (std::is_same_v<T, std::string>) {
                        body += "\"";
                        for (char c : v) {
                            if (c == '"') body += "\\\"";
                            else if (c == '\\') body += "\\\\";
                            else if (c == '\n') body += "\\n";
                            else body += c;
                        }
                        body += "\"";
                    }
                    else if constexpr (std::is_same_v<T, std::vector<char>>) {
                        body += "\"(binary)\"";
                    }
                }, val);
            }
            body += "}";
        }

        int64_t put_ns = 0;
        {
            ScopedTimer st(put_ns);
            if (s3_put_object(bucket, object_key, body.data(), body.size()))
                result.rows_affected++;
        }
        result.per_file_latencies_ns.push_back(put_ns);
        result.bytes_logical += static_cast<int64_t>(body.size());
        idx++;
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    return result;
}

MeasureResult MinioConnector::native_perfile_delete(PayloadType type) {
    // Same as BLOB delete -- list and delete all objects in bucket
    return perfile_delete();
}

int64_t MinioConnector::get_native_logical_size_bytes(PayloadType type) {
    return get_logical_size_bytes();
}

} // namespace dedup
