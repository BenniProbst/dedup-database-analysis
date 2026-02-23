#include "native_data_parser.hpp"
#include "../utils/logger.hpp"
#include "../utils/sha256.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <nlohmann/json.hpp>

namespace dedup {
namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

std::string NativeDataParser::trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n\"");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n\"");
    return s.substr(start, end - start + 1);
}

std::vector<std::string> NativeDataParser::split_csv_line(const std::string& line, char delim) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                field += '"';
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == delim && !in_quotes) {
            fields.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    fields.push_back(field);
    return fields;
}

// ============================================================================
// Bank Transactions CSV Parser
// Expects CSV with header: amount,currency,description,category,timestamp
// Or bankdataset binary chunks -> treat each chunk as one binary record
// ============================================================================

std::vector<NativeRecord> NativeDataParser::parse_bank_csv(const std::vector<char>& data) {
    std::vector<NativeRecord> records;
    std::string content(data.begin(), data.end());
    std::istringstream iss(content);
    std::string line;

    // Try to detect if this is CSV (has comma-separated header)
    if (!std::getline(iss, line)) return records;

    // Check if first line looks like a CSV header
    auto header_fields = split_csv_line(line);
    bool has_header = false;
    for (const auto& f : header_fields) {
        std::string lower = trim(f);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "amount" || lower == "currency" || lower == "description" ||
            lower == "category" || lower == "betrag" || lower == "waehrung") {
            has_header = true;
            break;
        }
    }

    if (!has_header) {
        // Binary chunk: treat as single record with raw payload
        NativeRecord rec;
        rec.set_double("amount", 0.0);
        rec.set_text("currency", "EUR");
        rec.set_text("description", "binary_chunk");
        rec.set_text("category", "import");
        records.push_back(std::move(rec));
        return records;
    }

    // Map header columns to indices
    std::map<std::string, size_t> col_idx;
    for (size_t i = 0; i < header_fields.size(); ++i) {
        std::string lower = trim(header_fields[i]);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        col_idx[lower] = i;
    }

    // Parse data rows
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto fields = split_csv_line(line);

        NativeRecord rec;
        auto get_field = [&](const std::string& name) -> std::string {
            auto it = col_idx.find(name);
            if (it != col_idx.end() && it->second < fields.size())
                return trim(fields[it->second]);
            return "";
        };

        // Try various column name variants
        std::string amount_str = get_field("amount");
        if (amount_str.empty()) amount_str = get_field("betrag");
        try {
            rec.set_double("amount", amount_str.empty() ? 0.0 : std::stod(amount_str));
        } catch (...) {
            rec.set_double("amount", 0.0);
        }

        std::string currency = get_field("currency");
        if (currency.empty()) currency = get_field("waehrung");
        if (currency.empty()) currency = "EUR";
        rec.set_text("currency", currency);

        std::string desc = get_field("description");
        if (desc.empty()) desc = get_field("beschreibung");
        rec.set_text("description", desc);

        std::string cat = get_field("category");
        if (cat.empty()) cat = get_field("kategorie");
        rec.set_text("category", cat);

        records.push_back(std::move(rec));
    }

    return records;
}

// ============================================================================
// Million Post JSON Parser
// Expects JSON array: [{"ID_Post": N, "Headline": "...", "Body": "...", ...}]
// ============================================================================

std::vector<NativeRecord> NativeDataParser::parse_million_post_json(const std::vector<char>& data) {
    std::vector<NativeRecord> records;

    try {
        auto j = nlohmann::json::parse(data.begin(), data.end());

        if (j.is_array()) {
            for (const auto& post : j) {
                NativeRecord rec;

                if (post.contains("ID_Post"))
                    rec.set_int("article_id", post["ID_Post"].get<int64_t>());
                else if (post.contains("id"))
                    rec.set_int("article_id", post["id"].get<int64_t>());

                if (post.contains("ID_User"))
                    rec.set_int("user_id", post["ID_User"].get<int64_t>());

                if (post.contains("Headline"))
                    rec.set_text("headline", post["Headline"].get<std::string>());
                else if (post.contains("headline"))
                    rec.set_text("headline", post["headline"].get<std::string>());

                if (post.contains("Body"))
                    rec.set_text("body", post["Body"].get<std::string>());
                else if (post.contains("body"))
                    rec.set_text("body", post["body"].get<std::string>());

                if (post.contains("Positive_Votes"))
                    rec.set_int("positive_votes", post["Positive_Votes"].get<int64_t>());
                if (post.contains("Negative_Votes"))
                    rec.set_int("negative_votes", post["Negative_Votes"].get<int64_t>());

                records.push_back(std::move(rec));
            }
        }
    } catch (const nlohmann::json::exception& e) {
        LOG_ERR("[native_parser] JSON parse error (million_post): %s", e.what());
    }

    return records;
}

