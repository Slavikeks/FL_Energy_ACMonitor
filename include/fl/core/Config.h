#pragma once
#include "fl/ac/Registry.h"
#include "fl/rotation/Group.h"

namespace fl {
struct Ipv4 { std::array<std::uint8_t, 4> octets{}; };
struct NetworkConfig {
    bool dhcp{false};
    Ipv4 ip{{10, 90, 90, 90}};
    Ipv4 mask{{255, 255, 255, 0}};
    Ipv4 gateway{{10, 90, 90, 1}};
    Ipv4 dns{{10, 90, 90, 1}};
};
struct TimeConfig {
    Text<128> server{std::string_view{"pool.ntp.org"}}; // One configurable server.
    Text<64> timezone{std::string_view{"UTC-5"}}; // POSIX sign convention; Asia/Tashkent (UTC+05).
};
struct AuthConfig {
    Text<32> username{std::string_view{"admin"}};
    Text<192> salted_password_kdf{}; // versioned KDF encoding; never plaintext
    bool onboarding_required{false}; // Owner selected admin/admin, no forced rotation.
    std::uint32_t session_lifetime_s{3600};
};
struct SnmpConfig {
    bool enabled{false};
    Text<64> ro_community{};
    Name system_name{std::string_view{"FL_Energy_ACMonitor"}};
    Name location{};
    Name contact{};
    std::optional<Ipv4> trap_receiver{}; // Exactly one destination.
    std::uint16_t trap_port{162};
};
struct MqttConfig {
    bool enabled{false};
    Text<128> broker{};
    std::uint16_t port{1883};
    Text<64> username{};
    Text<128> password{};
    Text<96> topic_prefix{std::string_view{"fl_energy"}};
    std::uint32_t publish_period_s{30};
};
struct BusConfig {
    std::uint32_t poll_interval_ms{12000};
    std::uint32_t response_timeout_ms{500}; // Provisional; tune during stage 2.
    std::uint8_t retry_limit{1};
    std::uint8_t offline_failures{3};
    std::uint8_t online_successes{2};
};
struct Config {
    std::uint32_t schema_version{kSchemaVersion};
    NetworkConfig network{};
    TimeConfig time{};
    AuthConfig auth{};
    SnmpConfig snmp{};
    MqttConfig mqtt{};
    BusConfig bus{};
    Registry registry{};
    List<GroupConfig, kMaxGroups> groups{};
    bool automatic_control_enabled{false};
    std::uint32_t startup_observe_only_s{60};
};
class IConfigRepository {
public:
    virtual ~IConfigRepository() = default;
    virtual Result load(Config&) = 0;
    virtual Result save(const Config&) = 0; // NVS adapter in stage 1, wear-aware.
    virtual Result loadRegistry(Registry&) { return Result::NotFound; }
    virtual Result saveRegistry(const Registry&) { return Result::NotImplemented; }
    virtual Result loadGroups(List<GroupConfig, kMaxGroups>&) { return Result::NotFound; }
    virtual Result saveGroups(const List<GroupConfig, kMaxGroups>&) { return Result::NotImplemented; }
};
// Compact, versioned, integrity-checked NVS payload. Deliberately stores only
// stage-1 settings; larger AC/group registry records get their own keyed storage.
inline constexpr std::size_t kPersistentConfigMaxBytes = 512;
Result encodePersistentConfig(const Config&, std::uint8_t* out,
                              std::size_t capacity, std::size_t& written);
Result decodePersistentConfig(const std::uint8_t* bytes, std::size_t length,
                              Config& inOut);
} // namespace fl
