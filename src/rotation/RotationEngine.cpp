#include "fl/rotation/RotationEngine.h"
#include "fl/core/AutomationHealth.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace fl {
namespace {
void log(EventLog& sink, Severity severity, std::string_view message) {
    LogEvent entry; entry.severity = severity; entry.message.assign(message); sink.append(entry);
}
AcState* findAc(RuntimeState& state, Address address) {
    for (auto& ac : state.conditioners) if (ac.address == address) return &ac;
    return nullptr;
}
const AcRegistration* findRegistration(const Config& config, Address address) {
    for (const auto& ac : config.registry) if (ac.address == address) return &ac;
    return nullptr;
}
bool fresh(const AcState& ac, MonotonicMs now, std::uint32_t poll_interval_ms) {
    return ac.availability == Availability::Online && ac.freshness == Freshness::Fresh &&
        ac.last_seen_ms && now >= *ac.last_seen_ms &&
        now - *ac.last_seen_ms <= static_cast<MonotonicMs>(poll_interval_ms) * 2;
}
bool hasFault(const AcState& ac) {
    return (ac.observed.error_flags.confirmed() && *ac.observed.error_flags.value != 0) ||
        (ac.observed.protection_flags.confirmed() && *ac.observed.protection_flags.value != 0) ||
        (ac.observed.ccm_error_flags.confirmed() && *ac.observed.ccm_error_flags.value != 0);
}
bool activeHealthy(const Config& config, const AcState& ac, MonotonicMs now) {
    return automationHealthy(config, ac, now);
}
bool ventilationReserveMode(const GroupConfig& group, const AcState& ac) {
    return group.ac_mode != Mode::Fan && ac.observed.power.confirmed() && *ac.observed.power.value &&
        ac.observed.mode.confirmed() && *ac.observed.mode.value == Mode::Fan;
}
bool groupActiveHealthy(const Config& config, const AcState& ac, MonotonicMs now,
                        const GroupConfig& group) {
    return activeHealthy(config, ac, now) && !ventilationReserveMode(group, ac);
}
bool candidate(const Config& config, const AcState& ac, MonotonicMs now,
               const GroupConfig& group) {
    const auto* registration = findRegistration(config, ac.address);
    if (!registration || !registration->model_validated || registration->maintenance || ac.maintenance ||
        !fresh(ac, now, config.bus.poll_interval_ms) || !ac.observed.power.confirmed() ||
        hasFault(ac) || ac.desired.command_state == CommandState::Pending) return false;
    const bool reserve_in_fan_standby = group.ventilation_reserve && ventilationReserveMode(group, ac);
    if (*ac.observed.power.value && !reserve_in_fan_standby) return false;
    // A reserve already powered in ventilation standby does not need an OFF
    // cooldown before it can be promoted into the group's operating mode.
    if (reserve_in_fan_standby) return true;
    return !ac.last_stopped_ms || (now >= *ac.last_stopped_ms &&
        now - *ac.last_stopped_ms >= static_cast<MonotonicMs>(group.min_off_s) * 1000);
}
bool localTime(const std::time_t& value, std::tm& result) {
#if defined(_WIN32)
    return localtime_s(&result, &value) == 0;
#else
    return localtime_r(&value, &result) != nullptr;
#endif
}
std::optional<EpochSeconds> nextRotationEpoch(const GroupConfig& group, EpochSeconds now) {
    if (!group.rotation_enabled || !group.last_rotation_anchor) return std::nullopt;
    const EpochSeconds interval_due = *group.last_rotation_anchor +
        static_cast<EpochSeconds>(group.interval_days) * 86400;
    const EpochSeconds base = std::max(now, interval_due);
    const std::time_t raw_base = static_cast<std::time_t>(base);
    std::tm base_local{};
    if (!localTime(raw_base, base_local)) return std::nullopt;
    if (base_local.tm_hour == group.rotation_hour && base_local.tm_min == group.rotation_minute)
        return base;
    base_local.tm_hour = group.rotation_hour;
    base_local.tm_min = group.rotation_minute;
    base_local.tm_sec = 0;
    base_local.tm_isdst = -1;
    std::time_t scheduled = std::mktime(&base_local);
    if (scheduled < 0) return std::nullopt;
    if (static_cast<EpochSeconds>(scheduled) < base) {
        base_local.tm_mday += 1;
        base_local.tm_isdst = -1;
        scheduled = std::mktime(&base_local);
        if (scheduled < 0) return std::nullopt;
    }
    return static_cast<EpochSeconds>(scheduled);
}
bool uncertainPoweredMember(const Config& config, const GroupConfig& group,
                            const RuntimeState& state, MonotonicMs now) {
    for (const auto address : group.members) {
        const AcState* ac = nullptr;
        for (const auto& item : state.conditioners) if (item.address == address) { ac = &item; break; }
        if (!ac) return true;
        const auto failed_poll_limit = std::max<std::uint8_t>(1, config.bus.offline_failures);
        const bool declared_lost = ac->availability == Availability::Offline &&
            ac->consecutive_failures >= failed_poll_limit;
        if (declared_lost) continue;
        if (group.ac_mode != Mode::Fan && fresh(*ac, now, config.bus.poll_interval_ms) &&
            ac->observed.power.confirmed() && *ac->observed.power.value &&
            ac->observed.mode.confirmed() && *ac->observed.mode.value == Mode::Fan) continue;
        const auto* registration = findRegistration(config, address);
        if (ac->maintenance || (registration && registration->maintenance)) {
            if (!fresh(*ac, now, config.bus.poll_interval_ms) || !ac->observed.power.confirmed()) return true;
            continue;
        }
        if (!fresh(*ac, now, config.bus.poll_interval_ms) || !ac->observed.power.confirmed() ||
            (*ac->observed.power.value && !activeHealthy(config, *ac, now))) return true;
    }
    return false;
}
bool knownOperationalFault(const Config& config, const AcState& ac, MonotonicMs now) {
    const auto issue = automationHealthIssue(config, ac, now);
    // Protection flags include expected states such as defrost; only explicit
    // equipment/CCM error flags are strong enough to stop a powered unit.
    return issue == "error" || issue == "ccm_error";
}
bool settingsDifferFromGroup(const GroupConfig& group, const AcState& ac) {
    if (ac.observed.mode.confirmed() && *ac.observed.mode.value != group.ac_mode) return true;
    if (group.ac_mode != Mode::Fan && ac.observed.mode.confirmed() &&
        *ac.observed.mode.value == group.ac_mode && ac.observed.setpoint_deci_c.confirmed() &&
        *ac.observed.setpoint_deci_c.value != group.setpoint_deci_c) return true;
    if (!ac.observed.fan_auto.confirmed()) return false;
    if (group.fan == Fan::Auto) return !*ac.observed.fan_auto.value;
    if (*ac.observed.fan_auto.value) return true;
    // C0 reports actual fan speed, not the selected fixed speed. Compare it
    // only while the fan is expected to run continuously, avoiding false
    // corrections during compressor cycling.
    const bool fan_expected_to_run = group.ac_mode == Mode::Fan ||
        (ac.observed.compressor_running.confirmed() && *ac.observed.compressor_running.value);
    if (!fan_expected_to_run || !ac.observed.fan_actual.confirmed()) return false;
    const auto expected = group.fan == Fan::Low ? FanSpeed::Low :
        group.fan == Fan::Medium ? FanSpeed::Medium : FanSpeed::High;
    return *ac.observed.fan_actual.value != expected;
}
}

