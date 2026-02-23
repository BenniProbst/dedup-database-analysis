#pragma once
// MinIO/S3 connector -- uses bucket prefix "dedup-lab-*" for isolation
// Production buckets are NEVER touched (gitlab-*, buildsystem-*)
// Uses libcurl + AWS Signature V4 for proper S3 authentication
#include "db_connector.hpp"
#include <vector>

struct curl_slist;

namespace dedup {

class MinioConnector : public DbConnector {
public:
    ~MinioConnector() override { disconnect(); }

    bool connect(const DbConnection& conn) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const override;

    bool create_lab_schema(const std::string& schema_name) override;
    bool drop_lab_schema(const std::string& schema_name) override;
    bool reset_lab_schema(const std::string& schema_name) override;

    MeasureResult bulk_insert(const std::string& data_dir, DupGrade grade) override;
    MeasureResult perfile_insert(const std::string& data_dir, DupGrade grade) override;
    MeasureResult perfile_delete() override;
    MeasureResult run_maintenance() override;

    int64_t get_logical_size_bytes() override;

    [[nodiscard]] DbSystem system() const override { return DbSystem::MINIO; }
    [[nodiscard]] const char* system_name() const override { return "minio"; }


    // Native insertion mode (Stage 1)
    bool create_native_schema(const std::string& schema_name, PayloadType type) override;
    bool drop_native_schema(const std::string& schema_name, PayloadType type) override;
    MeasureResult native_bulk_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_delete(PayloadType type) override;
    int64_t get_native_logical_size_bytes(PayloadType type) override;

private:
    std::string endpoint_;
    std::string s3_host_;
    std::string access_key_;
    std::string secret_key_;
    std::string bucket_prefix_ = "dedup-lab";
    std::string datetime_;     // Cached for current request signing
    bool connected_ = false;

    // AWS Signature V4 signing
    std::string s3_sign_request(const std::string& method, const std::string& path,
                                 const std::string& payload_hash);

    // Setup an authenticated S3 request with proper headers
    // Returns CURL* as void* to avoid #include <curl/curl.h> in header
    void* s3_setup_request(const std::string& method, const std::string& path,
                           const std::string& payload_hash,
                           struct curl_slist** out_headers);

    // S3 API operations
    bool s3_put_object(const std::string& bucket, const std::string& key,
                       const char* data, size_t len);
    bool s3_delete_object(const std::string& bucket, const std::string& key);
    bool s3_create_bucket(const std::string& bucket);
    bool s3_delete_bucket(const std::string& bucket);
    std::vector<std::string> s3_list_objects(const std::string& bucket);
};

} // namespace dedup
