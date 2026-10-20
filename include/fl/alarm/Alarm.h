#pragma once
#include "fl/core/Types.h"

namespace fl {
struct Alarm {
    AlarmType type{AlarmType::AcOffline};
    AlarmState state{AlarmState::Problem};
    Severity severity{Severity::Warning};
    std::optional<Address> address{};
    std::optional<GroupId> group_id{};
    MonotonicMs changed_ms{0};
    std::optional<EpochSeconds> epoch{};
    std::uint32_t code{0};
};
struct AlarmEvent {
    Alarm alarm{};
    Name ac_name{};
    Name group_name{};
    Text<160> text{};
};
struct RuntimeState;
class IAlarmManager {
public:
    virtual ~IAlarmManager() = default;
    virtual void tick(RuntimeState&, const ClockState&) = 0;
    // Debounced/deduplicated events only; names resolved from typed state.
    virtual bool nextEvent(AlarmEvent&) = 0;
};
} // namespace fl
