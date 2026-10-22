#pragma once

#include "fl/core/StateStore.h"
#include <algorithm>
#include <string_view>

namespace fl {

// Keep an already-running unit in the automation's healthy count through
// short poll gaps. The bus declares a unit offline only after N failed polls;
// this window includes those poll cycles and a conservative full-sweep margin.
inline MonotonicMs automationFreshnessWindowMs(const Config& config) {
    const MonotonicMs poll_ms = std::max<std::uint32_t>(1, config.bus.poll_interval_ms);
    const MonotonicMs failure_cycles = static_cast<MonotonicMs>(config.bus.offline_failures) + 1;
    const MonotonicMs attempts = static_cast<MonotonicMs>(config.bus.retry_limit) + 1;
    const MonotonicMs sweep_ms = static_cast<MonotonicMs>(config.bus.response_timeout_ms) *
        attempts * config.registry.size();
    return poll_ms * failure_cycles + sweep_ms;
}

inline const AcRegistration* automationRegistration(const Config& config, Address address) {
    for (const auto& registration : config.registry)
        if (registration.address == address) return &registration;
    return nullptr;
}

inline bool automationRecentlyObserved(const Config& config, const AcState& ac, MonotonicMs now) {
    return ac.availability == Availability::Online && ac.freshness == Freshness::Fresh &&
        ac.last_seen_ms && now >= *ac.last_seen_ms &&
        now - *ac.last_seen_ms <= automationFreshnessWindowMs(config);
}

// 0x0002 is the documented defrost state, which is expected during normal
// operation and must not make an otherwise powered conditioner unhealthy.
inline constexpr std::uint16_t kExpectedProtectionMask = 0x0002;

inline std::string_view automationHealthIssue(const Config& config, const AcState& ac, MonotonicMs now) {
    const auto* registration = automationRegistration(config, ac.address);
    if (!registration) return "not_registered";
    if (!registration->model_validated) return "model_not_validated";
    if (registration->maintenance || ac.maintenance) return "maintenance";
    if (ac.availability == Availability::Offline) return "offline";
    if (!automationRecentlyObserved(config, ac, now)) return "stale";
    if (!ac.observed.power.confirmed()) return "power_unknown";
    if (!*ac.observed.power.value) return "powered_off";
    if (ac.observed.error_flags.confirmed() && *ac.observed.error_flags.value) return "error";
    if (ac.observed.protection_flags.confirmed() &&
        (*ac.observed.protection_flags.value & static_cast<std::uint16_t>(~kExpectedProtectionMask)) != 0)
        return "protection";
    if (ac.observed.ccm_error_flags.confirmed() && *ac.observed.ccm_error_flags.value) return "ccm_error";
    return {};
}

inline bool automationHealthy(const Config& config, const AcState& ac, MonotonicMs now) {
    return automationHealthIssue(config, ac, now).empty();
}

} // namespace fl
