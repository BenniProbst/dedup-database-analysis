#pragma once
// MariaDB connector -- uses MySQL wire protocol (libmysqlclient)
// Required by doku.tex Section 5.2 as a relational DBMS under test.
// TODO: Install MariaDB in K8s cluster (StatefulSet with Longhorn PVC)
#include "db_connector.hpp"

namespace dedup {

class MariaDBConnector : public DbConnector {
public:
    ~MariaDBConnector() override { disconnect(); }

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

    [[nodiscard]] DbSystem system() const override { return DbSystem::MARIADB; }
    [[nodiscard]] const char* system_name() const override { return "mariadb"; }


    // Native insertion mode (Stage 1)
    bool create_native_schema(const std::string& schema_name, PayloadType type) override;
    bool drop_native_schema(const std::string& schema_name, PayloadType type) override;
    MeasureResult native_bulk_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_delete(PayloadType type) override;
    int64_t get_native_logical_size_bytes(PayloadType type) override;

private:
    void* conn_ = nullptr;  // MYSQL* handle
    std::string schema_;
    bool connected_ = false;
};

} // namespace dedup
