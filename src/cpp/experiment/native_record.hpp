#pragma once
// =============================================================================
// Native Record Data Structures for Structured Insertion
// TU Dresden Research Project "Deduplikation in Datenhaltungssystemen"
//
// Defines typed records for native/idiomatic insertion into each DB system.
// Stage 1 (doku.tex Â§5.4): Data is inserted using the DB's most natural
// schema and data types, enabling DB-specific optimizations like:
//   - PostgreSQL TOAST compression, JSONB operators
//   - ClickHouse columnar compression (Float64, FixedString)
//   - MariaDB JSON column type, InnoDB row format
//   - Redis HSET with typed fields
//   - Kafka structured JSON messages
//   - MinIO typed objects with proper Content-Type
//
// Stage 2 (BLOB mode) remains unchanged -- all data as BYTEA/binary payload.
// =============================================================================

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "../config.hpp"

namespace dedup {

// ============================================================================
// Insertion Mode (CLI parameter --insertion-mode)
// ============================================================================

enum class InsertionMode {
    BLOB,    // Stage 2: All data as BYTEA in unified files schema (current)
    NATIVE,  // Stage 1: Structured data in DB-native schemas
    BOTH     // Run both modes sequentially
};

inline const char* insertion_mode_str(InsertionMode m) {
    switch (m) {
        case InsertionMode::BLOB:   return "blob";
        case InsertionMode::NATIVE: return "native";
        case InsertionMode::BOTH:   return "both";
    }
    return "??";
}

inline InsertionMode parse_insertion_mode(const std::string& s) {
    if (s == "blob")   return InsertionMode::BLOB;
    if (s == "native") return InsertionMode::NATIVE;
    if (s == "both")   return InsertionMode::BOTH;
    return InsertionMode::BLOB;  // backward compatible default
}

// ============================================================================
// Typed Column Value (variant-based, covers all SQL/NoSQL value types)
// ============================================================================

using ColumnValue = std::variant<
    std::monostate,          // NULL
    bool,                    // BOOLEAN
    int64_t,                 // INTEGER / BIGINT / SERIAL
    double,                  // DOUBLE PRECISION / FLOAT / DECIMAL
    std::string,             // TEXT / VARCHAR / CHAR / JSON / JSONB (as string)
    std::vector<char>        // BYTEA / BLOB / binary data
>;

// ============================================================================
// Native Record: A single row with named, typed columns
// ============================================================================

struct NativeRecord {
    std::map<std::string, ColumnValue> columns;

    // Convenience helpers for building records
    void set_null(const std::string& col) {
        columns[col] = std::monostate{};
    }
    void set_bool(const std::string& col, bool val) {
        columns[col] = val;
    }
    void set_int(const std::string& col, int64_t val) {
        columns[col] = val;
    }
    void set_double(const std::string& col, double val) {
        columns[col] = val;
    }
    void set_text(const std::string& col, const std::string& val) {
        columns[col] = val;
    }
    void set_text(const std::string& col, std::string&& val) {
        columns[col] = std::move(val);
    }
    void set_binary(const std::string& col, const std::vector<char>& val) {
        columns[col] = val;
    }
    void set_binary(const std::string& col, std::vector<char>&& val) {
        columns[col] = std::move(val);
    }

    // Estimate serialized size for throughput calculation
    size_t estimated_size_bytes() const {
        size_t total = 0;
        for (const auto& [key, val] : columns) {
            total += key.size();
            std::visit([&total](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    // NULL: 0 bytes
                } else if constexpr (std::is_same_v<T, bool>) {
                    total += 1;
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    total += 8;
                } else if constexpr (std::is_same_v<T, double>) {
                    total += 8;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    total += v.size();
                } else if constexpr (std::is_same_v<T, std::vector<char>>) {
                    total += v.size();
                }
            }, val);
        }
        return total;
    }
};

// ============================================================================
// Column Definition: Name + SQL type hint for schema creation
// ============================================================================

struct ColumnDef {
    std::string name;
    std::string type_hint;  // Generic SQL type: "TEXT", "BIGINT", "DOUBLE", "BYTEA", "JSONB", "BOOLEAN", "TIMESTAMPTZ"
    bool is_primary_key = false;
    bool is_not_null = false;
    std::string default_expr;  // e.g. "gen_random_uuid()", "now()", "0"
};

// ============================================================================
// Native Schema: Table definition for a specific PayloadType
// ============================================================================

struct NativeSchema {
    std::string table_name;
    std::vector<ColumnDef> columns;

    // Find primary key column (first one marked as PK)
    const ColumnDef* primary_key() const {
        for (const auto& col : columns) {
            if (col.is_primary_key) return &col;
        }
        return nullptr;
    }

    // Get column names as comma-separated string
    std::string column_list() const {
        std::string result;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) result += ", ";
            result += columns[i].name;
        }
        return result;
    }

    // Get parameter placeholders ($1, $2, ...) for PostgreSQL-style prepared statements
    std::string param_placeholders() const {
        std::string result;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) result += ", ";
            result += "$" + std::to_string(i + 1);
        }
        return result;
    }

    // Get ? placeholders for MySQL/MariaDB-style prepared statements
    std::string question_placeholders() const {
        std::string result;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) result += ", ";
            result += "?";
        }
        return result;
    }
};

