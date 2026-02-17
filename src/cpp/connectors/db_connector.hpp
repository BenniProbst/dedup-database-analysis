#pragma once
// Abstract interface for all database connectors
// Each connector manages its own lab schema isolation
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include "../config.hpp"

namespace dedup {

// Result of a single measurement operation
struct MeasureResult {
    int64_t duration_ns = 0;      // Wall-clock time in nanoseconds
    int64_t rows_affected = 0;    // Number of rows/records/objects affected
    int64_t bytes_logical = 0;    // Logical data size (as reported by DB)
    std::string error;            // Empty on success

    // Per-file latency tracking for histogram analysis (doku.tex Stage 2/3)
    std::vector<int64_t> per_file_latencies_ns;
};

// Abstract database connector interface
class DbConnector {
public:
    virtual ~DbConnector() = default;

    // Connect to the database using lab credentials
    virtual bool connect(const DbConnection& conn) = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;

    // Lab schema management -- CRITICAL for production safety
    virtual bool create_lab_schema(const std::string& schema_name) = 0;
    virtual bool drop_lab_schema(const std::string& schema_name) = 0;
    virtual bool reset_lab_schema(const std::string& schema_name) = 0;

    // Data operations (all operate ONLY on lab schema)
    virtual MeasureResult bulk_insert(const std::string& data_dir, DupGrade grade) = 0;
    virtual MeasureResult perfile_insert(const std::string& data_dir, DupGrade grade) = 0;
    virtual MeasureResult perfile_delete() = 0;

    // Maintenance (system-specific: VACUUM, compaction, retention, etc.)
    virtual MeasureResult run_maintenance() = 0;

    // Query logical size of lab schema data
    virtual int64_t get_logical_size_bytes() = 0;

    // System identification
    [[nodiscard]] virtual DbSystem system() const = 0;
    [[nodiscard]] virtual const char* system_name() const = 0;
};

} // namespace dedup
