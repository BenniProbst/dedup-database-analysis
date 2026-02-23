#pragma once
#include "db_connector.hpp"

namespace dedup {

// Redis connector -- uses key-prefix for lab isolation (cluster mode, no SELECT)
// All lab keys use prefix "dedup:" -- production keys have NO such prefix.
class RedisConnector : public DbConnector {
public:
    ~RedisConnector() override { disconnect(); }

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

    [[nodiscard]] DbSystem system() const override { return DbSystem::REDIS; }
    [[nodiscard]] const char* system_name() const override { return "redis"; }


    // Native insertion mode (Stage 1)
    bool create_native_schema(const std::string& schema_name, PayloadType type) override;
    bool drop_native_schema(const std::string& schema_name, PayloadType type) override;
    MeasureResult native_bulk_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_delete(PayloadType type) override;
    int64_t get_native_logical_size_bytes(PayloadType type) override;

private:
    static constexpr const char* KEY_PREFIX = "dedup:";
    int64_t delete_all_lab_keys();  // SCAN + DEL for dedup:* keys
    void* ctx_ = nullptr;  // redisContext* or raw socket
    bool connected_ = false;
};

} // namespace dedup
