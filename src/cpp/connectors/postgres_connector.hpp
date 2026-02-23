#pragma once
// PostgreSQL connector -- also serves CockroachDB (PG wire protocol)
// Uses libpq (C API) for minimal overhead on N97
//
// Extended for native insertion mode (Stage 1):
//   - create_native_schema: Creates typed table per PayloadType
//   - native_bulk_insert: Inserts NativeRecords in transaction batch
//   - native_perfile_insert: Inserts NativeRecords with per-record latency
//   - native_perfile_delete: Deletes all records from native table
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

    // Override: use PQreset for faster reconnection (reuses connection params)
    bool reconnect(const DbConnection& conn) override;

    // Native insertion mode (Stage 1)
    bool create_native_schema(const std::string& schema_name, PayloadType type) override;
    bool drop_native_schema(const std::string& schema_name, PayloadType type) override;
    MeasureResult native_bulk_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_insert(const std::vector<NativeRecord>& records, PayloadType type) override;
    MeasureResult native_perfile_delete(PayloadType type) override;
    int64_t get_native_logical_size_bytes(PayloadType type) override;

private:
    DbSystem system_;
    PGconn* conn_ = nullptr;
    std::string schema_;

    // Execute SQL with error checking, returns true on success
    bool exec(const char* sql);
    // Execute SQL and return result set (caller must PQclear)
    PGresult* query(const char* sql);

    // Native helpers
    std::string pg_type_for(const ColumnDef& col) const;
    std::string build_create_table_sql(const NativeSchema& schema) const;
    std::string build_insert_sql(const NativeSchema& schema) const;
    bool bind_and_exec_native(const std::string& sql, const NativeSchema& schema,
                               const NativeRecord& record);
};

} // namespace dedup
