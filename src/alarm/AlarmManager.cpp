#include "fl/alarm/AlarmManager.h"
#include <cstdio>

namespace fl {
namespace {
constexpr MonotonicMs kDebounceMs = 10000;
Severity alarmSeverity(AlarmType type) {
    return type == AlarmType::AcFault || type == AlarmType::GroupOverTemperature ? Severity::Critical : Severity::Warning;
}
const char* alarmText(AlarmType type, bool recovery) {
    switch (type) {
    case AlarmType::AcOffline: return recovery ? "Кондиционер снова отвечает" : "Кондиционер не отвечает";
    case AlarmType::AcFault: return recovery ? "Ошибка кондиционера снята" : "Обнаружен код ошибки кондиционера";
    case AlarmType::GroupDegraded: return recovery ? "Группа восстановила требуемую ёмкость" : "Группа не может обеспечить требуемую ёмкость";
    case AlarmType::GroupOverTemperature: return recovery ? "Температура группы вернулась в допустимый диапазон" : "Температура группы выше аварийного порога";
    }
    return "Событие системы";
}
}

namespace {
const char* alarmTypeName(AlarmType type) {
    switch (type) {
    case AlarmType::AcOffline: return "AC offline";
    case AlarmType::AcFault: return "AC fault";
    case AlarmType::GroupDegraded: return "group degraded";
    case AlarmType::GroupOverTemperature: return "group overtemperature";
    }
    return "system alarm";
}
void appendAlarmLog(EventLog* sink, const AlarmEvent& event) {
    if (!sink) return;
    LogEvent entry;
    entry.severity = event.alarm.state == AlarmState::Problem ? event.alarm.severity : Severity::Info;
    entry.uptime_ms = event.alarm.changed_ms;
    entry.epoch = event.alarm.epoch;
    char message[160]{};
    const char* state = event.alarm.state == AlarmState::Problem ? "PROBLEM" : "RECOVERY";
    const char* type = alarmTypeName(event.alarm.type);
    if (event.alarm.address) {
        const auto name = event.ac_name.view();
        std::snprintf(message, sizeof(message), "ALARM %s: %s AC %.*s (address %u)", state, type,
            static_cast<int>(name.size()), name.data(), static_cast<unsigned>(*event.alarm.address));
    } else if (event.alarm.group_id) {
        const auto name = event.group_name.view();
        std::snprintf(message, sizeof(message), "ALARM %s: %s group %.*s (id %u)", state, type,
            static_cast<int>(name.size()), name.data(), static_cast<unsigned>(*event.alarm.group_id));
    } else {
        std::snprintf(message, sizeof(message), "ALARM %s: %s", state, type);
    }
    entry.message.assign(message);
    sink->append(entry);
}
}
void AlarmManager::tick(RuntimeState& state, const ClockState& clock) { process(state, clock, nullptr); }
void AlarmManager::tick(RuntimeState& state, const ClockState& clock, const Config& config) { process(state, clock, &config); }
void AlarmManager::process(RuntimeState& state, const ClockState& clock, const Config* config) {
    struct Observation { AlarmType type; std::optional<Address> address; std::optional<GroupId> group; bool problem; std::uint32_t code; };
    std::array<Observation, kTracked> observations{};
    std::size_t count = 0;
    for (const auto& ac : state.conditioners) {
        observations[count++] = {AlarmType::AcOffline, ac.address, {}, ac.availability == Availability::Offline, 0};
        const bool fault = ac.observed.error_flags.confirmed() && *ac.observed.error_flags.value != 0;
        observations[count++] = {AlarmType::AcFault, ac.address, {}, fault,
            ac.observed.error_flags.value ? static_cast<std::uint32_t>(*ac.observed.error_flags.value) : 0U};
    }
    for (const auto& group : state.groups) {
        const bool degraded = config ?
            (config->automatic_control_enabled && !state.observe_only && group.mode == GroupMode::Auto &&
             group.active_addresses.count() < group.required_active) :
            group.state == GroupState::Degraded;
        observations[count++] = {AlarmType::GroupDegraded, {}, group.id, degraded, 0};
        const GroupConfig* saved = nullptr;
        if (config) for (const auto& candidate : config->groups) if (candidate.id == group.id) { saved = &candidate; break; }
        bool over = group.state == GroupState::Emergency && group.temperature_deci_c.confirmed();
        if (config) {
            const bool monitored = saved && saved->mode == GroupMode::Auto &&
                config->automatic_control_enabled && !state.observe_only;
            const bool recovery_complete = monitored && group.temperature_deci_c.confirmed() &&
                group.recovery_since_ms && clock.uptime_ms >= *group.recovery_since_ms &&
                clock.uptime_ms - *group.recovery_since_ms >=
                    static_cast<MonotonicMs>(saved->recovery_delay_s) * 1000;
            bool alarm_active = false;
            for (const auto& track : tracked_) if (track.used && track.active &&
                track.type == AlarmType::GroupOverTemperature && track.group == group.id) {
                alarm_active = true;
                break;
            }
            // Keep an active temperature alarm latched through brief dips, stale
            // readings and group-state changes. Only the configured recovery
            // threshold held for its full delay clears it.
            over = monitored && group.temperature_deci_c.confirmed() && group.emergency_operation;
            if (monitored && alarm_active && !recovery_complete) over = true;
        }
        observations[count++] = {AlarmType::GroupOverTemperature, {}, group.id, over,
            group.temperature_deci_c.value ? static_cast<std::uint32_t>(*group.temperature_deci_c.value) : 0};
    }
    for (auto& track : tracked_) {
        track.seen = false;
        if (!track.used) continue;
        bool present = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (track.type == observations[i].type && track.address == observations[i].address &&
                track.group == observations[i].group) {
                present = true;
                break;
            }
        }
        if (!present) track = Track{};
    }
    const std::uint32_t observe_s = config ? config->startup_observe_only_s : 60;
    const bool suppress = clock.uptime_ms < static_cast<MonotonicMs>(observe_s) * 1000;
    state.alarms.clear();