// ============================================================================
// Numeric CSV Parser
// Expects CSV with header: f1,f2,...,f10 (or no header, just 10 double columns)
// ============================================================================

std::vector<NativeRecord> NativeDataParser::parse_numeric_csv(const std::vector<char>& data) {
    std::vector<NativeRecord> records;
    std::string content(data.begin(), data.end());
    std::istringstream iss(content);
    std::string line;

    // Read first line to detect header
    if (!std::getline(iss, line)) return records;

    auto first_fields = split_csv_line(line);
    bool has_header = false;

    // Check if first field is non-numeric (header)
    if (!first_fields.empty()) {
        std::string f = trim(first_fields[0]);
        try {
            std::stod(f);
        } catch (...) {
            has_header = true;
        }
    }

    // If no header, parse first line as data
    auto parse_row = [](const std::vector<std::string>& fields) -> NativeRecord {
        NativeRecord rec;
        for (size_t i = 0; i < fields.size() && i < 10; ++i) {
            std::string col = "f" + std::to_string(i + 1);
            try {
                rec.set_double(col, std::stod(trim(fields[i])));
            } catch (...) {
                rec.set_double(col, 0.0);
            }
        }
        return rec;
    };

    if (!has_header) {
        records.push_back(parse_row(first_fields));
    }

    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        auto fields = split_csv_line(line);
        records.push_back(parse_row(fields));
    }

    return records;
}

// ============================================================================
// GitHub Events JSON Lines Parser
// Each line is a JSON object: {"id":"...","type":"...","actor":{...},"repo":{...},"payload":{...}}
// ============================================================================

std::vector<NativeRecord> NativeDataParser::parse_github_events_json(const std::vector<char>& data) {
    std::vector<NativeRecord> records;
    std::string content(data.begin(), data.end());
    std::istringstream iss(content);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.empty()) continue;

        try {
            auto j = nlohmann::json::parse(line);
            NativeRecord rec;

            rec.set_text("id", j.value("id", ""));
            rec.set_text("type", j.value("type", ""));

            if (j.contains("actor") && j["actor"].is_object())
                rec.set_text("actor_login", j["actor"].value("login", ""));
            else
                rec.set_text("actor_login", "");

            if (j.contains("repo") && j["repo"].is_object())
                rec.set_text("repo_name", j["repo"].value("name", ""));
            else
                rec.set_text("repo_name", "");

            // Store payload as JSON string (for JSONB insertion)
            if (j.contains("payload"))
                rec.set_text("payload", j["payload"].dump());
            else
                rec.set_text("payload", "{}");

            records.push_back(std::move(rec));
        } catch (const nlohmann::json::exception& e) {
            // Skip malformed lines
            continue;
        }
    }

    return records;
}

// ============================================================================
// Structured JSON Parser (synthetic)
// ============================================================================

NativeRecord NativeDataParser::parse_structured_json(const std::vector<char>& data) {
    NativeRecord rec;

    try {
        auto j = nlohmann::json::parse(data.begin(), data.end());
        rec.set_text("name", j.value("name", ""));
        rec.set_text("email", j.value("email", ""));
        rec.set_text("data", j.dump());
    } catch (...) {
        rec.set_text("name", "");
        rec.set_text("email", "");
        rec.set_text("data", std::string(data.begin(), data.end()));
    }

    return rec;
}

// ============================================================================
// JSONB Document Parser (synthetic)
// ============================================================================

NativeRecord NativeDataParser::parse_jsonb_document(const std::vector<char>& data) {
    NativeRecord rec;

    try {
        auto j = nlohmann::json::parse(data.begin(), data.end());
        rec.set_text("event_id", j.value("event_id", ""));
        rec.set_text("type", j.value("type", ""));
        rec.set_text("data", j.dump());
    } catch (...) {
        rec.set_text("event_id", "unknown");
        rec.set_text("type", "unknown");
        rec.set_text("data", std::string(data.begin(), data.end()));
    }

    return rec;
}

// ============================================================================
// Text Document Parser (synthetic)
// ============================================================================

NativeRecord NativeDataParser::parse_text_document(const std::vector<char>& data) {
    NativeRecord rec;
    rec.set_text("content", std::string(data.begin(), data.end()));
    return rec;
}

// ============================================================================
// UUID Keys Parser (synthetic -- one UUID per line)
// ============================================================================

std::vector<NativeRecord> NativeDataParser::parse_uuid_keys(const std::vector<char>& data) {
    std::vector<NativeRecord> records;
    std::string content(data.begin(), data.end());
    std::istringstream iss(content);
    std::string line;

    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty()) continue;
        NativeRecord rec;
        rec.set_text("uuid", line);
        records.push_back(std::move(rec));
    }

    return records;
}

// ============================================================================
// Gutenberg Text Parser
// ============================================================================

