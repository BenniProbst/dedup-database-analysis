#pragma once
// =============================================================================
// DB-Internal Metrics -- Stage-boundary snapshots
//
// Captures database-internal state at experiment stage boundaries for
// fine-grained accuracy beyond Longhorn-level physical size tracking.
// Specified in doku.tex Section 6.5 "Database-internal instrumentation".
//
// Unlike the 100ms MetricsTrace collectors (metrics_trace.hpp), these
// snapshots may run heavier queries that are only appropriate at specific
// experiment milestones (before/after each stage).
//
// Per-system queries:
//   PostgreSQL:  pg_total_relation_size, pg_statio_all_tables, pg_stat_statements
//   CockroachDB: pg_database_size, crdb_internal.kv_store_status, SHOW RANGES
//   Redis:       MEMORY STATS decomposition, DBSIZE
//   Kafka:       JMX RequestMetrics timing, log dir sizes
//   MinIO:       minio_s3_ttfb_seconds_distribution, per-bucket sizes
//   MariaDB:     INNODB_TABLESPACES sizes, performance_schema IO waits
//   ClickHouse:  system.columns compression ratio, system.parts
// =============================================================================

#include <nlohmann/json.hpp>
#include "../config.hpp"

namespace dedup {
namespace db_internal {

// Capture a DB-internal state snapshot for one database.
// Returns a JSON object with system-specific metrics.
// Called at stage boundaries (before/after) in data_loader.cpp.
nlohmann::json snapshot(const DbConnection& conn);

// Per-system snapshot implementations
nlohmann::json snapshot_postgresql(const DbConnection& conn);
nlohmann::json snapshot_cockroachdb(const DbConnection& conn);
nlohmann::json snapshot_redis(const DbConnection& conn);
nlohmann::json snapshot_kafka(const DbConnection& conn);
nlohmann::json snapshot_minio(const DbConnection& conn);
nlohmann::json snapshot_mariadb(const DbConnection& conn);
nlohmann::json snapshot_clickhouse(const DbConnection& conn);

} // namespace db_internal
} // namespace dedup
