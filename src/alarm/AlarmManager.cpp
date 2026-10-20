#include "fl/alarm/AlarmManager.h"

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
        observations[count++] = {AlarmType::GroupDegraded, {}, group.id, group.state == GroupState::Degraded, 0};
        const GroupConfig* saved = nullptr;
        if (config) for (const auto& candidate : config->groups) if (candidate.id == group.id) { saved = &candidate; break; }
        const bool over = saved && group.temperature_deci_c.confirmed() &&
            *group.temperature_deci_c.value >= saved->high_temp_deci_c && group.state == GroupState::Emergency;
        observations[count++] = {AlarmType::GroupOverTemperature, {}, group.id, over,
            group.temperature_deci_c.value ? static_cast<std::uint32_t>(*group.temperature_deci_c.value) : 0};
    }
    for (auto& track : tracked_) track.seen = false;
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
        if (count_ == kQueue) { head_ = (head_ + 1) % kQueue; --count_; }
        queue_[(head_ + count_) % kQueue] = event; ++count_;
    };

    for (std::size_t i = 0; i < count; ++i) {
        const auto& observation = observations[i];
        Track* track = nullptr;
        for (auto& candidate : tracked_) if (candidate.used && candidate.type == observation.type &&
            candidate.address == observation.address && candidate.group == observation.group) { track = &candidate; break; }
        if (!track) {
            for (auto& candidate : tracked_) if (!candidate.used || !candidate.seen) {
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
