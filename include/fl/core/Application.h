#pragma once
#include "fl/core/StateStore.h"
#include "fl/core/Config.h"
#include "fl/core/EventLog.h"
#include <cstddef>

namespace fl {
class Application {
public:
    ~Application();
    void begin();
    void tick(MonotonicMs uptime_ms);
    const IStateView& state() const { return state_; }
    const Config& config() const { return config_; }
    const IEventLog& log() const { return log_; }
private:
    class StateClock final : public IClock {
    public:
        explicit StateClock(const StateStore& state) : state_(state) {}
        ClockState now() const override { return state_.read().clock; }
    private:
        const StateStore& state_;
    };
    Config config_{};
    StateStore state_{};
    StateClock clock_{state_};
    EventLog log_{&clock_};
    // Service implementations are composed at namespace scope in Application.cpp.
    struct ServiceStorage;
    static constexpr std::size_t kServiceStorageMaxBytes = 24576;
    ServiceStorage* service_{nullptr};
};
} // namespace fl
