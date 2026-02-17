#include "dataset_generator.hpp"
#include "../utils/logger.hpp"
#include "../utils/sha256.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace dedup {
namespace fs = std::filesystem;

// ---- xoshiro256** PRNG (fast, reproducible, period 2^256-1) ----

static uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void DatasetGenerator::seed_prng(uint64_t seed) {
    uint64_t s = seed;
    prng_state_[0] = splitmix64(s);
    prng_state_[1] = splitmix64(s);
    prng_state_[2] = splitmix64(s);
    prng_state_[3] = splitmix64(s);
}

static uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

uint64_t DatasetGenerator::next_u64() {
    const uint64_t result = rotl64(prng_state_[1] * 5, 7) * 9;
    const uint64_t t = prng_state_[1] << 17;
    prng_state_[2] ^= prng_state_[0];
    prng_state_[3] ^= prng_state_[1];
    prng_state_[1] ^= prng_state_[2];
    prng_state_[0] ^= prng_state_[3];
    prng_state_[2] ^= t;
    prng_state_[3] = rotl64(prng_state_[3], 45);
    return result;
}

size_t DatasetGenerator::random_size() {
    if (cfg_.fixed_file_size > 0) return cfg_.fixed_file_size;
    size_t range = cfg_.max_file_size - cfg_.min_file_size;
    return cfg_.min_file_size + (next_u64() % (range + 1));
}

// ---- Payload generators ----

std::vector<char> DatasetGenerator::generate_random_binary(size_t size) {
    std::vector<char> buf(size);
    // Fill 8 bytes at a time
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t r = next_u64();
        std::memcpy(buf.data() + i, &r, 8);
    }
    if (i < size) {
        uint64_t r = next_u64();
        std::memcpy(buf.data() + i, &r, size - i);
    }
    return buf;
}

std::vector<char> DatasetGenerator::generate_json(size_t approx_size) {
    // Generate realistic JSON payloads with known fields
    static const char* names[] = {
        "Alice", "Bob", "Charlie", "Diana", "Eve", "Frank",
        "Grace", "Hank", "Iris", "Jack", "Karen", "Leo"
    };
    static const char* cities[] = {
        "Dresden", "Berlin", "Munich", "Hamburg", "Leipzig",
        "Potsdam", "Frankfurt", "Stuttgart", "Cologne", "Bonn"
    };
    static const char* departments[] = {
        "Engineering", "Research", "Marketing", "Sales",
        "Operations", "Finance", "Legal", "HR"
    };

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"id\": " << next_u64() << ",\n";
    oss << "  \"name\": \"" << names[next_u64() % 12] << "\",\n";
    oss << "  \"email\": \"user" << (next_u64() % 10000) << "@comdare.de\",\n";
    oss << "  \"city\": \"" << cities[next_u64() % 10] << "\",\n";
    oss << "  \"department\": \"" << departments[next_u64() % 8] << "\",\n";
    oss << "  \"salary\": " << (30000 + next_u64() % 70000) << ",\n";
    oss << "  \"active\": " << (next_u64() % 2 ? "true" : "false") << ",\n";

    // Add padding data to reach target size
    oss << "  \"metadata\": {\n";
    oss << "    \"tags\": [";
    size_t current = oss.str().size();
    while (current < approx_size - 50) {
        if (current > oss.str().size() - 5)  // not first
            oss << ", ";
        oss << "\"tag_" << (next_u64() % 1000) << "\"";
        current = oss.str().size();
    }
    oss << "],\n";
    oss << "    \"version\": " << (next_u64() % 100) << "\n";
    oss << "  }\n";
    oss << "}\n";

    std::string s = oss.str();
    return {s.begin(), s.end()};
}

std::vector<char> DatasetGenerator::generate_text(size_t approx_size) {
    // Generate pseudo-natural text with word patterns (highly compressible)
    static const char* words[] = {
        "the", "deduplication", "storage", "system", "data", "block",
        "hash", "fingerprint", "chunk", "file", "index", "cache",
        "inline", "backup", "compression", "redundant", "unique",
        "database", "cluster", "node", "replica", "volume", "metric",
        "experiment", "measurement", "throughput", "latency", "insert",
        "delete", "maintenance", "vacuum", "compaction", "retention",
        "kafka", "minio", "postgresql", "redis", "cockroachdb",
        "longhorn", "kubernetes", "prometheus", "grafana",
        "of", "in", "and", "for", "with", "is", "are", "was",
        "that", "this", "from", "by", "on", "at", "to", "an"
    };
    constexpr size_t nwords = sizeof(words) / sizeof(words[0]);

    std::string text;
    text.reserve(approx_size + 100);
    size_t sentence_len = 0;

    while (text.size() < approx_size) {
        if (sentence_len == 0 && !text.empty()) {
            text += "\n\n";
        }

        const char* w = words[next_u64() % nwords];
        if (sentence_len == 0) {
            // Capitalize first letter
            text += static_cast<char>(std::toupper(w[0]));
            text += (w + 1);
        } else {
            text += w;
        }

        sentence_len++;
        if (sentence_len >= 8 + (next_u64() % 12)) {
            text += ". ";
            sentence_len = 0;
        } else {
            text += ' ';
        }
    }

    return {text.begin(), text.end()};
}