RotationEngine::RotationEngine(IConfigRepository& repository, EventLog& log)
    : repository_(repository), log_(log) {}

bool RotationEngine::startCommand(const GroupConfig& group, GroupRuntime& runtime, Address address,
                                  RuntimeState& state, ICommandSink& commands, MonotonicMs now,
                                  const char* reason) {
    ControlRequest request;
    request.address = address; request.origin = CommandOrigin::Rotation;
    request.transaction_id = next_transaction_id_++;
    if (!next_transaction_id_) next_transaction_id_ = 0x40000000U;
    request.group_id = group.id;
    request.settings.power = true; request.settings.mode = group.ac_mode;
    request.settings.fan = group.fan;
    if (group.ac_mode != Mode::Fan) request.settings.setpoint_deci_c = group.setpoint_deci_c;
    const Result result = commands.enqueue(request);
    if (result != Result::Accepted) return false;
    runtime.incoming = address; runtime.transaction_id = request.transaction_id;
    runtime.phase = HandoverPhase::Start; runtime.phase_started_ms = now;
    runtime.retry_after_ms = 0;
    runtime.last_action_reason.assign(reason ? reason : "Automatic start queued");
    log(log_, Severity::Info, runtime.last_action_reason.view());
    (void)state;
    return true;
}

bool RotationEngine::stopCommand(const Config& config, const GroupConfig& group, GroupRuntime& runtime, Address address,
                                 RuntimeState& state, ICommandSink& commands, MonotonicMs now,
                                 const char* reason, bool allow_below_required) {
    std::size_t healthy = 0;
    for (const auto member : group.members) {
        const auto* ac = findAc(state, member);
        if (ac && groupActiveHealthy(config, *ac, now, group)) ++healthy;
    }
    if (healthy <= group.required_active && !allow_below_required) return false;
    ControlRequest request;
    request.address = address; request.origin = CommandOrigin::Rotation;
    request.transaction_id = next_transaction_id_++;
    if (!next_transaction_id_) next_transaction_id_ = 0x40000000U;
    request.group_id = group.id;
    const bool stop_to_ventilation = group.ventilation_reserve && group.ac_mode != Mode::Fan;
    request.settings.power = stop_to_ventilation;
    if (stop_to_ventilation) {
        request.settings.mode = Mode::Fan;
        request.settings.fan = group.fan;
    }
    const Result result = commands.enqueue(request);
    if (result != Result::Accepted) return false;
    runtime.stop_to_ventilation = stop_to_ventilation;
    runtime.outgoing = address; runtime.transaction_id = request.transaction_id;
    runtime.phase = HandoverPhase::Stop; runtime.phase_started_ms = now;
    runtime.last_action_reason.assign(reason ? reason : "Automatic stop queued");
    log(log_, Severity::Info, runtime.last_action_reason.view());
    return true;
}

