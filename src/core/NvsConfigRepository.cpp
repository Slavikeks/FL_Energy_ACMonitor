#include "fl/core/ConfigRepository.h"
#include "fl/rotation/GroupCodec.h"
#if defined(ARDUINO_ARCH_ESP32)
#include <Preferences.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>
#endif

namespace fl {
#if defined(ARDUINO_ARCH_ESP32)
namespace {
std::uint32_t registryCrc(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
#pragma pack(push, 1)
struct StoredManagement {
    char magic[4]; std::uint16_t version; std::uint16_t length; std::uint32_t crc;
    std::uint8_t snmp_enabled; char community[64], system_name[97], location[97], contact[97];
    std::uint8_t receiver_enabled, receiver[4]; std::uint16_t trap_port;
    std::uint8_t mqtt_enabled; char broker[128]; std::uint16_t mqtt_port;
    char username[64], password[128], topic_prefix[97]; std::uint32_t publish_period_s;
};
#pragma pack(pop)
static_assert(sizeof(StoredManagement) < 1024, "Management config must fit in one NVS value");
template <std::size_t N> bool saveText(char (&out)[N], std::string_view value) {
    if (value.size() >= N || value.find('\0') != std::string_view::npos) return false;
    if (!value.empty()) std::memcpy(out, value.data(), value.size());
    return true;
}
template <std::size_t N, std::size_t M> bool loadText(Text<M>& out, const char (&in)[N]) {
    const auto* end = static_cast<const char*>(std::memchr(in, '\0', N));
    return end && out.assign(std::string_view(in, static_cast<std::size_t>(end - in)));
}
std::uint32_t managementCrc(const std::uint8_t* data, std::size_t size) {
    return registryCrc(data, size);
}
Result loadManagement(Preferences& preferences, Config& config) {
    const auto length = preferences.getBytesLength("management");
    if (!length) return Result::NotFound;
    if (length != sizeof(StoredManagement)) return Result::Invalid;
    StoredManagement stored{};
    if (preferences.getBytes("management", &stored, sizeof(stored)) != sizeof(stored)) return Result::IoError;
    if (std::memcmp(stored.magic, "FLM1", 4) != 0 || stored.version != 1 || stored.length != sizeof(stored) ||
        stored.snmp_enabled > 1 || stored.receiver_enabled > 1 || stored.mqtt_enabled > 1 ||
        managementCrc(reinterpret_cast<const std::uint8_t*>(&stored) + offsetof(StoredManagement, snmp_enabled),
                      sizeof(stored) - offsetof(StoredManagement, snmp_enabled)) != stored.crc) return Result::Invalid;
    config.snmp.enabled = stored.snmp_enabled != 0; config.snmp.trap_port = stored.trap_port;
    config.snmp.trap_receiver.reset(); if (stored.receiver_enabled) {
        Ipv4 ip; std::copy(stored.receiver, stored.receiver + 4, ip.octets.begin()); config.snmp.trap_receiver = ip;
    }
    config.mqtt.enabled = stored.mqtt_enabled != 0; config.mqtt.port = stored.mqtt_port;
    config.mqtt.publish_period_s = stored.publish_period_s;
    if (!loadText(config.snmp.ro_community, stored.community) || !loadText(config.snmp.system_name, stored.system_name) ||
        !loadText(config.snmp.location, stored.location) || !loadText(config.snmp.contact, stored.contact) ||
        !loadText(config.mqtt.broker, stored.broker) || !loadText(config.mqtt.username, stored.username) ||
        !loadText(config.mqtt.password, stored.password) || !loadText(config.mqtt.topic_prefix, stored.topic_prefix) ||
        stored.trap_port == 0 || stored.mqtt_port == 0 || stored.publish_period_s < 5 || stored.publish_period_s > 3600)
        return Result::Invalid;
    return Result::Accepted;
}
Result saveManagement(Preferences& preferences, const Config& config) {
    StoredManagement stored{}; std::memcpy(stored.magic, "FLM1", 4); stored.version = 1;
    stored.length = sizeof(stored); stored.snmp_enabled = config.snmp.enabled ? 1 : 0;
    stored.receiver_enabled = config.snmp.trap_receiver ? 1 : 0;
    if (config.snmp.trap_receiver) std::copy(config.snmp.trap_receiver->octets.begin(), config.snmp.trap_receiver->octets.end(), stored.receiver);
    stored.trap_port = config.snmp.trap_port; stored.mqtt_enabled = config.mqtt.enabled ? 1 : 0;
    stored.mqtt_port = config.mqtt.port; stored.publish_period_s = config.mqtt.publish_period_s;
    if (!saveText(stored.community, config.snmp.ro_community.view()) || !saveText(stored.system_name, config.snmp.system_name.view()) ||
        !saveText(stored.location, config.snmp.location.view()) || !saveText(stored.contact, config.snmp.contact.view()) ||
        !saveText(stored.broker, config.mqtt.broker.view()) || !saveText(stored.username, config.mqtt.username.view()) ||
        !saveText(stored.password, config.mqtt.password.view()) || !saveText(stored.topic_prefix, config.mqtt.topic_prefix.view())) return Result::Invalid;
    stored.crc = managementCrc(reinterpret_cast<const std::uint8_t*>(&stored) + offsetof(StoredManagement, snmp_enabled),
                               sizeof(stored) - offsetof(StoredManagement, snmp_enabled));
    return preferences.putBytes("management", &stored, sizeof(stored)) == sizeof(stored) ? Result::Accepted : Result::IoError;
}
}
Result NvsConfigRepository::load(Config& config) {
    Preferences preferences;
    if (!preferences.begin("flenergy", true)) return Result::IoError;
    const std::size_t length = preferences.getBytesLength("settings");
    if (length == 0) { preferences.end(); return Result::NotFound; }
    if (length > kPersistentConfigMaxBytes) { preferences.end(); return Result::Invalid; }
    std::uint8_t bytes[kPersistentConfigMaxBytes]{};
    if (preferences.getBytes("settings", bytes, length) != length) {
        preferences.end();
        return Result::IoError;
    }
    preferences.end();
    const auto decoded = decodePersistentConfig(bytes, length, config);
    if (decoded != Result::Accepted) return decoded;
    if (!preferences.begin("flenergy", true)) return Result::IoError;
    const auto management = loadManagement(preferences, config);
    preferences.end();
    return management == Result::NotFound ? Result::Accepted : management;
}

Result NvsConfigRepository::save(const Config& config) {
    std::uint8_t bytes[kPersistentConfigMaxBytes]{};
    std::size_t length = 0;
    const auto encoded = encodePersistentConfig(config, bytes, sizeof(bytes), length);
    if (encoded != Result::Accepted) return encoded;
    Preferences preferences;
    if (!preferences.begin("flenergy", false)) return Result::IoError;
    const auto written = preferences.putBytes("settings", bytes, length);
    const auto management = written == length ? saveManagement(preferences, config) : Result::IoError;
    preferences.end();
    return written == length ? management : Result::IoError;
}
Result NvsConfigRepository::loadRegistry(Registry& registry) {
    Preferences preferences;
    if (!preferences.begin("flenergy", true)) return Result::IoError;
    const auto length = preferences.getBytesLength("registry");
    if (!length) { preferences.end(); return Result::NotFound; }
    if (length < 12 || length > 2048) { preferences.end(); return Result::Invalid; }
    std::vector<std::uint8_t> bytes(length);
    if (preferences.getBytes("registry", bytes.data(), length) != length) { preferences.end(); return Result::IoError; }
    preferences.end();
    if (std::memcmp(bytes.data(), "FLR1", 4) != 0 || bytes[4] != 1 || bytes[5] > kMaxAc) return Result::Invalid;
    const std::uint32_t stored = std::uint32_t(bytes[length - 4]) | (std::uint32_t(bytes[length - 3]) << 8) |
        (std::uint32_t(bytes[length - 2]) << 16) | (std::uint32_t(bytes[length - 1]) << 24);
    if (registryCrc(bytes.data(), length - 4) != stored) return Result::Invalid;
    Registry decoded;
    std::size_t offset = 6;
    for (std::size_t i = 0; i < bytes[5]; ++i) {
        if (offset + 8 > length - 4) return Result::Invalid;
        AcRegistration item;
        item.address = bytes[offset++]; const auto flags = bytes[offset++];
        item.maintenance = (flags & 1) != 0; item.model_validated = (flags & 2) != 0;
        item.min_setpoint_deci_c = static_cast<std::int16_t>(bytes[offset] | (std::uint16_t(bytes[offset + 1]) << 8)); offset += 2;
        item.max_setpoint_deci_c = static_cast<std::int16_t>(bytes[offset] | (std::uint16_t(bytes[offset + 1]) << 8)); offset += 2;
        const auto name_length = bytes[offset++];
        if (offset + name_length > length - 4 || !validAddress(item.address) || name_length == 0 ||
            item.min_setpoint_deci_c >= item.max_setpoint_deci_c ||
            !item.name.assign(std::string_view(reinterpret_cast<const char*>(bytes.data() + offset), name_length))) return Result::Invalid;
        offset += name_length;
        for (const auto& prior : decoded) if (prior.address == item.address) return Result::Invalid;
        if (!decoded.push_back(item)) return Result::Full;
    }
    if (offset != length - 4) return Result::Invalid;
    registry = decoded;
    return Result::Accepted;
}
Result NvsConfigRepository::saveRegistry(const Registry& registry) {
    if (registry.size() > kMaxAc) return Result::Full;
    std::vector<std::uint8_t> bytes{'F','L','R','1',1,static_cast<std::uint8_t>(registry.size())};
    for (std::size_t i = 0; i < registry.size(); ++i) {
        const auto& item = registry[i];
        if (!validAddress(item.address) || item.name.view().empty() || item.name.view().size() > 96 ||
            item.min_setpoint_deci_c >= item.max_setpoint_deci_c) return Result::Invalid;
        for (std::size_t j = 0; j < i; ++j) if (registry[j].address == item.address) return Result::Invalid;
        bytes.push_back(item.address); bytes.push_back((item.maintenance ? 1 : 0) | (item.model_validated ? 2 : 0));
        const auto min = static_cast<std::uint16_t>(item.min_setpoint_deci_c);
        const auto max = static_cast<std::uint16_t>(item.max_setpoint_deci_c);
        bytes.push_back(static_cast<std::uint8_t>(min)); bytes.push_back(static_cast<std::uint8_t>(min >> 8));
        bytes.push_back(static_cast<std::uint8_t>(max)); bytes.push_back(static_cast<std::uint8_t>(max >> 8));
        bytes.push_back(static_cast<std::uint8_t>(item.name.view().size()));
        bytes.insert(bytes.end(), item.name.view().begin(), item.name.view().end());
    }
    const auto crc = registryCrc(bytes.data(), bytes.size());
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
    Preferences preferences;
    if (!preferences.begin("flenergy", false)) return Result::IoError;
    const auto written = preferences.putBytes("registry", bytes.data(), bytes.size());
    preferences.end();
    return written == bytes.size() ? Result::Accepted : Result::IoError;
}
Result NvsConfigRepository::loadGroups(List<GroupConfig, kMaxGroups>& groups) {
    Preferences preferences;
    if (!preferences.begin("flenergy", true)) return Result::IoError;
    const auto length = preferences.getBytesLength("groups");
    if (!length) { preferences.end(); return Result::NotFound; }
    if (length > kGroupStorageMaxBytes) { preferences.end(); return Result::Invalid; }
    std::vector<std::uint8_t> bytes(length);
    if (preferences.getBytes("groups", bytes.data(), length) != length) { preferences.end(); return Result::IoError; }
    preferences.end();
    return decodeGroups(bytes.data(), bytes.size(), groups);
}
Result NvsConfigRepository::saveGroups(const List<GroupConfig, kMaxGroups>& groups) {
    std::vector<std::uint8_t> bytes(kGroupStorageMaxBytes);
    std::size_t length = 0;
    const auto encoded = encodeGroups(groups, bytes.data(), bytes.size(), length);
    if (encoded != Result::Accepted) return encoded;
    Preferences preferences;
    if (!preferences.begin("flenergy", false)) return Result::IoError;
    const auto written = preferences.putBytes("groups", bytes.data(), length);
    preferences.end();
    return written == length ? Result::Accepted : Result::IoError;
}
#else
Result NvsConfigRepository::load(Config&) { return Result::NotImplemented; }
Result NvsConfigRepository::save(const Config&) { return Result::NotImplemented; }
Result NvsConfigRepository::loadRegistry(Registry&) { return Result::NotImplemented; }
Result NvsConfigRepository::saveRegistry(const Registry&) { return Result::NotImplemented; }
Result NvsConfigRepository::loadGroups(List<GroupConfig, kMaxGroups>&) { return Result::NotImplemented; }
Result NvsConfigRepository::saveGroups(const List<GroupConfig, kMaxGroups>&) { return Result::NotImplemented; }
#endif
} // namespace fl
