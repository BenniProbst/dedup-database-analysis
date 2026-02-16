#include "minio_connector.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>

namespace dedup {
namespace fs = std::filesystem;

bool MinioConnector::connect(const DbConnection& conn) {
    endpoint_ = "http://" + conn.host + ":" + std::to_string(conn.port);
    access_key_ = conn.user;
    secret_key_ = conn.password;
    bucket_prefix_ = conn.lab_schema.empty() ? "dedup-lab" : conn.lab_schema;

#ifdef DEDUP_DRY_RUN
    LOG_INF("[minio] DRY RUN: simulating connection to %s", endpoint_.c_str());
    connected_ = true;
    return true;
#endif

    // Test connection with a HEAD request to endpoint
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

bool MinioConnector::s3_create_bucket(const std::string& bucket) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[minio] DRY RUN: would create bucket %s", bucket.c_str());
    return true;
#endif
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = endpoint_ + "/" + bucket;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_USERNAME, access_key_.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, secret_key_.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    return res == CURLE_OK && (http_code == 200 || http_code == 409);  // 409 = already exists
}

bool MinioConnector::s3_put_object(const std::string& bucket, const std::string& key,
                                    const char* data, size_t len) {
#ifdef DEDUP_DRY_RUN
    (void)bucket; (void)key; (void)data; (void)len;
    return true;
#endif
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = endpoint_ + "/" + bucket + "/" + key;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(len));
    curl_easy_setopt(curl, CURLOPT_USERNAME, access_key_.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, secret_key_.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK && http_code == 200;
}

bool MinioConnector::s3_delete_object(const std::string& bucket, const std::string& key) {
    (void)bucket; (void)key;
    // TODO: implement S3 DELETE
    return true;
}

bool MinioConnector::s3_delete_bucket(const std::string& bucket) {
    (void)bucket;
    // TODO: implement bucket deletion (must be empty first)
    return true;
}

bool MinioConnector::create_lab_schema(const std::string&) {
    LOG_INF("[minio] Creating lab buckets: %s-U0, %s-U50, %s-U90",
        bucket_prefix_.c_str(), bucket_prefix_.c_str(), bucket_prefix_.c_str());
    bool ok = true;
    ok &= s3_create_bucket(bucket_prefix_ + "-u0");
    ok &= s3_create_bucket(bucket_prefix_ + "-u50");
    ok &= s3_create_bucket(bucket_prefix_ + "-u90");
    return ok;
}

bool MinioConnector::drop_lab_schema(const std::string&) {
    LOG_WRN("[minio] Dropping lab buckets: %s-*", bucket_prefix_.c_str());
    // TODO: list and delete all objects, then delete buckets
    return true;
}

bool MinioConnector::reset_lab_schema(const std::string& s) {
    return drop_lab_schema(s) && create_lab_schema(s);
}

MeasureResult MinioConnector::bulk_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    const std::string bucket = bucket_prefix_ + "-" + dup_grade_str(grade);

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
    return bulk_insert(data_dir, grade);  // S3 PUT is already per-object
}

MeasureResult MinioConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[minio] Per-file delete (TODO: list and delete all objects)");
    return result;
}

MeasureResult MinioConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[minio] No explicit maintenance (erasure coding is automatic)");
    return result;
}

int64_t MinioConnector::get_logical_size_bytes() {
    // Would need to list all objects and sum sizes
    return -1;
}

} // namespace dedup