bool RotationEngine::reconcileSettings(const Config& config, const GroupConfig& group, GroupRuntime& runtime,
                                       RuntimeState& state, ICommandSink& commands, MonotonicMs now) {
    if (now < runtime.settings_sync_retry_after_ms) return false;
    for (const auto address : group.members) {
        auto* ac = findAc(state, address);
        const auto* registration = findRegistration(config, address);
        if (!ac || !registration || !registration->model_validated || registration->maintenance || ac->maintenance ||
            !fresh(*ac, now, config.bus.poll_interval_ms) || !ac->observed.power.confirmed() ||
            ac->desired.command_state == CommandState::Pending || hasFault(*ac)) continue;
        bool active = groupActiveHealthy(config, *ac, now, group);
        bool ventilation_request = false;
        if (!active && group.ventilation_reserve && group.ac_mode != Mode::Fan &&
            (!*ac->observed.power.value || ventilationReserveMode(group, *ac))) {
            bool fan_differs = !*ac->observed.power.value || !ac->observed.mode.confirmed() ||
                *ac->observed.mode.value != Mode::Fan || !ac->observed.fan_auto.confirmed() ||
                *ac->observed.fan_auto.value != (group.fan == Fan::Auto);
            if (!fan_differs && group.fan != Fan::Auto && ac->observed.fan_actual.confirmed()) {
                const auto expected = group.fan == Fan::Low ? FanSpeed::Low :
                    group.fan == Fan::Medium ? FanSpeed::Medium : FanSpeed::High;
                fan_differs = *ac->observed.fan_actual.value != expected;
            }
            if (!fan_differs) continue;
            ventilation_request = true;
        } else if (!active && !group.ventilation_reserve && ventilationReserveMode(group, *ac)) {
            // When standby ventilation is disabled, a fan-only member is not
            // an active unit. Power it down; normal N logic may later restart it.
            ControlRequest request;
            request.address = address; request.origin = CommandOrigin::Rotation; request.group_id = group.id;
            request.transaction_id = next_transaction_id_++;
            if (!next_transaction_id_) next_transaction_id_ = 0x40000000U;
            request.settings.power = false;
            if (commands.enqueue(request) != Result::Accepted) {
                runtime.settings_sync_retry_after_ms = now + 1000;
                return false;
            }
            runtime.settings_sync_address = address;
            runtime.settings_sync_transaction_id = request.transaction_id;
            runtime.settings_sync_retry_after_ms = now + 30000;
            runtime.last_action_reason.assign("Automatic standby ventilation disabled; powering off reserve");
            log(log_, Severity::Info, runtime.last_action_reason.view());
            return true;
        } else if (!active || !settingsDifferFromGroup(group, *ac)) continue;
        ControlRequest request;
        request.address = address; request.origin = CommandOrigin::Rotation; request.group_id = group.id;
        request.transaction_id = next_transaction_id_++;
        if (!next_transaction_id_) next_transaction_id_ = 0x40000000U;
        request.settings.power = true;
        request.settings.mode = ventilation_request ? Mode::Fan : group.ac_mode;
        request.settings.fan = group.fan;
        if (!ventilation_request && group.ac_mode != Mode::Fan) request.settings.setpoint_deci_c = group.setpoint_deci_c;
        if (commands.enqueue(request) != Result::Accepted) {
            runtime.settings_sync_retry_after_ms = now + 1000;
            return false;
        }
        runtime.settings_sync_address = address;
        runtime.settings_sync_transaction_id = request.transaction_id;
        runtime.settings_sync_retry_after_ms = now + 30000;
        char message[128]{};
        std::snprintf(message, sizeof(message),
            ventilation_request ? "Automatic standby ventilation queued for AC %u" :
                "Automatic settings correction queued for AC %u after a remote change",
            static_cast<unsigned>(address));
        runtime.last_action_reason.assign(message);
        log(log_, Severity::Info, runtime.last_action_reason.view());
        return true;
    }
    return false;
}

bool RotationEngine::beginBreakRotation(const GroupConfig& group, GroupRuntime& runtime,
                                       Address outgoing, Address incoming, ICommandSink& commands,
                                       MonotonicMs now) {
    ControlRequest request;
    request.address = outgoing; request.origin = CommandOrigin::Rotation;
    request.transaction_id = next_transaction_id_++;
    if (!next_transaction_id_) next_transaction_id_ = 0x40000000U;
    request.group_id = group.id;
    request.settings.power = group.ventilation_reserve && group.ac_mode != Mode::Fan;
    if (*request.settings.power) { request.settings.mode = Mode::Fan; request.settings.fan = group.fan; }
    if (commands.enqueue(request) != Result::Accepted) return false;
    runtime.outgoing = outgoing; runtime.incoming = incoming;
    runtime.transaction_id = request.transaction_id; runtime.phase = HandoverPhase::Break;
    runtime.phase_started_ms = now; runtime.retry_after_ms = 0;
    runtime.rotation_handover = true; runtime.rotation_break_before_make = true;
    runtime.last_action_reason.assign("Scheduled rotation: stopping the current unit before starting its replacement");
    log(log_, Severity::Info, runtime.last_action_reason.view());
    return true;
}

bool RotationEngine::beginFailoverBreak(const GroupConfig& group, GroupRuntime& runtime,
                                        Address outgoing, Address incoming, ICommandSink& commands,
                                        MonotonicMs now) {
    ControlRequest request;
    request.address = outgoing; request.origin = CommandOrigin::Rotation;
    request.transaction_id = next_transaction_id_++;
    if (!next_transaction_id_) next_transaction_id_ = 0x40000000U;
    request.group_id = group.id; request.settings.power = false;
    if (commands.enqueue(request) != Result::Accepted) return false;
    runtime.outgoing = outgoing; runtime.incoming = incoming;
    runtime.transaction_id = request.transaction_id; runtime.phase = HandoverPhase::Break;
    runtime.phase_started_ms = now; runtime.retry_after_ms = 0;
    runtime.rotation_handover = false; runtime.rotation_break_before_make = false;
    runtime.failover_break_before_make = true;
    runtime.last_action_reason.assign("Automatic failover: stopping the faulted unit before starting its reserve");
    log(log_, Severity::Warning, runtime.last_action_reason.view());
    return true;
}

