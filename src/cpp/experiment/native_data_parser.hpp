#pragma once
// =============================================================================
// Native Data Parser -- Parses raw file data into typed NativeRecords
// TU Dresden Research Project "Deduplikation in Datenhaltungssystemen"
//
// Converts raw binary/text file contents into structured NativeRecord vectors
// for native insertion mode (Stage 1, doku.tex 5.4). Each PayloadType has
// a dedicated parser that extracts typed columns matching the NativeSchema.
//
// Parsing hierarchy:
//   NAS datasets:    CSV/JSON/binary files -> NativeRecords
//   Synthetic types: Generated data -> NativeRecords (re-parsed from blobs)
//   Binary types:    Raw binary -> single NativeRecord with BYTEA payload
// =============================================================================

#include <string>
#include <vector>

#include "../config.hpp"
#include "native_record.hpp"

namespace dedup {

class NativeDataParser {
public:
    // Parse a single raw file into one or more NativeRecords
    // Returns empty vector on parse failure
    static std::vector<NativeRecord> parse_file(
        const std::vector<char>& data,
        PayloadType type,
        const std::string& filename = "");

    // Parse all files in a directory for a given grade
    // Used by native experiment loop instead of raw file reading
    static std::vector<NativeRecord> parse_directory(
        const std::string& dir_path,
        PayloadType type,
        size_t max_files = 0);  // 0 = all files

    // Parse a single data entry by type (for synthetic data that's already in memory)
    static NativeRecord parse_single(
        const std::vector<char>& data,
        PayloadType type,
        const std::string& filename = "");

private:
    // Type-specific parsers (NAS datasets)
    static std::vector<NativeRecord> parse_bank_csv(const std::vector<char>& data);
    static std::vector<NativeRecord> parse_million_post_json(const std::vector<char>& data);
    static std::vector<NativeRecord> parse_numeric_csv(const std::vector<char>& data);
    static std::vector<NativeRecord> parse_github_events_json(const std::vector<char>& data);

    // Type-specific parsers (synthetic)
    static NativeRecord parse_structured_json(const std::vector<char>& data);
    static NativeRecord parse_jsonb_document(const std::vector<char>& data);
    static NativeRecord parse_text_document(const std::vector<char>& data);
    static std::vector<NativeRecord> parse_uuid_keys(const std::vector<char>& data);
    static NativeRecord parse_gutenberg_text(const std::vector<char>& data, const std::string& filename);

    // Binary types (same as BLOB mode, wrapped in NativeRecord)
    static NativeRecord parse_binary_blob(const std::vector<char>& data, const std::string& filename);

    // Helpers
    static std::vector<std::string> split_csv_line(const std::string& line, char delim = ',');
    static std::string trim(const std::string& s);
};

} // namespace dedup
