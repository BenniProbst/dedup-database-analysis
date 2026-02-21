#pragma once
// =============================================================================
// Experiment Checkpoint Manager -- TU Dresden Research Project
// "Deduplikation in Datenhaltungssystemen"
//
// Provides crash recovery by tracking per-system, per-run completion status.
// On failure: all runs for that DB type are invalidated and must be re-run.
//
// Checkpoint files are written as JSON to a directory (--checkpoint-dir).
// File naming: {system}_run{N}.checkpoint
// =============================================================================

#include <string>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <nlohmann/json.hpp>
#include "../utils/logger.hpp"

namespace dedup {

class Checkpoint {
public:
    explicit Checkpoint(const std::string& checkpoint_dir)
        : dir_(checkpoint_dir) {
        std::filesystem::create_directories(dir_);
        LOG_INF("[Checkpoint] Directory: %s", dir_.c_str());
    }

    // Check if a specific system+run is already completed
    bool is_complete(const std::string& system, int run_id) const {
        auto path = checkpoint_path(system, run_id);
        if (std::filesystem::exists(path)) {
            LOG_INF("[Checkpoint] Found %s run %d: COMPLETE", system.c_str(), run_id);
            return true;
        }
        return false;
    }

    // Mark a system+run as successfully completed
    void mark_complete(const std::string& system, int run_id,
                       int result_count = 0) {
        nlohmann::json j = {
            {"system", system},
            {"run_id", run_id},
            {"status", "complete"},
            {"result_count", result_count},
            {"timestamp", current_timestamp()}
        };

        auto path = checkpoint_path(system, run_id);
        std::ofstream f(path);
        if (f.is_open()) {
            f << j.dump(2);
            LOG_INF("[Checkpoint] Marked %s run %d as COMPLETE (%d results)",
                system.c_str(), run_id, result_count);
        } else {
            LOG_ERR("[Checkpoint] Failed to write %s", path.c_str());
        }
    }

    // Invalidate ALL runs for a system (used on crash recovery)
    void invalidate_system(const std::string& system) {
        LOG_WRN("[Checkpoint] Invalidating ALL checkpoints for %s", system.c_str());
        int removed = 0;
        for (int i = 1; i <= 100; ++i) {
            auto path = checkpoint_path(system, i);
            if (std::filesystem::exists(path)) {
                std::filesystem::remove(path);
                removed++;
            }
        }
        if (removed > 0) {
            LOG_INF("[Checkpoint] Removed %d checkpoint files for %s",
                removed, system.c_str());
        }
    }

    const std::string& dir() const { return dir_; }

private:
    std::string dir_;

    std::string checkpoint_path(const std::string& system, int run_id) const {
        return dir_ + "/" + system + "_run" + std::to_string(run_id) + ".checkpoint";
    }

    static std::string current_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return buf;
    }
};

} // namespace dedup
