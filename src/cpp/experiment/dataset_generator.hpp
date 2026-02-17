#pragma once
// Generates synthetic test datasets with controlled duplication levels
// as defined in doku.tex Chapter 5 (Experimental Design):
//   U0  = unique (0% duplicates)
//   U50 = 50% duplicates
//   U90 = 90% duplicates
//
// Payload types:
//   - random binary (simulates images/video)
//   - structured JSON (simulates application data)
//   - text (simulates documents)
//   - UUID keys (high-entropy identifiers)
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
};

// Payload type for generated files
enum class PayloadType {
    RANDOM_BINARY,    // Pure random bytes (incompressible, like encrypted data)
    STRUCTURED_JSON,  // JSON with known fields (compressible, semi-structured)
    TEXT_DOCUMENT,    // ASCII text with word patterns (highly compressible)
    MIXED             // Random mix of above types
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

    // Payload generators
    std::vector<char> generate_random_binary(size_t size);
    std::vector<char> generate_json(size_t approx_size);
    std::vector<char> generate_text(size_t approx_size);
    std::vector<char> generate_payload(size_t size, PayloadType type);

    // Write file to disk
    bool write_file(const std::string& path, const std::vector<char>& data);
};

} // namespace dedup