// ============================================================================
// Schema Registry: Returns the native schema for a given PayloadType
//
// Binary types (NASA_IMAGE, BLENDER_VIDEO, RANDOM_BINARY) use the same
// schema as BLOB mode since they ARE binary data -- no structured alternative.
// ============================================================================

inline NativeSchema get_native_schema(PayloadType type) {
    switch (type) {
        case PayloadType::BANK_TRANSACTIONS:
            return {"bank_transactions", {
                {"id",          "SERIAL",       true,  true,  ""},
                {"amount",      "DOUBLE",       false, true,  ""},
                {"currency",    "CHAR(3)",      false, false, "'EUR'"},
                {"description", "TEXT",         false, false, ""},
                {"category",    "TEXT",         false, false, ""},
                {"timestamp",   "TIMESTAMPTZ",  false, false, "now()"},
            }};

        case PayloadType::TEXT_CORPUS:
            return {"posts", {
                {"id",             "SERIAL",   true,  true,  ""},
                {"article_id",     "BIGINT",   false, false, ""},
                {"user_id",        "BIGINT",   false, false, ""},
                {"headline",       "TEXT",     false, false, ""},
                {"body",           "TEXT",     false, true,  ""},
                {"positive_votes", "INT",      false, false, "0"},
                {"negative_votes", "INT",      false, false, "0"},
                {"created_at",     "TIMESTAMPTZ", false, false, "now()"},
            }};

        case PayloadType::NUMERIC_DATASET:
            return {"numbers", {
                {"id",  "SERIAL", true,  true, ""},
                {"f1",  "DOUBLE", false, true, ""},
                {"f2",  "DOUBLE", false, true, ""},
                {"f3",  "DOUBLE", false, true, ""},
                {"f4",  "DOUBLE", false, true, ""},
                {"f5",  "DOUBLE", false, true, ""},
                {"f6",  "DOUBLE", false, true, ""},
                {"f7",  "DOUBLE", false, true, ""},
                {"f8",  "DOUBLE", false, true, ""},
                {"f9",  "DOUBLE", false, true, ""},
                {"f10", "DOUBLE", false, true, ""},
            }};

        case PayloadType::GITHUB_EVENTS:
            return {"github_events", {
                {"id",         "TEXT",        true,  true, ""},
                {"type",       "TEXT",        false, true, ""},
                {"actor_login","TEXT",        false, false, ""},
                {"repo_name",  "TEXT",        false, false, ""},
                {"payload",    "JSONB",       false, false, ""},
                {"created_at", "TIMESTAMPTZ", false, false, "now()"},
            }};

        case PayloadType::STRUCTURED_JSON:
            return {"json_records", {
                {"id",    "SERIAL", true,  true,  ""},
                {"name",  "TEXT",   false, false, ""},
                {"email", "TEXT",   false, false, ""},
                {"data",  "JSONB",  false, true,  ""},
            }};

        case PayloadType::JSONB_DOCUMENTS:
            return {"jsonb_documents", {
                {"event_id", "TEXT",        true,  true,  ""},
                {"type",     "TEXT",        false, true,  ""},
                {"data",     "JSONB",       false, true,  ""},
                {"ts",       "TIMESTAMPTZ", false, false, "now()"},
            }};

        case PayloadType::TEXT_DOCUMENT:
            return {"text_documents", {
                {"id",      "SERIAL", true,  true,  ""},
                {"content", "TEXT",   false, true,  ""},
            }};

        case PayloadType::UUID_KEYS:
            return {"uuid_records", {
                {"uuid", "TEXT", true, true, ""},
            }};

        case PayloadType::GUTENBERG_TEXT:
            return {"gutenberg_texts", {
                {"id",      "SERIAL", true,  true,  ""},
                {"title",   "TEXT",   false, false, ""},
                {"content", "TEXT",   false, true,  ""},
            }};

        // Binary types: same schema as BLOB mode (no structured alternative)
        case PayloadType::NASA_IMAGE:
        case PayloadType::BLENDER_VIDEO:
        case PayloadType::RANDOM_BINARY:
            return {"binary_objects", {
                {"id",         "UUID",   true,  true, "gen_random_uuid()"},
                {"filename",   "TEXT",   false, true, ""},
                {"mime",       "TEXT",   false, true, ""},
                {"size_bytes", "BIGINT", false, true, ""},
                {"sha256",     "BYTEA",  false, true, ""},
                {"payload",    "BYTEA",  false, true, ""},
            }};

        case PayloadType::MIXED:
            // MIXED uses the generic files schema (same as BLOB mode)
            return {"files", {
                {"id",         "UUID",   true,  true, "gen_random_uuid()"},
                {"mime",       "TEXT",   false, true, ""},
                {"size_bytes", "BIGINT", false, true, ""},
                {"sha256",     "BYTEA",  false, true, ""},
                {"payload",    "BYTEA",  false, true, ""},
            }};
    }
    // Fallback
    return {"files", {
        {"id",         "UUID",   true,  true, "gen_random_uuid()"},
        {"mime",       "TEXT",   false, true, ""},
        {"size_bytes", "BIGINT", false, true, ""},
        {"sha256",     "BYTEA",  false, true, ""},
        {"payload",    "BYTEA",  false, true, ""},
    }};
}

} // namespace dedup
