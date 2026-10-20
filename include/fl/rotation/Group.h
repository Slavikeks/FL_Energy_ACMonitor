#pragma once
#include "fl/core/Types.h"
#include <bitset>

namespace fl {
struct GroupConfig {
    GroupId id{0}; // zero reserved, persistent ID independent of list position
    Name name{};
    List<Address, kMaxAc> members{};
    GroupMode mode{GroupMode::Manual};
    std::uint8_t required_active{1};
    std::uint8_t maximum_active{1};
    Mode ac_mode{Mode::Cool};
    std::int16_t setpoint_deci_c{220};
    Fan fan{Fan::Auto};
    bool ventilation_reserve{false};
    std::uint32_t color_rgb{0x5A9EBB};
    bool rotation_enabled{false};
    std::uint16_t interval_days{30};
    std::uint8_t rotation_hour{10};
    std::uint8_t rotation_minute{0};
    std::int16_t high_temp_deci_c{280};
    std::uint32_t high_delay_s{300};
    std::int16_t recovery_temp_deci_c{250};
    std::uint32_t recovery_delay_s{600};
    std::uint32_t handover_s{300};
    std::uint32_t start_verification_s{60};
    std::uint32_t min_on_s{600};
    std::uint32_t min_off_s{300};
    std::optional<EpochSeconds> last_rotation_anchor{};
};
struct GroupRuntime {
    GroupId id{0};
    Name name{};
    List<Address, kMaxAc> members{};
    GroupMode mode{GroupMode::Manual};
    std::uint8_t required_active{1};
    std::uint8_t maximum_active{1};
    Mode ac_mode{Mode::Cool};
    std::int16_t setpoint_deci_c{220};
    Fan fan{Fan::Auto};
    bool ventilation_reserve{false};
    std::uint32_t color_rgb{0x5A9EBB};
    bool rotation_enabled{false};
    GroupState state{GroupState::Manual};
    HandoverPhase phase{HandoverPhase::Idle};
    std::bitset<kAddressCount> active_addresses{};
    std::bitset<kAddressCount> ventilation_addresses{};
    Sample<std::int16_t> temperature_deci_c{};
    std::optional<EpochSeconds> rotation_next_epoch{};
    std::optional<Address> outgoing{};
    std::optional<Address> incoming{};
    TransactionId transaction_id{0};
    MonotonicMs phase_started_ms{0};
    MonotonicMs retry_after_ms{0};
    std::optional<MonotonicMs> high_since_ms{};
    std::optional<MonotonicMs> recovery_since_ms{};
    bool rotation_handover{false};
    bool rotation_break_before_make{false};
    bool failover_break_before_make{false};
    bool emergency_operation{false};
    Text<128> last_action_reason{};
    std::optional<Address> settings_sync_address{};
    TransactionId settings_sync_transaction_id{0};
    MonotonicMs settings_sync_retry_after_ms{0};
    bool stop_to_ventilation{false};
};
struct Config;
struct RuntimeState;
class ICommandSink;
class IRotationEngine {
public:
    virtual ~IRotationEngine() = default;
    // Nonblocking step; every command must pass through the shared command sink.
    virtual void tick(Config&, RuntimeState&, const ClockState&, ICommandSink&) = 0;
};
} // namespace fl
