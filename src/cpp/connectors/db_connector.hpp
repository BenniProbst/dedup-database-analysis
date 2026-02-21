#pragma once
// Abstract interface for all database connectors
// Each connector manages its own lab schema isolation
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include <thread>
#include <chrono>
#include "../config.hpp"
#include "../utils/logger.hpp"

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

    // Connection resilience: reconnect after connection loss.
    // Default: disconnect + connect. Override for protocol-specific reset (e.g. PQreset).
    virtual bool reconnect(const DbConnection& conn) {
        disconnect();
        return connect(conn);
    }

    // Ensure connection is alive, retry with exponential backoff if lost.
    // Returns true if connected (either already was or successfully reconnected).
    // Backoff: base_delay_ms * 2^(attempt-1), capped at 30s.
    bool ensure_connected(const DbConnection& conn,
                          int max_retries = 5, int base_delay_ms = 1000) {
        if (is_connected()) return true;

        LOG_WRN("[%s] Connection lost! Starting reconnection (max %d retries)...",
            system_name(), max_retries);

        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            int delay = base_delay_ms * (1 << (attempt - 1));
            if (delay > 30000) delay = 30000;  // cap at 30s

            LOG_WRN("[%s] Reconnect attempt %d/%d (backoff: %d ms)...",
                system_name(), attempt, max_retries, delay);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));

            if (reconnect(conn)) {
                LOG_INF("[%s] Reconnected successfully on attempt %d",
                    system_name(), attempt);
                return true;
            }
            LOG_ERR("[%s] Reconnect attempt %d failed", system_name(), attempt);
        }

        LOG_ERR("[%s] All %d reconnection attempts FAILED", system_name(), max_retries);
        return false;
    }
};

} // namespace dedup
