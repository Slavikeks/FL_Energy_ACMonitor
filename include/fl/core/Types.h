#pragma once

#include "fl/core/Bounded.h"
#include <cstdint>
#include <optional>

namespace fl {
inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr char kFirmwareVersion[] = "0.7.7";
// The transport scans all 64 protocol addresses, while the embedded runtime
// reserves typed records for 16 registered units and groups to fit WT32 RAM.
inline constexpr std::size_t kAddressCount = 64;
inline constexpr std::size_t kMaxAc = 16;
inline constexpr std::size_t kMaxGroups = 16;
inline constexpr std::size_t kMaxAlarms = 2 * kMaxAc + 2 * kMaxGroups;
inline constexpr std::size_t kLogCapacity = 200;
using Address = std::uint8_t;
using GroupId = std::uint16_t;
using TransactionId = std::uint32_t;
using MonotonicMs = std::uint64_t; // Clock adapter must extend wrapping millis().
using EpochSeconds = std::int64_t;
using Name = Text<96>;
constexpr bool validAddress(Address address) { return address < kAddressCount; }

enum class Result : std::uint8_t {
    Accepted, Invalid, Busy, Full, NotFound, Forbidden, NotImplemented, IoError
};
enum class Mode : std::uint8_t { Cool, Heat, Fan, Dry };
enum class Fan : std::uint8_t { Auto, Low, Medium, High };
enum class FanSpeed : std::uint8_t { Stopped, Low, Medium, High };
enum class Availability : std::uint8_t { Unknown, Online, Offline };
enum class ValueStatus : std::uint8_t { Unknown, Valid, Unsupported, Experimental };
enum class Freshness : std::uint8_t { NeverSeen, Fresh, Stale };
enum class CommandState : std::uint8_t { Idle, Pending, Acknowledged, Failed };
enum class CommandOrigin : std::uint8_t { Web, Rotation };
enum class GroupMode : std::uint8_t { Manual, Auto };
enum class GroupState : std::uint8_t { Manual, Normal, Rotating, Degraded, Emergency };
enum class HandoverPhase : std::uint8_t { Idle, Start, Verify, Overlap, Stop, Aborted, Break, Restore };
enum class AlarmType : std::uint8_t { AcOffline, AcFault, GroupDegraded, GroupOverTemperature };
enum class AlarmState : std::uint8_t { Problem, Recovery };
enum class Severity : std::uint8_t { Info, Warning, Critical };

template <typename T> struct Sample {
    std::optional<T> value{};
    ValueStatus status{ValueStatus::Unknown};
    MonotonicMs updated_ms{0};
    // Freshness and model validation are additional requirements for safety use.
    bool confirmed() const { return status == ValueStatus::Valid && value.has_value(); }
};

struct ClockState {
    MonotonicMs uptime_ms{0};
    std::optional<EpochSeconds> epoch{};
};
struct ControlSettings {
    std::optional<bool> power{};
    std::optional<Mode> mode{};
    std::optional<std::int16_t> setpoint_deci_c{};
    std::optional<Fan> fan{};
};
struct ControlRequest {
    Address address{0};
    ControlSettings settings{};
    CommandOrigin origin{CommandOrigin::Web};
    TransactionId transaction_id{0};
    std::optional<GroupId> group_id{};
};
} // namespace fl
