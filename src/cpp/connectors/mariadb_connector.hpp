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

private:
    void* conn_ = nullptr;  // MYSQL* handle
    std::string schema_;
    bool connected_ = false;
};

} // namespace dedup
