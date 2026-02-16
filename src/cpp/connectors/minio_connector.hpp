#pragma once
#include "db_connector.hpp"

namespace dedup {

// MinIO/S3 connector -- uses bucket prefix "dedup-lab-*" for isolation
// Production buckets are NEVER touched (gitlab-*, buildsystem-*)
// Uses libcurl for S3-compatible API (no heavy AWS SDK needed)
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

private:
    std::string endpoint_;
    std::string access_key_;
    std::string secret_key_;
    std::string bucket_prefix_ = "dedup-lab";
    bool connected_ = false;

    // S3 API helpers using libcurl
    bool s3_put_object(const std::string& bucket, const std::string& key,
                       const char* data, size_t len);
    bool s3_delete_object(const std::string& bucket, const std::string& key);
    bool s3_create_bucket(const std::string& bucket);
    bool s3_delete_bucket(const std::string& bucket);
};

} // namespace dedup
