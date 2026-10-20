#include "fl/core/Config.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>

namespace fl {
namespace {
#pragma pack(push, 1)
struct StoredConfigV1 {
    char magic[4];
    std::uint16_t format_version;
    std::uint16_t length;
    std::uint32_t crc32;
    std::uint8_t dhcp;
    std::uint8_t ip[4], mask[4], gateway[4], dns[4];
    char ntp_server[128];
    char timezone[64];
    char username[32];
    char password_kdf[192];
    std::uint32_t session_lifetime_s;
};
struct StoredConfig {
    char magic[4];
    std::uint16_t format_version;
    std::uint16_t length;
    std::uint32_t crc32;
    std::uint8_t dhcp;
    std::uint8_t ip[4], mask[4], gateway[4], dns[4];
    char ntp_server[128];
    char timezone[64];
    char username[32];
    char password_kdf[192];
    std::uint32_t session_lifetime_s;
    std::uint8_t automatic_control_enabled;
    std::uint32_t startup_observe_only_s;
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable<StoredConfig>::value, "NVS record must be a POD");
static_assert(sizeof(StoredConfig) <= kPersistentConfigMaxBytes, "NVS settings record too large");
static_assert(sizeof(StoredConfigV1) <= kPersistentConfigMaxBytes, "Legacy NVS settings record too large");

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

template <std::size_t N> bool copyText(char (&out)[N], std::string_view value) {
    if (value.empty() || value.size() >= N || value.find('\0') != std::string_view::npos)
        return false;
    std::memcpy(out, value.data(), value.size());
    return true;
}
template <std::size_t N, std::size_t M> bool loadText(Text<M>& out, const char (&in)[N]) {
    const auto* end = static_cast<const char*>(std::memchr(in, '\0', N));
    if (!end) return false;
    return out.assign(std::string_view(in, static_cast<std::size_t>(end - in)));
}
void storeIp(std::uint8_t (&out)[4], Ipv4 value) {
    std::copy(value.octets.begin(), value.octets.end(), out);
}
Ipv4 loadIp(const std::uint8_t (&in)[4]) {
    Ipv4 value;
    std::copy(in, in + 4, value.octets.begin());
    return value;
}
}

Result encodePersistentConfig(const Config& config, std::uint8_t* out,
                              std::size_t capacity, std::size_t& written) {
    written = 0;
    if (!out || capacity < sizeof(StoredConfig)) return Result::Invalid;
    StoredConfig record{};
    std::memcpy(record.magic, "FLE1", 4);
    record.format_version = 2;
    record.length = sizeof(record);
    record.dhcp = config.network.dhcp ? 1 : 0;
    storeIp(record.ip, config.network.ip);
    storeIp(record.mask, config.network.mask);
    storeIp(record.gateway, config.network.gateway);
    storeIp(record.dns, config.network.dns);
    if (!copyText(record.ntp_server, config.time.server.view()) ||
        !copyText(record.timezone, config.time.timezone.view()) ||
        !copyText(record.username, config.auth.username.view())) return Result::Invalid;
    if (!config.auth.salted_password_kdf.view().empty() &&
        !copyText(record.password_kdf, config.auth.salted_password_kdf.view())) return Result::Invalid;
    record.session_lifetime_s = config.auth.session_lifetime_s;
    record.automatic_control_enabled = config.automatic_control_enabled ? 1 : 0;
    record.startup_observe_only_s = config.startup_observe_only_s;
    record.crc32 = crc32(reinterpret_cast<const std::uint8_t*>(&record) + offsetof(StoredConfig, dhcp),
                         sizeof(record) - offsetof(StoredConfig, dhcp));
    std::memcpy(out, &record, sizeof(record));
    written = sizeof(record);
    return Result::Accepted;
}

Result decodePersistentConfig(const std::uint8_t* bytes, std::size_t length, Config& config) {
    if (!bytes || (length != sizeof(StoredConfig) && length != sizeof(StoredConfigV1))) return Result::Invalid;
    if (length == sizeof(StoredConfigV1)) {
        StoredConfigV1 old{};
        std::memcpy(&old, bytes, sizeof(old));
        if (std::memcmp(old.magic, "FLE1", 4) != 0 || old.format_version != 1 ||
            old.length != sizeof(old) || old.dhcp > 1 || old.session_lifetime_s < 60 ||
            old.session_lifetime_s > 86400) return Result::Invalid;
        const auto expected = crc32(reinterpret_cast<const std::uint8_t*>(&old) + offsetof(StoredConfigV1, dhcp),
                                    sizeof(old) - offsetof(StoredConfigV1, dhcp));
        if (expected != old.crc32 || !std::memchr(old.ntp_server, '\0', sizeof(old.ntp_server)) ||
            !std::memchr(old.timezone, '\0', sizeof(old.timezone)) ||
            !std::memchr(old.username, '\0', sizeof(old.username)) ||
            !std::memchr(old.password_kdf, '\0', sizeof(old.password_kdf))) return Result::Invalid;
        config.network.dhcp = old.dhcp != 0;
        config.network.ip = loadIp(old.ip); config.network.mask = loadIp(old.mask);
        config.network.gateway = loadIp(old.gateway); config.network.dns = loadIp(old.dns);
        if (!loadText(config.time.server, old.ntp_server) || !loadText(config.time.timezone, old.timezone) ||
            !loadText(config.auth.username, old.username) || !loadText(config.auth.salted_password_kdf, old.password_kdf))
            return Result::Invalid;
        config.auth.session_lifetime_s = old.session_lifetime_s;
        config.auth.onboarding_required = false;
        config.automatic_control_enabled = false;
        config.startup_observe_only_s = 60;
        return Result::Accepted;
    }
    StoredConfig record{};
    std::memcpy(&record, bytes, sizeof(record));
    if (std::memcmp(record.magic, "FLE1", 4) != 0 || record.format_version != 2 ||
        record.length != sizeof(record) || record.dhcp > 1 ||
        record.session_lifetime_s < 60 || record.session_lifetime_s > 86400 ||
        record.automatic_control_enabled > 1 || record.startup_observe_only_s > 3600) return Result::Invalid;
    const auto expected = crc32(reinterpret_cast<const std::uint8_t*>(&record) + offsetof(StoredConfig, dhcp),
                                sizeof(record) - offsetof(StoredConfig, dhcp));
    if (expected != record.crc32) return Result::Invalid;

    // Validate all NUL terminators first, then decode directly into the existing
    // configuration object. A full Config temporary exceeded Arduino's loop stack.
    if (!std::memchr(record.ntp_server, '\0', sizeof(record.ntp_server)) ||
        !std::memchr(record.timezone, '\0', sizeof(record.timezone)) ||
        !std::memchr(record.username, '\0', sizeof(record.username)) ||
        !std::memchr(record.password_kdf, '\0', sizeof(record.password_kdf))) return Result::Invalid;
    config.network.dhcp = record.dhcp != 0;
    config.network.ip = loadIp(record.ip);
    config.network.mask = loadIp(record.mask);
    config.network.gateway = loadIp(record.gateway);
    config.network.dns = loadIp(record.dns);
    if (!loadText(config.time.server, record.ntp_server) ||
        !loadText(config.time.timezone, record.timezone) ||
        !loadText(config.auth.username, record.username) ||
        !loadText(config.auth.salted_password_kdf, record.password_kdf)) return Result::Invalid;
    config.auth.session_lifetime_s = record.session_lifetime_s;
    config.auth.onboarding_required = false;
    config.automatic_control_enabled = record.automatic_control_enabled != 0;
    config.startup_observe_only_s = record.startup_observe_only_s;
    return Result::Accepted;
}
} // namespace fl