std::vector<char> DatasetGenerator::generate_payload(size_t size, PayloadType type) {
    switch (type) {
        case PayloadType::RANDOM_BINARY:
            return generate_random_binary(size);
        case PayloadType::STRUCTURED_JSON:
            return generate_json(size);
        case PayloadType::TEXT_DOCUMENT:
            return generate_text(size);
        case PayloadType::MIXED: {
            uint64_t choice = next_u64() % 3;
            if (choice == 0) return generate_random_binary(size);
            if (choice == 1) return generate_json(size);
            return generate_text(size);
        }
    }
    return generate_random_binary(size);
}

bool DatasetGenerator::write_file(const std::string& path, const std::vector<char>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        LOG_ERR("[datagen] Cannot write: %s", path.c_str());
        return false;
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return f.good();
}

// ---- Public API ----

size_t DatasetGenerator::generate_all(const std::string& data_dir, PayloadType type) {
    LOG_INF("[datagen] Generating datasets in %s (seed=%llu, %zu files/grade)",
        data_dir.c_str(), static_cast<unsigned long long>(cfg_.seed), cfg_.num_files);

    size_t total = 0;
    total += generate_grade(data_dir + "/U0", DupGrade::U0, type);
    total += generate_grade(data_dir + "/U50", DupGrade::U50, type);
    total += generate_grade(data_dir + "/U90", DupGrade::U90, type);

    LOG_INF("[datagen] Complete: %zu files, %zu bytes total", total_files_, total_bytes_);
    return total;
}

size_t DatasetGenerator::generate_grade(const std::string& output_dir, DupGrade grade,
                                         PayloadType type) {
    fs::create_directories(output_dir);

    // Determine duplication ratio
    double dup_ratio = 0.0;
    switch (grade) {
        case DupGrade::U0:  dup_ratio = 0.0; break;
        case DupGrade::U50: dup_ratio = 0.5; break;
        case DupGrade::U90: dup_ratio = 0.9; break;
    }

    size_t num_unique = static_cast<size_t>(cfg_.num_files * (1.0 - dup_ratio));
    if (num_unique == 0) num_unique = 1;  // At least one unique file
    size_t num_dups = cfg_.num_files - num_unique;

    LOG_INF("[datagen] Grade %s: %zu unique + %zu duplicates = %zu files",
        dup_grade_str(grade), num_unique, num_dups, cfg_.num_files);

    // Seed PRNG deterministically per grade
    seed_prng(cfg_.seed + static_cast<uint64_t>(grade));

    // Generate unique files
    std::vector<std::vector<char>> unique_payloads;
    unique_payloads.reserve(num_unique);
    size_t files_written = 0;

    for (size_t i = 0; i < num_unique; ++i) {
        size_t fsize = random_size();
        auto payload = generate_payload(fsize, type);
        std::string sha = SHA256::hash_hex(payload.data(), payload.size());

        std::string filename = output_dir + "/file_" +
            std::to_string(i) + "_" + sha.substr(0, 8) + ".dat";

        if (write_file(filename, payload)) {
            total_bytes_ += payload.size();
            total_files_++;
            files_written++;
        }

        unique_payloads.push_back(std::move(payload));
    }

    // Generate duplicates by copying random unique payloads
    for (size_t i = 0; i < num_dups; ++i) {
        size_t src_idx = next_u64() % unique_payloads.size();
        const auto& payload = unique_payloads[src_idx];
        std::string sha = SHA256::hash_hex(payload.data(), payload.size());

        // Different filename, identical content (= a duplicate)
        std::string filename = output_dir + "/file_" +
            std::to_string(num_unique + i) + "_dup_" + sha.substr(0, 8) + ".dat";

        if (write_file(filename, payload)) {
            total_bytes_ += payload.size();
            total_files_++;
            files_written++;
        }
    }

    LOG_INF("[datagen] %s: wrote %zu files to %s",
        dup_grade_str(grade), files_written, output_dir.c_str());
    return files_written;
}

} // namespace dedup
