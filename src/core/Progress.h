#pragma once

#include <atomic>
#include <string_view>

namespace opencdc {

/// Receives progress updates from a running analysis. Implementations must be
/// thread-safe: the engine may call them from a worker thread.
class ProgressSink {
public:
    ProgressSink() = default;
    ProgressSink(const ProgressSink&) = delete;
    ProgressSink& operator=(const ProgressSink&) = delete;
    virtual ~ProgressSink() = default;

    /// @param stage    short human-readable stage name
    /// @param fraction overall progress in [0, 1]
    virtual void update(std::string_view stage, double fraction) = 0;
};

class NullProgressSink final : public ProgressSink {
public:
    void update(std::string_view, double) override {}
};

/// Cooperative cancellation flag shared between the caller and the engine.
class CancellationToken {
public:
    CancellationToken() = default;
    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;

    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> cancelled_{false};
};

} // namespace opencdc
