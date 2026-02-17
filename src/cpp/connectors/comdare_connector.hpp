#pragma once
// comdare-DB connector -- experimental system, treated as black box (doku.tex 5.2)
// Uses HTTP/REST API for data operations (endpoint configurable)
// Lab schema: database-level isolation (CREATE DATABASE dedup_lab)
// Maintenance: POST /api/v1/maintenance (system-specific compaction/GC)
// Size query: GET /api/v1/stats (returns JSON with logical_size_bytes)
//
// TODO: comdare-DB K8s deployment + actual API integration
#include "db_connector.hpp"

namespace dedup {

class ComdareConnector : public DbConnector {
public:
    ~ComdareConnector() override { disconnect(); }

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

    [[nodiscard]] DbSystem system() const override { return DbSystem::COMDARE_DB; }
    [[nodiscard]] const char* system_name() const override { return "comdare-db"; }

private:
    std::string endpoint_;    // HTTP API endpoint (http://host:port)
    std::string database_;    // Lab database name
    bool connected_ = false;

    // HTTP helpers using libcurl
    std::string http_get(const std::string& path);
    std::string http_post(const std::string& path, const std::string& body,
                          const std::string& content_type = "application/json");
};

} // namespace dedup
