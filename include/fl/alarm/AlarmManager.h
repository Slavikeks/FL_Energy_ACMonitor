#pragma once
#include "fl/alarm/Alarm.h"
#include "fl/core/Config.h"
#include "fl/core/EventLog.h"
#include "fl/core/StateStore.h"
#include <array>

namespace fl {
class AlarmManager final : public IAlarmManager {
public:
    explicit AlarmManager(EventLog* event_log = nullptr) : event_log_(event_log) {}
    void tick(RuntimeState&, const ClockState&) override;
    void tick(RuntimeState&, const ClockState&, const Config&);
    bool nextEvent(AlarmEvent&) override;
private:
    static constexpr std::size_t kTracked = 2 * kMaxAc + 2 * kMaxGroups;
    // Every tracked condition can transition in the same tick. Keep all of
    // them until Application drains the queue, including the maximum burst.
    static constexpr std::size_t kQueue = kMaxAlarms;
    struct Track {
        bool used{false}, seen{false}, active{false}; AlarmType type{AlarmType::AcOffline};
        std::optional<Address> address{}; std::optional<GroupId> group{};
        MonotonicMs candidate_since_ms{0}; bool candidate{false}; std::uint32_t code{0};
    };
    struct QueuedEvent {
        Alarm alarm{};
        Name object_name{};
    };
    std::array<Track, kTracked> tracked_{};
    std::array<QueuedEvent, kQueue> queue_{};
    std::size_t head_{0}, count_{0};
    EventLog* event_log_{nullptr};
    void process(RuntimeState&, const ClockState&, const Config*);
};
} // namespace fl