    auto enqueue = [&](const Track& track, AlarmState alarm_state, std::uint32_t code) {
        AlarmEvent event; event.alarm.type = track.type; event.alarm.state = alarm_state;
        event.alarm.severity = alarmSeverity(track.type); event.alarm.address = track.address;
        event.alarm.group_id = track.group; event.alarm.changed_ms = clock.uptime_ms;
        event.alarm.epoch = clock.epoch; event.alarm.code = code;
        if (track.address) for (const auto& ac : state.conditioners) if (ac.address == *track.address) event.ac_name = ac.name;
        if (track.group) for (const auto& group : state.groups) if (group.id == *track.group) event.group_name = group.name;
        event.text.assign(alarmText(track.type, alarm_state == AlarmState::Recovery));
        appendAlarmLog(event_log_, event);
        if (count_ == kQueue) { head_ = (head_ + 1) % kQueue; --count_; }
        queue_[(head_ + count_) % kQueue] = event; ++count_;
    };

    for (std::size_t i = 0; i < count; ++i) {
        const auto& observation = observations[i];
        Track* track = nullptr;
        for (auto& candidate : tracked_) if (candidate.used && candidate.type == observation.type &&
            candidate.address == observation.address && candidate.group == observation.group) { track = &candidate; break; }
        if (!track) {
            for (auto& candidate : tracked_) if (!candidate.used) {
                candidate = Track{}; candidate.used = true; candidate.type = observation.type;
                candidate.address = observation.address; candidate.group = observation.group; track = &candidate; break;
            }
        }
        if (!track) continue; // Bounded to the configured AC/group capacity.
        track->seen = true;
        if (suppress) {
            track->active = observation.problem; track->candidate = observation.problem; track->candidate_since_ms = clock.uptime_ms;
        } else {
            if (track->candidate != observation.problem) { track->candidate = observation.problem; track->candidate_since_ms = clock.uptime_ms; }
            if (clock.uptime_ms >= track->candidate_since_ms && clock.uptime_ms - track->candidate_since_ms >= kDebounceMs && track->active != track->candidate) {
                track->active = track->candidate;
                track->code = observation.code;
                enqueue(*track, track->active ? AlarmState::Problem : AlarmState::Recovery, observation.code);
            }
        }
        if (track->active) {
            Alarm alarm; alarm.type = track->type; alarm.state = AlarmState::Problem;
            alarm.severity = alarmSeverity(track->type); alarm.address = track->address; alarm.group_id = track->group;
            alarm.code = observation.code; state.alarms.push_back(alarm);
        }
    }
    for (auto& track : tracked_) if (track.used && !track.seen) {
        // Removing a monitored row is not evidence of a recovered condition.
        track = Track{};
    }
}

bool AlarmManager::nextEvent(AlarmEvent& event) {
    if (!count_) return false;
    event = queue_[head_]; head_ = (head_ + 1) % kQueue; --count_; return true;
}
} // namespace fl
