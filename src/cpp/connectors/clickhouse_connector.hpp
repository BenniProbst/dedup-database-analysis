#pragma once
// ClickHouse connector -- column-oriented analytical DBMS
// Required by doku.tex Section 5.2: "aggressive compression"
// Uses ClickHouse HTTP API (port 8123) or clickhouse-cpp library
// TODO: Install ClickHouse in K8s cluster (StatefulSet with Longhorn PVC)
#include "db_connector.hpp"

namespace dedup {

class ClickHouseConnector : public DbConnector {
public:
    ~ClickHouseConnector() override { disconnect(); }

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

    [[nodiscard]] DbSystem system() const override { return DbSystem::CLICKHOUSE; }
    [[nodiscard]] const char* system_name() const override { return "clickhouse"; }


    // Native insertion mode (Stage 1)
    bool create_native_schema(const std::string& schema_name, PayloadType type) override;
    bool drop_native_schema(const std::string& schema_name, PayloadType type) override;
    MeasureResult native_bulk_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_delete(PayloadType type) override;
    int64_t get_native_logical_size_bytes(PayloadType type) override;

private:
    std::string endpoint_;    // HTTP API endpoint (http://host:8123)
    std::string database_;
    std::string user_;        // ClickHouse user (e.g. dedup_lab)
    std::string password_;    // ClickHouse password
    bool connected_ = false;

    // HTTP query helper using libcurl
    std::string http_query(const std::string& sql);
    bool http_exec(const std::string& sql);
};

} // namespace dedup
