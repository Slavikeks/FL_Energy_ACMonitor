#pragma once
#include "fl/core/Types.h"
#include "fl/hal/Rs485Transport.h"

namespace fl {
struct LogEvent {
    Severity severity{Severity::Info};
    MonotonicMs uptime_ms{0};
    std::optional<EpochSeconds> epoch{};
    Text<160> message{}; // Must never contain credentials, cookies, raw config.
};
class IEventLog {
public:
    virtual ~IEventLog() = default;
    virtual void append(const LogEvent&) = 0;
    virtual std::size_t size() const = 0;
    virtual bool get(std::size_t oldest_first_index, LogEvent&) const = 0;
}; // RAM ring of 200 events implemented in stage 1.

class EventLog final : public IEventLog {
public:
    explicit EventLog(const IClock* clock = nullptr) : clock_(clock) {}
    void append(const LogEvent& event) override {
        LogEvent copy = event;
        if (clock_) {
            const auto now = clock_->now();
            copy.uptime_ms = now.uptime_ms;
            copy.epoch = now.epoch;
        }
        entries_[next_] = copy;
        next_ = (next_ + 1) % kLogCapacity;
        if (count_ < kLogCapacity) ++count_;
    }
    std::size_t size() const override { return count_; }
    bool get(std::size_t oldest_first_index, LogEvent& out) const override {
        if (oldest_first_index >= count_) return false;
        const auto oldest = (next_ + kLogCapacity - count_) % kLogCapacity;
        out = entries_[(oldest + oldest_first_index) % kLogCapacity];
        return true;
    }
private:
    std::array<LogEvent, kLogCapacity> entries_{};
    const IClock* clock_{nullptr};
    std::size_t next_{0};
    std::size_t count_{0};
};
} // namespace fl