void RotationEngine::persistAnchor(Config& config, GroupId id, EpochSeconds anchor) {
    groups_scratch_ = config.groups;
    bool found = false;
    for (auto& group : groups_scratch_) if (group.id == id) {
        group.last_rotation_anchor = anchor; found = true; break;
    }
    if (!found) return;
    if (repository_.saveGroups(groups_scratch_) == Result::Accepted) config.groups = groups_scratch_;
    else log(log_, Severity::Warning, "Rotation anchor could not be saved");
}

void RotationEngine::tick(Config& config, RuntimeState& state, const ClockState& clock,
                          ICommandSink& commands) {
    for (auto& ac : state.conditioners) ac.group_id.reset();
    // Keep the public runtime projection aligned with persisted configuration.
    for (std::size_t i = 0; i < state.groups.size();) {
        bool exists = false;
        for (const auto& group : config.groups) if (group.id == state.groups[i].id) { exists = true; break; }
        if (!exists) state.groups.erase(i); else ++i;
    }
    for (const auto& group : config.groups) {
        GroupRuntime* runtime = nullptr;
        for (auto& current : state.groups) if (current.id == group.id) { runtime = &current; break; }
        if (!runtime) {
            GroupRuntime fresh_runtime; fresh_runtime.id = group.id;
            if (!state.groups.push_back(fresh_runtime)) continue;
            runtime = &state.groups[state.groups.size() - 1];
        }
        runtime->name = group.name; runtime->members = group.members; runtime->mode = group.mode;
        runtime->required_active = group.required_active; runtime->maximum_active = group.maximum_active;
        runtime->ac_mode = group.ac_mode; runtime->setpoint_deci_c = group.setpoint_deci_c;
        runtime->fan = group.fan; runtime->rotation_enabled = group.rotation_enabled;
        if (group.rotation_enabled && clock.epoch && !group.last_rotation_anchor)
            persistAnchor(config, group.id, *clock.epoch);
        runtime->rotation_next_epoch = group.rotation_enabled && clock.epoch ?
            nextRotationEpoch(group, *clock.epoch) : std::nullopt;
        runtime->active_addresses.reset();
        runtime->ventilation_addresses.reset();
        runtime->ventilation_reserve = group.ventilation_reserve;
        runtime->color_rgb = group.color_rgb;
        std::size_t healthy_on = 0;
        std::size_t manual_control_powered = 0;
        std::size_t candidate_count = 0;
        bool has_temperature = false;
        std::int16_t maximum_temperature = -32768;
        for (const auto address : group.members) {
            auto* ac = findAc(state, address);
            if (!ac) continue;
            ac->group_id = group.id;
            const auto* registration = findRegistration(config, address);
            if (groupActiveHealthy(config, *ac, clock.uptime_ms, group)) {
                runtime->active_addresses.set(address); ++healthy_on;
            }
            if (group.ventilation_reserve && registration && registration->model_validated &&
                !registration->maintenance && !ac->maintenance && !hasFault(*ac) &&
                fresh(*ac, clock.uptime_ms, config.bus.poll_interval_ms) && ventilationReserveMode(group, *ac))
                runtime->ventilation_addresses.set(address);
            if ((ac->maintenance || (registration && registration->maintenance)) &&
                fresh(*ac, clock.uptime_ms, config.bus.poll_interval_ms) &&
                ac->observed.power.confirmed() && *ac->observed.power.value) ++manual_control_powered;
            if (candidate(config, *ac, clock.uptime_ms, group)) ++candidate_count;
            if (fresh(*ac, clock.uptime_ms, config.bus.poll_interval_ms) &&
                ac->observed.t1_deci_c.confirmed() && ac->observed.t1_deci_c.updated_ms <= clock.uptime_ms &&
                clock.uptime_ms - ac->observed.t1_deci_c.updated_ms <=
                    static_cast<MonotonicMs>(config.bus.poll_interval_ms) * 2) {
                maximum_temperature = std::max(maximum_temperature, *ac->observed.t1_deci_c.value);
                has_temperature = true;
            }
        }
        if (has_temperature) {
            runtime->temperature_deci_c.value = maximum_temperature;
            runtime->temperature_deci_c.status = ValueStatus::Valid;
            runtime->temperature_deci_c.updated_ms = clock.uptime_ms;
        } else {
            runtime->temperature_deci_c.value.reset();
            runtime->temperature_deci_c.status = ValueStatus::Unknown;
        }

        if (group.mode != GroupMode::Auto || !config.automatic_control_enabled || state.observe_only) {
            if (runtime->phase == HandoverPhase::Start || runtime->phase == HandoverPhase::Verify ||
                runtime->phase == HandoverPhase::Overlap || runtime->phase == HandoverPhase::Break ||
                runtime->phase == HandoverPhase::Restore) {
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false;
                runtime->rotation_break_before_make = false;
                runtime->failover_break_before_make = false;
                runtime->stop_to_ventilation = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
                log(log_, Severity::Warning, "Automatic handover cancelled because group automation was disabled");
            }
            runtime->state = GroupState::Manual;
            continue;
        }

        if (runtime->phase == HandoverPhase::Aborted) {
            if (clock.uptime_ms < runtime->retry_after_ms) { runtime->state = GroupState::Degraded; continue; }
            runtime->phase = HandoverPhase::Idle;
        }
        bool settings_sync_pending = false;
        if (runtime->settings_sync_address) {
            auto* synced = findAc(state, *runtime->settings_sync_address);
            if (synced && synced->desired.transaction_id == runtime->settings_sync_transaction_id &&
                synced->desired.command_state == CommandState::Pending) settings_sync_pending = true;
            if (synced && synced->desired.transaction_id == runtime->settings_sync_transaction_id &&
                synced->desired.command_state == CommandState::Failed) {
                runtime->settings_sync_retry_after_ms = std::max(runtime->settings_sync_retry_after_ms,
                    clock.uptime_ms + 60000);
                runtime->settings_sync_address.reset();
            } else if (!settings_sync_pending) {
                if (synced && synced->desired.transaction_id == runtime->settings_sync_transaction_id &&
                    synced->desired.command_state == CommandState::Acknowledged &&
                    synced->desired.settings.power && !*synced->desired.settings.power &&
                    synced->observed.power.confirmed() && !*synced->observed.power.value)
                    synced->last_stopped_ms = clock.uptime_ms;
                runtime->settings_sync_address.reset();
            }
        }
        const MonotonicMs verification_ms = std::max<MonotonicMs>(
            static_cast<MonotonicMs>(group.start_verification_s) * 1000,
            static_cast<MonotonicMs>(config.bus.poll_interval_ms) * (config.registry.size() + 2));
        if (runtime->phase == HandoverPhase::Break && clock.uptime_ms < runtime->retry_after_ms) continue;
        auto* incoming_ac = runtime->incoming ? findAc(state, *runtime->incoming) : nullptr;
        auto* outgoing_ac = runtime->outgoing ? findAc(state, *runtime->outgoing) : nullptr;
        if (runtime->phase == HandoverPhase::Break) {
            if (!outgoing_ac) {
                const bool failover = runtime->failover_break_before_make;
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false; runtime->rotation_break_before_make = false;
                runtime->failover_break_before_make = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
                runtime->last_action_reason.assign(failover ?
                    "Failover paused: failed unit state is unavailable; reserve was not started" :
                    "Rotation paused: old unit state is unavailable; reserve was not started");
                if (!failover && clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                log(log_, Severity::Warning, runtime->last_action_reason.view());
            } else if (outgoing_ac->desired.transaction_id == runtime->transaction_id &&
                       outgoing_ac->desired.command_state == CommandState::Failed) {
                const bool failover = runtime->failover_break_before_make;
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false; runtime->rotation_break_before_make = false;
                runtime->failover_break_before_make = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
                runtime->last_action_reason.assign(failover ?
                    "Failover paused: faulted unit did not confirm shutdown; reserve was not started" :
                    "Rotation paused: old unit did not confirm shutdown; reserve was not started");
                if (!failover && clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                log(log_, Severity::Warning, runtime->last_action_reason.view());
            } else if (fresh(*outgoing_ac, clock.uptime_ms, config.bus.poll_interval_ms) &&
                       outgoing_ac->observed.power.confirmed() &&
                       (runtime->failover_break_before_make ? !*outgoing_ac->observed.power.value :
                        group.ventilation_reserve && group.ac_mode != Mode::Fan ?
                            ventilationReserveMode(group, *outgoing_ac) : !*outgoing_ac->observed.power.value)) {
                if (runtime->rotation_break_before_make && ventilationReserveMode(group, *outgoing_ac))
                    outgoing_ac->last_stopped_ms = clock.uptime_ms;
                const Address old_address = *runtime->outgoing;
                const Address reserve_address = runtime->incoming.value_or(old_address);
                const bool failover = runtime->failover_break_before_make;
                const char* reason = failover ?
                    "Automatic failover: reserve starting after faulted unit confirmed off" :
                    group.ventilation_reserve ?
                        "Scheduled rotation: reserve starting after old unit entered ventilation standby" :
                        "Scheduled rotation: reserve started after old unit confirmed off";
                if (!startCommand(group, *runtime, reserve_address, state, commands, clock.uptime_ms, reason)) {
                    if (failover) {
                        runtime->retry_after_ms = clock.uptime_ms + 1000;
                        runtime->last_action_reason.assign("Failover waiting to queue reserve after confirmed shutdown");
                        log(log_, Severity::Warning, runtime->last_action_reason.view());
                    } else {
                        runtime->rotation_handover = false; runtime->rotation_break_before_make = false;
                        runtime->incoming = old_address; runtime->outgoing.reset();
                        runtime->phase = HandoverPhase::Restore; runtime->phase_started_ms = clock.uptime_ms;
                        runtime->retry_after_ms = clock.uptime_ms + 2100;
                        runtime->last_action_reason.assign("Rotation reserve unavailable; waiting to restore previous unit");
                        if (clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                        log(log_, Severity::Warning, runtime->last_action_reason.view());
                    }
                }
            } else if (clock.uptime_ms >= runtime->phase_started_ms &&
                       clock.uptime_ms - runtime->phase_started_ms > verification_ms) {
                const bool failover = runtime->failover_break_before_make;
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false; runtime->rotation_break_before_make = false;
                runtime->failover_break_before_make = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
                runtime->last_action_reason.assign(failover ?
                    "Failover paused: shutdown was not verified; reserve was not started" :
                    "Rotation paused: shutdown was not verified; reserve was not started");
                if (!failover && clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                log(log_, Severity::Warning, runtime->last_action_reason.view());
            }
        }
        if (runtime->phase == HandoverPhase::Restore) {
            const Address old_address = runtime->incoming.value_or(0);
            if (clock.uptime_ms >= runtime->retry_after_ms &&
                !startCommand(group, *runtime, old_address, state, commands, clock.uptime_ms,
                              "No-overlap rotation: restoring the previous unit after reserve failure")) {
                if (clock.uptime_ms - runtime->phase_started_ms > verification_ms) {
                    runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                    runtime->incoming.reset();
                    runtime->last_action_reason.assign("Critical: rotation failed and previous unit could not be restored");
                    log(log_, Severity::Critical, runtime->last_action_reason.view());
                } else runtime->retry_after_ms = clock.uptime_ms + 1000;
            }
        }
        incoming_ac = runtime->incoming ? findAc(state, *runtime->incoming) : nullptr;
        if (runtime->phase == HandoverPhase::Start || runtime->phase == HandoverPhase::Verify) {
            if (!incoming_ac) {
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false; runtime->rotation_break_before_make = false;
                runtime->failover_break_before_make = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
            } else if (incoming_ac->desired.transaction_id == runtime->transaction_id &&
                       incoming_ac->desired.command_state == CommandState::Failed) {
                const bool was_break_before_make = runtime->rotation_break_before_make;
                const bool was_rotation = runtime->rotation_handover;
                const bool was_failover = runtime->failover_break_before_make;
                const bool safe_to_restore = was_break_before_make && runtime->outgoing &&
                    fresh(*incoming_ac, clock.uptime_ms, config.bus.poll_interval_ms) &&
                    incoming_ac->observed.power.confirmed() &&
                    (!*incoming_ac->observed.power.value ||
                     (group.ventilation_reserve && ventilationReserveMode(group, *incoming_ac)));
                const Address old_address = runtime->outgoing.value_or(0);
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false; runtime->rotation_break_before_make = false;
                runtime->failover_break_before_make = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
                if (was_rotation && clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                if (safe_to_restore && !was_failover) {
                    runtime->phase = HandoverPhase::Restore; runtime->incoming = old_address;
                    runtime->phase_started_ms = clock.uptime_ms; runtime->retry_after_ms = clock.uptime_ms + 2100;
                    runtime->last_action_reason.assign("Rotation reserve failed; waiting to restore previous unit");
                    log(log_, Severity::Warning, runtime->last_action_reason.view());
                } else {
                    runtime->last_action_reason.assign(was_failover ?
                        "Failover reserve did not confirm; faulted unit remains off" : was_break_before_make ?
                        "No-overlap rotation failed; reserve did not confirm and previous unit may remain off" :
                        "Automatic start was not verified; existing unit left running");
                    log(log_, Severity::Warning, runtime->last_action_reason.view());
                }
                continue;
            } else if (incoming_ac->desired.transaction_id == runtime->transaction_id &&
                       incoming_ac->desired.command_state == CommandState::Acknowledged &&
                       fresh(*incoming_ac, clock.uptime_ms, config.bus.poll_interval_ms) &&
                       incoming_ac->observed.power.confirmed() && *incoming_ac->observed.power.value) {
                incoming_ac->last_started_ms = clock.uptime_ms;
                if (runtime->rotation_break_before_make || runtime->failover_break_before_make) {
                    const bool was_rotation = runtime->rotation_handover;
                    const bool was_failover = runtime->failover_break_before_make;
                    runtime->phase = HandoverPhase::Idle; runtime->incoming.reset(); runtime->outgoing.reset();
                    runtime->rotation_handover = false; runtime->rotation_break_before_make = false;
                    runtime->failover_break_before_make = false;
                    if (was_rotation && clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                    log(log_, Severity::Info, was_failover ? "Automatic failover completed after confirmed shutdown" :
                        "No-overlap scheduled rotation completed");
                } else if (runtime->outgoing) {
                    runtime->phase = HandoverPhase::Overlap; runtime->phase_started_ms = clock.uptime_ms;
                } else {
                    runtime->phase = HandoverPhase::Idle; runtime->incoming.reset();
                }
            } else if (clock.uptime_ms >= runtime->phase_started_ms &&
                       clock.uptime_ms - runtime->phase_started_ms > verification_ms) {
                const bool was_break_before_make = runtime->rotation_break_before_make;
                const bool was_rotation = runtime->rotation_handover;
                const bool was_failover = runtime->failover_break_before_make;
                const bool safe_to_restore = was_break_before_make && runtime->outgoing &&
                    fresh(*incoming_ac, clock.uptime_ms, config.bus.poll_interval_ms) &&
                    incoming_ac->observed.power.confirmed() &&
                    (!*incoming_ac->observed.power.value ||
                     (group.ventilation_reserve && ventilationReserveMode(group, *incoming_ac)));
                const Address old_address = runtime->outgoing.value_or(0);
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false; runtime->rotation_break_before_make = false;
                runtime->failover_break_before_make = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
                if (was_rotation && clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                if (safe_to_restore && !was_failover) {
                    runtime->phase = HandoverPhase::Restore; runtime->incoming = old_address;
                    runtime->phase_started_ms = clock.uptime_ms; runtime->retry_after_ms = clock.uptime_ms + 2100;
                    runtime->last_action_reason.assign("Rotation reserve timed out; waiting to restore previous unit");
                    log(log_, Severity::Warning, runtime->last_action_reason.view());
                } else {
                    runtime->last_action_reason.assign(was_failover ?
                        "Failover reserve timed out; faulted unit remains off" : was_break_before_make ?
                        "No-overlap rotation timed out; reserve state is uncertain and old unit may remain off" :
                        "Automatic start timed out; existing unit left running");
                    log(log_, Severity::Warning, runtime->last_action_reason.view());
                }
                continue;
            }
        }
        if (runtime->phase == HandoverPhase::Overlap) {
            if (!incoming_ac || !groupActiveHealthy(config, *incoming_ac, clock.uptime_ms, group)) {
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
                log(log_, Severity::Warning, "Replacement became unavailable during overlap; old unit left running");
            } else if (!outgoing_ac || !groupActiveHealthy(config, *outgoing_ac, clock.uptime_ms, group)) {
                runtime->phase = HandoverPhase::Idle; runtime->incoming.reset(); runtime->outgoing.reset();
                if (runtime->rotation_handover && clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                runtime->rotation_handover = false;
            } else if (clock.uptime_ms - runtime->phase_started_ms >=
                       static_cast<MonotonicMs>(group.handover_s) * 1000) {
                if (!stopCommand(config, group, *runtime, *runtime->outgoing, state, commands, clock.uptime_ms,
                                 "Scheduled rotation: replacement verified and overlap elapsed; stopping old unit")) {
                    runtime->retry_after_ms = clock.uptime_ms + 1000;
                }
            }
        }
        if (runtime->phase == HandoverPhase::Stop) {
            const bool stop_verified = outgoing_ac && fresh(*outgoing_ac, clock.uptime_ms, config.bus.poll_interval_ms) &&
                (runtime->stop_to_ventilation ? ventilationReserveMode(group, *outgoing_ac) :
                 outgoing_ac->observed.power.confirmed() && !*outgoing_ac->observed.power.value);
            if (!outgoing_ac || stop_verified) {
                if (outgoing_ac && runtime->stop_to_ventilation) outgoing_ac->last_stopped_ms = clock.uptime_ms;
                const bool scheduled_rotation_completed = runtime->rotation_handover;
                runtime->phase = HandoverPhase::Idle; runtime->incoming.reset(); runtime->outgoing.reset();
                runtime->rotation_handover = false;
                runtime->stop_to_ventilation = false;
                if (scheduled_rotation_completed && clock.epoch) persistAnchor(config, group.id, *clock.epoch);
                log(log_, Severity::Info, scheduled_rotation_completed ?
                    "Automatic scheduled rotation completed" : "Automatic group stop completed");
            } else if (outgoing_ac->desired.transaction_id == runtime->transaction_id &&
                       outgoing_ac->desired.command_state == CommandState::Failed) {
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false;
                runtime->stop_to_ventilation = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
            } else if (clock.uptime_ms - runtime->phase_started_ms > verification_ms) {
                runtime->phase = HandoverPhase::Aborted; runtime->retry_after_ms = clock.uptime_ms + 60000;
                runtime->rotation_handover = false;
                runtime->stop_to_ventilation = false;
                runtime->incoming.reset(); runtime->outgoing.reset();
                log(log_, Severity::Warning, "Automatic group stop was not verified");
            }
        }

        // Only fresh, validated T1 readings can trigger the emergency target.
        const bool high = has_temperature && maximum_temperature >= group.high_temp_deci_c;
        const bool recovered = has_temperature && maximum_temperature <= group.recovery_temp_deci_c;
        if (high) {
            if (!runtime->high_since_ms) runtime->high_since_ms = clock.uptime_ms;
            runtime->recovery_since_ms.reset();
        } else if (recovered) {
            if (!runtime->recovery_since_ms) runtime->recovery_since_ms = clock.uptime_ms;
            runtime->high_since_ms.reset();
        } else {
            runtime->high_since_ms.reset(); runtime->recovery_since_ms.reset();
        }
        const bool emergency = runtime->high_since_ms && clock.uptime_ms >= *runtime->high_since_ms &&
            clock.uptime_ms - *runtime->high_since_ms >= static_cast<MonotonicMs>(group.high_delay_s) * 1000;
        const bool recovery_complete = runtime->recovery_since_ms && clock.uptime_ms >= *runtime->recovery_since_ms &&
            clock.uptime_ms - *runtime->recovery_since_ms >= static_cast<MonotonicMs>(group.recovery_delay_s) * 1000;
        runtime->emergency_operation = emergency;
        runtime->state = emergency ? GroupState::Emergency : healthy_on < group.required_active ?
            GroupState::Degraded : runtime->phase != HandoverPhase::Idle ? GroupState::Rotating : GroupState::Normal;

        if (runtime->phase != HandoverPhase::Idle || clock.uptime_ms < runtime->retry_after_ms) continue;
        std::size_t target = group.required_active;
        if (emergency) target = group.maximum_active;
        // Unknown power must not trigger a replacement during a short poll gap.
        // Once the bus declares a member offline after its configured failure
        // streak, it is treated as lost and its reserve may take over.
        const bool unsafe_power_snapshot = uncertainPoweredMember(config, group, state, clock.uptime_ms);
        const std::size_t physical_on = healthy_on + manual_control_powered;
        if (healthy_on < target && candidate_count && physical_on < group.maximum_active &&
            runtime->retry_after_ms <= clock.uptime_ms) {
            AcState* faulted_powered = nullptr;
            for (const auto address : group.members) {
                auto* ac = findAc(state, address);
                const auto* registration = findRegistration(config, address);
                if (ac && !ac->maintenance && !(registration && registration->maintenance) &&
                    ac->observed.power.confirmed() && *ac->observed.power.value &&
                    knownOperationalFault(config, *ac, clock.uptime_ms)) {
                    faulted_powered = ac; break;
                }
            }
            if (faulted_powered) {
                for (const auto address : group.members) {
                    const auto* reserve = findAc(state, address);
                    if (!reserve || address == faulted_powered->address ||
                        !candidate(config, *reserve, clock.uptime_ms, group)) continue;
                    if (beginFailoverBreak(group, *runtime, faulted_powered->address, address,
                                           commands, clock.uptime_ms)) break;
                }
                if (runtime->phase == HandoverPhase::Break) continue;
            }
            if (unsafe_power_snapshot) continue;
            for (const auto address : group.members) {
                const auto* ac = findAc(state, address);
                if (ac && candidate(config, *ac, clock.uptime_ms, group)) {
                    char reason[128]{};
                    const AcState* lost_member = nullptr;
                    const auto failed_poll_limit = std::max<std::uint8_t>(1, config.bus.offline_failures);
                    for (const auto member : group.members) {
                        const auto* member_ac = findAc(state, member);
                        if (member_ac && member_ac->availability == Availability::Offline &&
                            member_ac->consecutive_failures >= failed_poll_limit &&
                            (!member_ac->observed.power.confirmed() || *member_ac->observed.power.value)) {
                            lost_member = member_ac; break;
                        }
                    }
                    if (lost_member) {
                        std::snprintf(reason, sizeof(reason),
                            "Automatic failover: AC %u lost after %u consecutive failed polls; starting reserve",
                            static_cast<unsigned>(lost_member->address), static_cast<unsigned>(failed_poll_limit));
                    } else if (emergency && has_temperature) {
                        std::snprintf(reason, sizeof(reason),
                            "Auto start: group T1 %d.%d C reached %d.%d C for %lu s",
                            maximum_temperature / 10, std::abs(maximum_temperature % 10),
                            group.high_temp_deci_c / 10, std::abs(group.high_temp_deci_c % 10),
                            static_cast<unsigned long>(group.high_delay_s));
                    } else {
                        std::snprintf(reason, sizeof(reason),
                            "Auto start: %u healthy (<N=%u); checking group member state",
                            static_cast<unsigned>(healthy_on), static_cast<unsigned>(group.required_active));
                        for (const auto member : group.members) {
                            const auto* member_ac = findAc(state, member);
                            if (!member_ac) continue;
                            const auto issue = automationHealthIssue(config, *member_ac, clock.uptime_ms);
                            if (!issue.empty() && issue != "powered_off") {
                                std::snprintf(reason, sizeof(reason),
                                    "Auto start: %u healthy (<N=%u); AC %u %.*s",
                                    static_cast<unsigned>(healthy_on), static_cast<unsigned>(group.required_active),
                                    static_cast<unsigned>(member), static_cast<int>(issue.size()), issue.data());
                                break;
                            }
                        }
                    }
                    if (startCommand(group, *runtime, address, state, commands, clock.uptime_ms, reason)) break;
                }
            }
            continue;
        }

        const bool above_maximum = physical_on > group.maximum_active;
        if (above_maximum || (healthy_on > target && (emergency || recovery_complete))) {
            for (const auto address : group.members) {
                const auto* ac = findAc(state, address);
                if (!ac || !groupActiveHealthy(config, *ac, clock.uptime_ms, group)) continue;
                if (ac->last_started_ms && (clock.uptime_ms < *ac->last_started_ms ||
                    clock.uptime_ms - *ac->last_started_ms < static_cast<MonotonicMs>(group.min_on_s) * 1000)) continue;
                const char* reason = above_maximum ? "Auto stop: enforcing maximum active count M including manual-control units" :
                    emergency ? "Auto stop: active count exceeds emergency maximum M" :
                    "Auto stop: T1 recovery delay completed; returning to required N";
                if (stopCommand(config, group, *runtime, address, state, commands, clock.uptime_ms,
                                reason, above_maximum)) break;
            }
            continue;
        }

        if (!settings_sync_pending && reconcileSettings(config, group, *runtime, state, commands, clock.uptime_ms)) continue;

        if (group.rotation_enabled && healthy_on == group.required_active && candidate_count > 0 &&
            !unsafe_power_snapshot && clock.epoch) {
            if (!group.last_rotation_anchor) {
                persistAnchor(config, group.id, *clock.epoch);
                continue;
            }
            const auto elapsed = *clock.epoch - *group.last_rotation_anchor;
            std::tm local{};
            const std::time_t current = static_cast<std::time_t>(*clock.epoch);
            if (elapsed >= static_cast<EpochSeconds>(group.interval_days) * 86400 &&
                localTime(current, local) && local.tm_hour == group.rotation_hour &&
                local.tm_min == group.rotation_minute) {
                Address outgoing = 0; bool found_outgoing = false;
                for (const auto address : group.members) {
                    const auto* ac = findAc(state, address);
                    if (!ac || !groupActiveHealthy(config, *ac, clock.uptime_ms, group)) continue;
                    if (ac->last_started_ms && (clock.uptime_ms < *ac->last_started_ms ||
                        clock.uptime_ms - *ac->last_started_ms < static_cast<MonotonicMs>(group.min_on_s) * 1000)) continue;
                    outgoing = address; found_outgoing = true; break;
                }
                if (found_outgoing) for (const auto address : group.members) {
                    const auto* ac = findAc(state, address);
                    if (!ac || !candidate(config, *ac, clock.uptime_ms, group)) continue;
                    if (group.handover_s == 0) {
                        if (beginBreakRotation(group, *runtime, outgoing, address, commands, clock.uptime_ms)) break;
                    } else {
                        char reason[128]{};
                        std::snprintf(reason, sizeof(reason),
                            "Scheduled rotation: start reserve %u to replace unit %u",
                            static_cast<unsigned>(address), static_cast<unsigned>(outgoing));
                        if (startCommand(group, *runtime, address, state, commands, clock.uptime_ms, reason)) {
                            runtime->outgoing = outgoing; runtime->rotation_handover = true; break;
                        }
                    }
                }
            }
        }
    }
}
} // namespace fl
