#pragma once
// Precision timer for N97 (Alder Lake-N) -- steady_clock for deterministic measurements
#include <chrono>
#include <cstdint>

namespace dedup {

class Timer {
public:
    void start() noexcept { start_ = std::chrono::steady_clock::now(); }

    void stop() noexcept { end_ = std::chrono::steady_clock::now(); }

    [[nodiscard]] int64_t elapsed_ns() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end_ - start_).count();
    }

    [[nodiscard]] int64_t elapsed_us() const noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(end_ - start_).count();
    }

    [[nodiscard]] int64_t elapsed_ms() const noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(end_ - start_).count();
    }

    [[nodiscard]] double elapsed_sec() const noexcept {
        return std::chrono::duration<double>(end_ - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point end_{};
};

// RAII scoped timer -- records elapsed time into a reference on destruction
class ScopedTimer {
public:
    explicit ScopedTimer(int64_t& out_ns) noexcept
        : out_ns_(out_ns), start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() noexcept {
        auto end = std::chrono::steady_clock::now();
        out_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    int64_t& out_ns_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace dedup
