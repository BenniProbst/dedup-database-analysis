#pragma once
// Generates test datasets with controlled duplication levels
// as defined in doku.tex Chapter 6 (Experimental Design):
//   U0  = unique (0% duplicates)
//   U50 = 50% duplicates
//   U90 = 90% duplicates
//
// Supports both synthetic and real-world payload types (§6.3):
//   Synthetic: random binary, structured JSON, text, UUID, JSONB
//   Real-world: NASA images, Blender video, Gutenberg text, GH Archive
//
// PayloadType enum is defined in config.hpp.
#include <cstdint>
#include <string>
#include <vector>
#include "../config.hpp"

namespace dedup {

struct DatasetConfig {
    size_t num_files = 100;           // Total files per grade
    size_t min_file_size = 4096;      // 4 KB minimum
    size_t max_file_size = 1048576;   // 1 MB maximum
    size_t fixed_file_size = 0;       // If >0, override min/max with fixed size
    uint64_t seed = 42;              // PRNG seed for reproducibility
    DataSourceConfig data_sources;   // URLs for real-world data caching
};

class DatasetGenerator {
public:
    explicit DatasetGenerator(const DatasetConfig& cfg = {}) : cfg_(cfg) {}

    // Generate datasets for all duplication grades into data_dir/{U0,U50,U90}/
    // Returns total number of files generated
    size_t generate_all(const std::string& data_dir, PayloadType type = PayloadType::MIXED);

    // Generate dataset for a single duplication grade
    size_t generate_grade(const std::string& output_dir, DupGrade grade,
                          PayloadType type = PayloadType::MIXED);

    // Get info about generated data
    [[nodiscard]] size_t total_bytes_written() const { return total_bytes_; }
    [[nodiscard]] size_t total_files_written() const { return total_files_; }

private:
    DatasetConfig cfg_;
    size_t total_bytes_ = 0;
    size_t total_files_ = 0;

    // PRNG state (xoshiro256**)
    uint64_t prng_state_[4]{};
    void seed_prng(uint64_t seed);
    uint64_t next_u64();
    size_t random_size();

    // Synthetic payload generators
    std::vector<char> generate_random_binary(size_t size);
    std::vector<char> generate_json(size_t approx_size);
    std::vector<char> generate_text(size_t approx_size);
    std::vector<char> generate_uuid_keys(size_t approx_size);
    std::vector<char> generate_jsonb_document(size_t approx_size);
    std::vector<char> generate_payload(size_t size, PayloadType type);

    // Real-world data source loaders (download + cache)
    std::vector<char> load_cached_or_download(const std::string& url, const std::string& cache_key);
    std::vector<char> load_nasa_image();
    std::vector<char> load_blender_video();
    std::vector<char> load_gutenberg_text();
    std::vector<char> load_github_events();

    // Write file to disk
    bool write_file(const std::string& path, const std::vector<char>& data);
};

} // namespace dedup
