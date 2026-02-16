#pragma once
// PostgreSQL connector -- also serves CockroachDB (PG wire protocol)
// Uses libpq (C API) for minimal overhead on N97
#include "db_connector.hpp"
#include <libpq-fe.h>

namespace dedup {

class PostgresConnector : public DbConnector {
public:
    explicit PostgresConnector(DbSystem sys = DbSystem::POSTGRESQL)
        : system_(sys) {}

    ~PostgresConnector() override { disconnect(); }

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

    [[nodiscard]] DbSystem system() const override { return system_; }
    [[nodiscard]] const char* system_name() const override { return db_system_str(system_); }

private:
    DbSystem system_;
    PGconn* conn_ = nullptr;
    std::string schema_;

    // Execute SQL with error checking, returns true on success
    bool exec(const char* sql);
    // Execute SQL and return result set (caller must PQclear)
    PGresult* query(const char* sql);
};

} // namespace dedup
