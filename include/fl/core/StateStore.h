#pragma once
#include "fl/ac/Registry.h"
#include "fl/alarm/Alarm.h"
#include "fl/core/Config.h"

namespace fl {
struct ObservedState {
    Sample<bool> power{};
    Sample<Mode> mode{};
    Sample<Fan> fan_selected{};
    Sample<FanSpeed> fan_actual{};
    Sample<bool> fan_auto{};
    Sample<std::int16_t> setpoint_deci_c{};
    Sample<std::int16_t> t1_deci_c{}, t2_deci_c{}, t2b_deci_c{}, t3_deci_c{};
    Sample<bool> compressor_running{}, drain_pump{}, water_lock{};
    Sample<std::uint16_t> error_flags{}, protection_flags{};
    Sample<std::uint8_t> ccm_error_flags{}, capabilities_raw{};
};
struct DesiredState {
    ControlSettings settings{};
    CommandState command_state{CommandState::Idle};
    TransactionId transaction_id{0};
    CommandOrigin origin{CommandOrigin::Web};
    MonotonicMs submitted_ms{0};
    Result failure{Result::Accepted};
};
struct AcState {
    Address address{0};
    Name name{};
    std::optional<GroupId> group_id{};
    bool maintenance{false};
    bool model_validated{false};
    Availability availability{Availability::Unknown};
    Freshness freshness{Freshness::NeverSeen};
    ObservedState observed{};
    DesiredState desired{};
    std::optional<MonotonicMs> last_seen_ms{};
    std::optional<EpochSeconds> last_seen_epoch{};
    std::optional<MonotonicMs> last_started_ms{}, last_stopped_ms{};
    std::uint8_t consecutive_failures{0}, consecutive_successes{0};
};
struct BusStats {
    std::uint32_t baud{4800};
    std::uint32_t poll_ok{0}, timeouts{0}, checksum_errors{0}, rejected_frames{0};
    bool scan_running{false};
    std::uint8_t scan_completed{0};
    std::bitset<kAddressCount> discovered{};
};
struct NetworkState {
    bool link{false};
    bool dhcp{false};
    std::optional<Ipv4> ip{};
};
struct RuntimeState {
    std::uint32_t schema_version{kSchemaVersion};
    Name device_name{std::string_view{"FL_Energy_ACMonitor"}};
    ClockState clock{};
    NetworkState network{};
    BusStats bus{};
    List<AcState, kMaxAc> conditioners{};
    List<GroupRuntime, kMaxGroups> groups{};
    List<Alarm, kMaxAlarms> alarms{};
    bool observe_only{true};
    bool ota_active{false};
    bool critical_settings_change{false};
};
// Public typed state contains no Config, password hashes, communities or tokens.
// Single loop owns writes; readers may borrow only during a synchronous tick.
// Multitasking in later stages requires a locking/snapshot adapter.
class IStateView {
public:
    virtual ~IStateView() = default;
    virtual const RuntimeState& read() const = 0;
};
class StateStore final : public IStateView {
public:
    const RuntimeState& read() const override { return state_; }
    RuntimeState& edit() { return state_; }
private:
    RuntimeState state_{};
};
inline constexpr std::size_t kModelBytes = sizeof(Config) + sizeof(StateStore);
static_assert(kModelBytes <= 96 * 1024, "Typed model exceeds initial 96 KiB budget");
} // namespace fl