NativeRecord NativeDataParser::parse_gutenberg_text(const std::vector<char>& data,
                                                      const std::string& filename) {
    NativeRecord rec;
    std::string content(data.begin(), data.end());

    // Extract title from filename or first line
    std::string title = filename;
    if (title.empty()) {
        auto nl = content.find('\n');
        if (nl != std::string::npos)
            title = trim(content.substr(0, nl));
    }

    rec.set_text("title", title);
    rec.set_text("content", std::move(content));
    return rec;
}

// ============================================================================
// Binary Blob Parser (NASA, Blender, Random Binary)
// ============================================================================

NativeRecord NativeDataParser::parse_binary_blob(const std::vector<char>& data,
                                                   const std::string& filename) {
    NativeRecord rec;

    // Determine MIME type from filename
    std::string mime = "application/octet-stream";
    if (filename.size() >= 4) {
        std::string ext = filename.substr(filename.rfind('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".tif" || ext == ".tiff") mime = "image/tiff";
        else if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
        else if (ext == ".png") mime = "image/png";
        else if (ext == ".mp4") mime = "video/mp4";
        else if (ext == ".mkv") mime = "video/x-matroska";
        else if (ext == ".mov") mime = "video/quicktime";
        else if (ext == ".dat") mime = "application/octet-stream";
    }

    rec.set_text("filename", filename);
    rec.set_text("mime", mime);
    rec.set_int("size_bytes", static_cast<int64_t>(data.size()));

    // SHA256 hash
    std::string sha_hex = SHA256::hash_hex(data.data(), data.size());
    std::vector<char> sha_bytes(sha_hex.begin(), sha_hex.end());
    rec.set_binary("sha256", std::move(sha_bytes));

    rec.set_binary("payload", data);
    return rec;
}

// ============================================================================
// Public API: parse_file
// ============================================================================

std::vector<NativeRecord> NativeDataParser::parse_file(
    const std::vector<char>& data,
    PayloadType type,
    const std::string& filename) {

    if (data.empty()) return {};

    switch (type) {
        case PayloadType::BANK_TRANSACTIONS:
            return parse_bank_csv(data);

        case PayloadType::TEXT_CORPUS:
            return parse_million_post_json(data);

        case PayloadType::NUMERIC_DATASET:
            return parse_numeric_csv(data);

        case PayloadType::GITHUB_EVENTS:
            return parse_github_events_json(data);

        case PayloadType::UUID_KEYS:
            return parse_uuid_keys(data);

        case PayloadType::STRUCTURED_JSON:
            return {parse_structured_json(data)};

        case PayloadType::JSONB_DOCUMENTS:
            return {parse_jsonb_document(data)};

        case PayloadType::TEXT_DOCUMENT:
            return {parse_text_document(data)};

        case PayloadType::GUTENBERG_TEXT:
            return {parse_gutenberg_text(data, filename)};

        case PayloadType::NASA_IMAGE:
        case PayloadType::BLENDER_VIDEO:
        case PayloadType::RANDOM_BINARY:
            return {parse_binary_blob(data, filename)};

        case PayloadType::MIXED:
            // For MIXED, treat as binary blob
            return {parse_binary_blob(data, filename)};
    }

    return {parse_binary_blob(data, filename)};
}

// ============================================================================
// Public API: parse_single (convenience for single-record types)
// ============================================================================

NativeRecord NativeDataParser::parse_single(
    const std::vector<char>& data,
    PayloadType type,
    const std::string& filename) {

    auto records = parse_file(data, type, filename);
    if (records.empty()) {
        NativeRecord empty;
        return empty;
    }
    return std::move(records[0]);
}

// ============================================================================
// Public API: parse_directory
// ============================================================================

std::vector<NativeRecord> NativeDataParser::parse_directory(
    const std::string& dir_path,
    PayloadType type,
    size_t max_files) {

    std::vector<NativeRecord> all_records;

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        LOG_ERR("[native_parser] Directory not found: %s", dir_path.c_str());
        return all_records;
    }

    // Collect and sort files for deterministic ordering
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (entry.is_regular_file() && entry.file_size() > 0) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    if (max_files > 0 && files.size() > max_files) {
        files.resize(max_files);
    }

    LOG_INF("[native_parser] Parsing %zu files from %s (type: %s)",
        files.size(), dir_path.c_str(), payload_type_str(type));

    size_t total_records = 0;
    for (const auto& fpath : files) {
        std::ifstream f(fpath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;

        auto size = f.tellg();
        f.seekg(0);
        std::vector<char> data(static_cast<size_t>(size));
        f.read(data.data(), size);

        auto records = parse_file(data, type, fpath.filename().string());
        total_records += records.size();
        all_records.insert(all_records.end(),
            std::make_move_iterator(records.begin()),
            std::make_move_iterator(records.end()));
    }

    LOG_INF("[native_parser] Parsed %zu records from %zu files", total_records, files.size());
    return all_records;
}

} // namespace dedup
