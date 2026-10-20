#include <unity.h>
#include "fl/core/Application.h"
#include "fl/core/AutomationHealth.h"
#include "fl/auth/Auth.h"
#include "fl/core/EventLog.h"
#include "fl/network/Network.h"
#include "fl/web/Web.h"
#include "fl/xye/Bus.h"
#include "fl/xye/XyeBusManager.h"
#include "fl/rotation/RotationEngine.h"
#include "fl/rotation/GroupCodec.h"
#include "fl/alarm/AlarmManager.h"
#include "fl/snmp/Snmp.h"
#include "fl/snmp/TrapText.h"
#include "fl/mqtt/Mqtt.h"
#include "fl/mqtt/Telemetry.h"
#include "fl/ota/Ota.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <type_traits>
#include <vector>

void setUp() {}
void tearDown() {}
void test_unknown_is_not_zero_or_confirmed() {
    fl::ObservedState observed;
    TEST_ASSERT_FALSE(observed.t1_deci_c.value.has_value());
    TEST_ASSERT_FALSE(observed.power.value.has_value());
    TEST_ASSERT_FALSE(observed.error_flags.value.has_value());
    TEST_ASSERT_FALSE(observed.compressor_running.confirmed());
    observed.compressor_running.value = true;
    observed.compressor_running.status = fl::ValueStatus::Experimental;
    TEST_ASSERT_FALSE(observed.compressor_running.confirmed());
    observed.t1_deci_c.value = 0; // Real 0 C is different from unknown.
    observed.t1_deci_c.status = fl::ValueStatus::Valid;
    TEST_ASSERT_TRUE(observed.t1_deci_c.confirmed());
}
void test_safe_defaults_and_empty_registry() {
    fl::Config config;
    fl::StateStore store;
    fl::GroupConfig group;
    TEST_ASSERT_FALSE(config.automatic_control_enabled);
    TEST_ASSERT_FALSE(config.mqtt.enabled);
    TEST_ASSERT_FALSE(config.auth.onboarding_required);
    TEST_ASSERT_EQUAL_UINT(0, config.registry.size());
    TEST_ASSERT_EQUAL_UINT(0, store.read().conditioners.size());
    TEST_ASSERT_TRUE(store.read().observe_only);
    TEST_ASSERT_FALSE(store.read().clock.epoch.has_value());
    TEST_ASSERT_TRUE(group.mode == fl::GroupMode::Manual);
    TEST_ASSERT_FALSE(group.rotation_enabled);
}
void test_persistent_config_roundtrip_without_full_config_temporary() {
    fl::Config source; source.network.dhcp = true;
    source.time.server.assign("ntp.example"); source.time.timezone.assign("UTC0");
    source.auth.username.assign("operator"); source.auth.salted_password_kdf.assign("v1$test");
    std::uint8_t bytes[fl::kPersistentConfigMaxBytes]{}; std::size_t written = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::encodePersistentConfig(source, bytes, sizeof(bytes), written)));
    fl::Config decoded;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::decodePersistentConfig(bytes, written, decoded)));
    TEST_ASSERT_TRUE(decoded.network.dhcp); TEST_ASSERT_EQUAL_STRING("ntp.example", decoded.time.server.c_str());
    TEST_ASSERT_EQUAL_STRING("UTC0", decoded.time.timezone.c_str());
    TEST_ASSERT_EQUAL_STRING("operator", decoded.auth.username.c_str());
    TEST_ASSERT_EQUAL_STRING("v1$test", decoded.auth.salted_password_kdf.c_str());
    const auto previous_server = decoded.time.server;
    bytes[written - 1] ^= 1;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(fl::decodePersistentConfig(bytes, written, decoded)));
    TEST_ASSERT_EQUAL_STRING(previous_server.c_str(), decoded.time.server.c_str());
}
void test_bounded_registry_1_4_15_16() {
    for (std::size_t count : {1U, 4U, 15U, 16U}) {
        fl::Registry registry;
        for (std::size_t i = 0; i < count; ++i) {
            fl::AcRegistration ac;
            ac.address = static_cast<fl::Address>(i);
            TEST_ASSERT_TRUE(registry.push_back(ac));
        }
        TEST_ASSERT_EQUAL_UINT(count, registry.size());
        TEST_ASSERT_EQUAL_UINT(count - 1, registry[count - 1].address);
        if (count == 16) TEST_ASSERT_FALSE(registry.push_back({}));
        registry.clear();
        TEST_ASSERT_EQUAL_UINT(0, registry.size());
    }
    TEST_ASSERT_TRUE(fl::validAddress(0));
    TEST_ASSERT_TRUE(fl::validAddress(63));
    TEST_ASSERT_FALSE(fl::validAddress(64));
    TEST_ASSERT_FALSE(fl::validAddress(255));
}
void test_utf8_names_and_rejected_overflow() {
    fl::Text<8> text;
    TEST_ASSERT_TRUE(text.assign(u8"Зал"));
    TEST_ASSERT_EQUAL_STRING(u8"Зал", text.c_str());
    TEST_ASSERT_FALSE(text.assign("123456789"));
    TEST_ASSERT_EQUAL_STRING(u8"Зал", text.c_str());
    TEST_ASSERT_FALSE(text.assign(std::string_view{"ab\0cd", 5}));
    TEST_ASSERT_TRUE(text.assign("12345678"));
    TEST_ASSERT_EQUAL_UINT(8, text.view().size());
}
void test_snmp_trap_text_is_ascii_and_transliterates_cyrillic() {
    char output[64]{};
    TEST_ASSERT_EQUAL_UINT(11, fl::toSnmpDisplayText(u8"Группа Ёж", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("Gruppa Yozh", output);
    char small[5]{};
    TEST_ASSERT_EQUAL_UINT(4, fl::toSnmpDisplayText(u8"Щука", small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("Shch", small);
    TEST_ASSERT_EQUAL_UINT(4, fl::toSnmpDisplayText(u8"Яя", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("Yaya", output);
}
void test_desired_does_not_change_observed() {
    fl::AcState ac;
    ac.desired.settings.power = true;
    ac.desired.command_state = fl::CommandState::Pending;
    TEST_ASSERT_FALSE(ac.observed.power.value.has_value());
    TEST_ASSERT_TRUE(ac.availability == fl::Availability::Unknown);
    TEST_ASSERT_TRUE(ac.freshness == fl::Freshness::NeverSeen);
}
void test_xye_query_frame_has_documented_layout_and_checksum() {
    auto& codec = fl::defaultXyeCodec();
    fl::TxFrame tx{};
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(codec.encodeQuery(15, tx)));
    TEST_ASSERT_EQUAL_HEX8(0xAA, tx[0]); TEST_ASSERT_EQUAL_HEX8(0xC0, tx[1]);
    TEST_ASSERT_EQUAL_HEX8(15, tx[2]); TEST_ASSERT_EQUAL_HEX8(0x80, tx[4]);
    TEST_ASSERT_EQUAL_HEX8(0x3F, tx[13]); TEST_ASSERT_EQUAL_HEX8(0x55, tx[15]);
    unsigned sum = 0; for (std::size_t i = 0; i < tx.size(); ++i) if (i != 14) sum += tx[i];
    TEST_ASSERT_EQUAL_HEX8(0xFFU - (sum & 0xFFU), tx[14]);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(codec.encodeQuery(64, tx)));
    fl::ControlRequest command; command.address = 15; command.settings.power = true;
    command.settings.mode = fl::Mode::Cool; command.settings.fan = fl::Fan::High;
    command.settings.setpoint_deci_c = 240;
    fl::TxFrame set{};
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(codec.encodeSet(command, set)));
    TEST_ASSERT_EQUAL_HEX8(0xAA, set[0]); TEST_ASSERT_EQUAL_HEX8(0xC3, set[1]);
    TEST_ASSERT_EQUAL_HEX8(15, set[2]); TEST_ASSERT_EQUAL_HEX8(0x88, set[6]);
    TEST_ASSERT_EQUAL_HEX8(0x01, set[7]); TEST_ASSERT_EQUAL_HEX8(24, set[8]);
    TEST_ASSERT_EQUAL_HEX8(0x3C, set[13]); TEST_ASSERT_EQUAL_HEX8(0x55, set[15]);
    sum = 0; for (std::size_t i = 0; i < set.size(); ++i) if (i != 14) sum += set[i];
    TEST_ASSERT_EQUAL_HEX8(0xFFU - (sum & 0xFFU), set[14]);
    command.settings.setpoint_deci_c = 245;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(codec.encodeSet(command, set)));
    command.settings.setpoint_deci_c = 240;
    command.settings.power = false; command.settings.mode.reset(); command.settings.fan.reset(); command.settings.setpoint_deci_c.reset();
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(codec.encodeSet(command, set)));
    TEST_ASSERT_EQUAL_HEX8(0x00, set[6]); TEST_ASSERT_EQUAL_HEX8(0x00, set[7]); TEST_ASSERT_EQUAL_HEX8(0xFF, set[8]);
    command.settings.power = true; command.settings.mode = fl::Mode::Fan;
    command.settings.fan = fl::Fan::Auto;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(codec.encodeSet(command, set)));
    TEST_ASSERT_EQUAL_HEX8(0x81, set[6]); TEST_ASSERT_EQUAL_HEX8(0x80, set[7]); TEST_ASSERT_EQUAL_HEX8(0xFF, set[8]);
    command.settings.setpoint_deci_c = 220;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(codec.encodeSet(command, set)));
}
void test_xye_homeops_captured_response_and_reject_corruption() {
    // HomeOps captured C0 response for source address 1; checksum includes 0x55.
    const fl::RxFrame rx{{0xAA,0xC0,0x80,0x00,0x01,0x00,0xE0,0x14,0x00,0x00,0x18,0x5C,0x34,0x36,0xFF,0xFF,
                         0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0A,0xFF,0xFF,0xE6,0x55}};
    auto& codec = fl::defaultXyeCodec(); fl::ObservedState observed;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(codec.decode(rx, 1, fl::XyeCommand::Query, observed)));
    TEST_ASSERT_TRUE(observed.power.value.has_value()); TEST_ASSERT_FALSE(observed.power.value.value());
    TEST_ASSERT_TRUE(observed.mode.status == fl::ValueStatus::Unsupported);
    TEST_ASSERT_EQUAL_INT(240, observed.setpoint_deci_c.value.value_or(-1));
    TEST_ASSERT_TRUE(observed.setpoint_deci_c.status == fl::ValueStatus::Experimental);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(fl::FanSpeed::Stopped), static_cast<int>(observed.fan_actual.value.value_or(fl::FanSpeed::High)));
    TEST_ASSERT_TRUE(observed.fan_auto.value.has_value()); TEST_ASSERT_FALSE(*observed.fan_auto.value);
    TEST_ASSERT_TRUE(observed.compressor_running.value.has_value()); TEST_ASSERT_TRUE(*observed.compressor_running.value);
    TEST_ASSERT_EQUAL_INT(60, observed.t2_deci_c.value.value_or(-1));
    TEST_ASSERT_EQUAL_INT(70, observed.t2b_deci_c.value.value_or(-1));
    TEST_ASSERT_TRUE(observed.t3_deci_c.status == fl::ValueStatus::Unsupported);
    auto variant = rx; variant[9] = 0x83; variant[19] = 0; variant[22] = 0x34; variant[23] = 0x12;
    variant[24] = 0x02; variant[25] = 0x80;
    unsigned variant_sum = 0; for (std::size_t i = 0; i < variant.size(); ++i) if (i != 30) variant_sum += variant[i];
    variant[30] = static_cast<std::uint8_t>(0xFFU - (variant_sum & 0xFFU));
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(codec.decode(variant, 1, fl::XyeCommand::Query, observed)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(fl::FanSpeed::Low), static_cast<int>(observed.fan_actual.value.value_or(fl::FanSpeed::High)));
    TEST_ASSERT_TRUE(observed.fan_auto.value.has_value()); TEST_ASSERT_TRUE(*observed.fan_auto.value);
    TEST_ASSERT_TRUE(observed.compressor_running.value.has_value()); TEST_ASSERT_FALSE(*observed.compressor_running.value);
    TEST_ASSERT_EQUAL_UINT16(0x1234, observed.error_flags.value.value_or(0));
    TEST_ASSERT_EQUAL_UINT16(0x8002, observed.protection_flags.value.value_or(0));
    auto auto_fan = rx; auto_fan[9] = 0x80;
    unsigned auto_sum = 0; for (std::size_t i = 0; i < auto_fan.size(); ++i) if (i != 30) auto_sum += auto_fan[i];
    auto_fan[30] = static_cast<std::uint8_t>(0xFFU - (auto_sum & 0xFFU));
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(codec.decode(auto_fan, 1, fl::XyeCommand::Query, observed)));
    TEST_ASSERT_FALSE(observed.fan_actual.value.has_value()); TEST_ASSERT_TRUE(*observed.fan_auto.value);
    auto corrupt = rx; corrupt[10] ^= 1;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(codec.decode(corrupt, 1, fl::XyeCommand::Query, observed)));
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(codec.decode(rx, 2, fl::XyeCommand::Query, observed)));
    auto sentinel = rx; sentinel[10] = 0xFF;
    unsigned sum = 0; for (std::size_t i = 0; i < sentinel.size(); ++i) if (i != 30) sum += sentinel[i];
    sentinel[30] = static_cast<std::uint8_t>(0xFFU - (sum & 0xFFU));
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(codec.decode(sentinel, 1, fl::XyeCommand::Query, observed)));
    TEST_ASSERT_TRUE(observed.setpoint_deci_c.status == fl::ValueStatus::Unsupported);
}
class SimTransport final : public fl::IRs485Transport {
public:
    bool online{true}, ignore_set{false}; unsigned stale_queries_after_set{0}, queries{0}, sets{0};
    std::uint8_t mode{0x88}, fan{0x80}, temperature{28};
    std::uint8_t pending_mode{0}, pending_fan{0}, pending_temperature{0}; bool pending_set{false};
    fl::RxFrame response{}; std::size_t offset{0};
    fl::Result begin() override { return fl::Result::Accepted; }
    std::size_t write(const std::uint8_t* bytes, std::size_t size) override {
        if (size != 16 || (bytes[1] != 0xC0 && bytes[1] != 0xC3)) return 0;
        if (bytes[1] == 0xC0) ++queries;
        else {
            ++sets;
            if (!ignore_set) {
                pending_mode = bytes[6]; pending_fan = bytes[7]; pending_temperature = bytes[8];
                pending_set = stale_queries_after_set != 0;
                if (!pending_set) { mode = pending_mode; fan = pending_fan; temperature = pending_temperature; }
            }
        }
        if (!online) return size;
        response = {{0xAA,bytes[1],0x80,0x00,bytes[2],0x00,0xE0,0x14,mode,fan,temperature,0x5C,0x34,0x36,0xFF,0xFF,
                    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0A,0xFF,0xFF,0x00,0x55}};
        unsigned sum = 0; for (std::size_t i = 0; i < response.size(); ++i) if (i != 30) sum += response[i];
        response[30] = static_cast<std::uint8_t>(0xFFU - (sum & 0xFFU)); offset = 0;
        if (bytes[1] == 0xC0 && pending_set && stale_queries_after_set) {
            if (--stale_queries_after_set == 0) {
                mode = pending_mode; fan = pending_fan; temperature = pending_temperature; pending_set = false;
            }
        }
        return size;
    }
    std::size_t read(std::uint8_t* out, std::size_t cap) override {
        if (!online || offset >= response.size()) return 0;
        const auto count = std::min(cap, response.size() - offset);
        std::copy(response.begin() + offset, response.begin() + offset + count, out); offset += count; return count;
    }
    bool txIdle() const override { return true; }
};
class SimRepository final : public fl::IConfigRepository {
public:
    fl::Registry stored;
    fl::List<fl::GroupConfig, fl::kMaxGroups> stored_groups;
    fl::Result load(fl::Config&) override { return fl::Result::NotFound; }
    fl::Result save(const fl::Config&) override { return fl::Result::Accepted; }
    fl::Result saveRegistry(const fl::Registry& r) override { stored = r; return fl::Result::Accepted; }
    fl::Result saveGroups(const fl::List<fl::GroupConfig, fl::kMaxGroups>& g) override { stored_groups = g; return fl::Result::Accepted; }
};
class CaptureCommands final : public fl::ICommandSink {
public:
    std::vector<fl::ControlRequest> requests;
    fl::Result enqueue(const fl::ControlRequest& request) override { requests.push_back(request); return fl::Result::Accepted; }
};
fl::GroupConfig testGroup(fl::GroupId id, std::size_t count, std::uint8_t required, std::uint8_t maximum) {
    fl::GroupConfig group; group.id = id; group.name.assign("test group");
    group.mode = fl::GroupMode::Auto; group.required_active = required; group.maximum_active = maximum;
    for (std::size_t i = 0; i < count; ++i) group.members.push_back(static_cast<fl::Address>(i));
    return group;
}
void addValidatedUnits(fl::Config& config, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        fl::AcRegistration item; item.address = static_cast<fl::Address>(i);
        item.name.assign("sim"); item.model_validated = true; config.registry.push_back(item);
    }
}
void setUnit(fl::AcState& ac, fl::Address address, bool on, fl::MonotonicMs now) {
    ac.address = address; ac.model_validated = true; ac.availability = fl::Availability::Online;
    ac.freshness = fl::Freshness::Fresh; ac.last_seen_ms = now;
    ac.observed.power.value = on; ac.observed.power.status = fl::ValueStatus::Valid;
    ac.observed.power.updated_ms = now;
    ac.observed.t1_deci_c.value = 220; ac.observed.t1_deci_c.status = fl::ValueStatus::Valid;
    ac.observed.t1_deci_c.updated_ms = now;
}
void test_group_n_of_m_validation_and_storage_codec() {
    for (const auto shape : {std::array<std::uint8_t, 3>{1, 2, 2}, {2, 3, 3}, {3, 5, 5}}) {
        fl::Config config; addValidatedUnits(config, shape[1]);
        fl::List<fl::GroupConfig, fl::kMaxGroups> groups;
        auto group = testGroup(static_cast<fl::GroupId>(shape[0]), shape[1], shape[0], shape[2]);
        group.rotation_enabled = true; groups.push_back(group);
        groups[0].ventilation_reserve = true;
        groups[0].color_rgb = 0x18A7D4;
        TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::validateGroupSet(config, groups)));
        std::uint8_t bytes[fl::kGroupStorageMaxBytes]{}; std::size_t written = 0;
        TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::encodeGroups(groups, bytes, sizeof(bytes), written)));
        fl::List<fl::GroupConfig, fl::kMaxGroups> decoded;
        TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::decodeGroups(bytes, written, decoded)));
        TEST_ASSERT_EQUAL_UINT(1, decoded.size());
        TEST_ASSERT_EQUAL_UINT(shape[1], decoded[0].members.size());
        TEST_ASSERT_EQUAL_UINT(shape[0], decoded[0].required_active);
        TEST_ASSERT_TRUE(decoded[0].ventilation_reserve);
        TEST_ASSERT_EQUAL_HEX32(0x18A7D4, decoded[0].color_rgb);

        // Simulate the previous persistent format (version 1 had no color bytes
        // and did not define the ventilation flag) to protect existing NVS data.
        std::uint8_t legacy[fl::kGroupStorageMaxBytes]{};
        std::copy(bytes, bytes + 48, legacy);
        std::copy(bytes + 51, bytes + written - 4, legacy + 48);
        legacy[4] = 1;
        const std::size_t legacy_length = written - 3;
        std::uint32_t crc = 0xFFFFFFFFU;
        for (std::size_t i = 0; i < legacy_length - 4; ++i) {
            crc ^= legacy[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
        crc = ~crc;
        for (unsigned i = 0; i < 4; ++i)
            legacy[legacy_length - 4 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
        fl::List<fl::GroupConfig, fl::kMaxGroups> legacy_decoded;
        TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted),
            static_cast<int>(fl::decodeGroups(legacy, legacy_length, legacy_decoded)));
        TEST_ASSERT_FALSE(legacy_decoded[0].ventilation_reserve);
        TEST_ASSERT_EQUAL_HEX32(0x5A9EBB, legacy_decoded[0].color_rgb);
    }
    fl::Config config; addValidatedUnits(config, 2);
    fl::List<fl::GroupConfig, fl::kMaxGroups> invalid; invalid.push_back(testGroup(1, 2, 3, 2));
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(fl::validateGroupSet(config, invalid)));
    invalid[0] = testGroup(1, 2, 1, 2); invalid[0].ac_mode = fl::Mode::Fan;
    invalid[0].ventilation_reserve = true;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(fl::validateGroupSet(config, invalid)));
}
void test_group_rotation_allows_single_capacity_without_overlap() {
    fl::Config config; addValidatedUnits(config, 2);
    fl::List<fl::GroupConfig, fl::kMaxGroups> groups;
    auto group = testGroup(1, 2, 1, 1); group.rotation_enabled = true; group.handover_s = 0;
    groups.push_back(group);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::validateGroupSet(config, groups)));
    groups[0].handover_s = 1;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Invalid), static_cast<int>(fl::validateGroupSet(config, groups)));
}
void test_rotation_starts_reserve_verifies_then_stops_old_unit() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(7, 2, 1, 2); group.rotation_enabled = true;
    group.interval_days = 1; group.handover_s = 2; group.start_verification_s = 3;
    group.min_on_s = 1; group.min_off_s = 1;
    const auto now_epoch = static_cast<fl::EpochSeconds>(std::time(nullptr));
    std::tm local{}; const std::time_t raw = static_cast<std::time_t>(now_epoch);
#if defined(_WIN32)
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    group.rotation_hour = static_cast<std::uint8_t>(local.tm_hour);
    group.rotation_minute = static_cast<std::uint8_t>(local.tm_min);
    group.last_rotation_anchor = now_epoch - 86401;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState a, b; setUnit(a, 0, true, 10000); setUnit(b, 1, false, 10000);
    a.last_started_ms = 0; b.last_stopped_ms = 0;
    state.conditioners.push_back(a); state.conditioners.push_back(b);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000; clock.epoch = now_epoch;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_TRUE(state.groups[0].rotation_next_epoch.has_value());
    TEST_ASSERT_EQUAL_INT64(now_epoch, state.groups[0].rotation_next_epoch.value_or(0));
    TEST_ASSERT_TRUE(commands.requests[0].origin == fl::CommandOrigin::Rotation);
    TEST_ASSERT_TRUE(commands.requests[0].settings.power.value_or(false));
    TEST_ASSERT_EQUAL_UINT(7, commands.requests[0].group_id.value_or(0));
    TEST_ASSERT_EQUAL_UINT(0, state.groups[0].outgoing.value_or(63));

    auto& incoming = state.conditioners[1]; incoming.desired.transaction_id = commands.requests[0].transaction_id;
    incoming.desired.command_state = fl::CommandState::Acknowledged;
    incoming.observed.power.value = true; incoming.observed.power.status = fl::ValueStatus::Valid;
    incoming.last_seen_ms = 10100; incoming.observed.power.updated_ms = 10100;
    clock.uptime_ms = 10100; engine.tick(config, state, clock, commands);
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Overlap);
    state.conditioners[0].last_seen_ms = 12200; state.conditioners[0].observed.power.updated_ms = 12200;
    state.conditioners[1].last_seen_ms = 12200; state.conditioners[1].observed.power.updated_ms = 12200;
    clock.uptime_ms = 12200; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(2, commands.requests.size());
    TEST_ASSERT_FALSE(commands.requests[1].settings.power.value_or(true));
    TEST_ASSERT_EQUAL_UINT(0, commands.requests[1].address);
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Stop);
    state.conditioners[0].desired.transaction_id = commands.requests[1].transaction_id;
    state.conditioners[0].desired.command_state = fl::CommandState::Acknowledged;
    state.conditioners[0].observed.power.value = false;
    state.conditioners[0].observed.power.updated_ms = 12300;
    state.conditioners[0].last_seen_ms = 12300;
    clock.uptime_ms = 12300; engine.tick(config, state, clock, commands);
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Idle);
    TEST_ASSERT_EQUAL_UINT(1, repository.stored_groups.size());
    TEST_ASSERT_EQUAL_INT64(now_epoch, repository.stored_groups[0].last_rotation_anchor.value_or(0));
}
void test_rotation_without_overlap_stops_old_before_starting_reserve() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(8, 2, 1, 1); group.rotation_enabled = true;
    group.interval_days = 1; group.handover_s = 0; group.start_verification_s = 3;
    group.min_on_s = 1; group.min_off_s = 1;
    const auto now_epoch = static_cast<fl::EpochSeconds>(std::time(nullptr));
    std::tm local{}; const std::time_t raw = static_cast<std::time_t>(now_epoch);
#if defined(_WIN32)
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    group.rotation_hour = static_cast<std::uint8_t>(local.tm_hour);
    group.rotation_minute = static_cast<std::uint8_t>(local.tm_min);
    group.last_rotation_anchor = now_epoch - 86401;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState old_unit, reserve; setUnit(old_unit, 0, true, 10000); setUnit(reserve, 1, false, 10000);
    old_unit.last_started_ms = 0; reserve.last_stopped_ms = 0;
    state.conditioners.push_back(old_unit); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000; clock.epoch = now_epoch;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_FALSE(commands.requests[0].settings.power.value_or(true));
    TEST_ASSERT_EQUAL_UINT(0, commands.requests[0].address);
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Break);

    auto& stopped = state.conditioners[0];
    stopped.desired.transaction_id = commands.requests[0].transaction_id;
    stopped.desired.command_state = fl::CommandState::Acknowledged;
    stopped.observed.power.value = false; stopped.observed.power.updated_ms = 10100;
    stopped.last_seen_ms = 10100; stopped.last_stopped_ms = 10100;
    clock.uptime_ms = 10100; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(2, commands.requests.size());
    TEST_ASSERT_TRUE(commands.requests[1].settings.power.value_or(false));
    TEST_ASSERT_EQUAL_UINT(1, commands.requests[1].address);
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Start);

    auto& started = state.conditioners[1];
    started.desired.transaction_id = commands.requests[1].transaction_id;
    started.desired.command_state = fl::CommandState::Acknowledged;
    started.observed.power.value = true; started.observed.power.updated_ms = 10200;
    started.last_seen_ms = 10200;
    clock.uptime_ms = 10200; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(2, commands.requests.size());
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Idle);
    TEST_ASSERT_EQUAL_UINT(1, repository.stored_groups.size());
    TEST_ASSERT_EQUAL_INT64(now_epoch, repository.stored_groups[0].last_rotation_anchor.value_or(0));
}
void test_group_does_not_start_second_unit_below_high_temperature_threshold() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(10, 2, 1, 2); group.high_temp_deci_c = 280; group.high_delay_s = 1;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState active, reserve; setUnit(active, 0, true, 10000); setUnit(reserve, 1, false, 10000);
    active.observed.t1_deci_c.value = 250; reserve.observed.t1_deci_c.value = 240;
    active.last_started_ms = 0; reserve.last_stopped_ms = 0;
    state.conditioners.push_back(active); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000;
    engine.tick(config, state, clock, commands);
    clock.uptime_ms = 12000;
    state.conditioners[0].last_seen_ms = 12000; state.conditioners[1].last_seen_ms = 12000;
    state.conditioners[0].observed.t1_deci_c.updated_ms = 12000;
    state.conditioners[1].observed.t1_deci_c.updated_ms = 12000;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());
    TEST_ASSERT_FALSE(state.groups[0].emergency_operation);
    TEST_ASSERT_EQUAL_UINT(250, state.groups[0].temperature_deci_c.value.value_or(0));
}
void test_auto_group_restores_remote_mode_and_setpoint_changes() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(61, 2, 1, 2);
    group.ac_mode = fl::Mode::Cool; group.setpoint_deci_c = 220; group.fan = fl::Fan::Auto;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState active, reserve; setUnit(active, 0, true, 10000); setUnit(reserve, 1, false, 10000);
    active.observed.mode.value = fl::Mode::Heat; active.observed.mode.status = fl::ValueStatus::Valid;
    active.observed.setpoint_deci_c.value = 260; active.observed.setpoint_deci_c.status = fl::ValueStatus::Valid;
    active.observed.fan_auto.value = true; active.observed.fan_auto.status = fl::ValueStatus::Valid;
    state.conditioners.push_back(active); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_EQUAL_UINT(0, commands.requests[0].address);
    TEST_ASSERT_TRUE(commands.requests[0].origin == fl::CommandOrigin::Rotation);
    TEST_ASSERT_TRUE(commands.requests[0].settings.power.value_or(false));
    TEST_ASSERT_TRUE(commands.requests[0].settings.mode && *commands.requests[0].settings.mode == fl::Mode::Cool);
    TEST_ASSERT_TRUE(commands.requests[0].settings.fan && *commands.requests[0].settings.fan == fl::Fan::Auto);
    TEST_ASSERT_EQUAL_INT(220, commands.requests[0].settings.setpoint_deci_c.value_or(-1));
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Idle);
}
void test_ventilation_reserve_is_maintained_and_promoted_for_emergency() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(70, 2, 1, 2); group.ventilation_reserve = true;
    group.fan = fl::Fan::Medium; group.high_temp_deci_c = 280; group.high_delay_s = 1;
    group.min_off_s = 300;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState active, reserve; setUnit(active, 0, true, 10000); setUnit(reserve, 1, false, 10000);
    active.observed.mode.value = fl::Mode::Cool; active.observed.mode.status = fl::ValueStatus::Valid;
    active.observed.fan_auto.value = false; active.observed.fan_auto.status = fl::ValueStatus::Valid;
    active.observed.fan_actual.value = fl::FanSpeed::Medium; active.observed.fan_actual.status = fl::ValueStatus::Valid;
    active.last_started_ms = 0; reserve.last_stopped_ms = 9999;
    state.conditioners.push_back(active); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000;

    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_EQUAL_UINT(1, commands.requests[0].address);
    TEST_ASSERT_TRUE(commands.requests[0].settings.power.value_or(false));
    TEST_ASSERT_TRUE(commands.requests[0].settings.mode && *commands.requests[0].settings.mode == fl::Mode::Fan);
    TEST_ASSERT_TRUE(commands.requests[0].settings.fan && *commands.requests[0].settings.fan == fl::Fan::Medium);
    TEST_ASSERT_FALSE(commands.requests[0].settings.setpoint_deci_c.has_value());

    auto& standby = state.conditioners[1];
    standby.desired.transaction_id = commands.requests[0].transaction_id;
    standby.desired.command_state = fl::CommandState::Acknowledged;
    standby.observed.power.value = true; standby.observed.power.updated_ms = 10100;
    standby.observed.mode.value = fl::Mode::Fan; standby.observed.mode.status = fl::ValueStatus::Valid;
    standby.observed.mode.updated_ms = 10100;
    standby.observed.fan_auto.value = false; standby.observed.fan_auto.status = fl::ValueStatus::Valid;
    standby.observed.fan_actual.value = fl::FanSpeed::Medium; standby.observed.fan_actual.status = fl::ValueStatus::Valid;
    standby.observed.t1_deci_c.value = 285; standby.observed.t1_deci_c.updated_ms = 10100;
    standby.last_seen_ms = 10100;
    clock.uptime_ms = 10100; engine.tick(config, state, clock, commands);
    TEST_ASSERT_TRUE(state.groups[0].active_addresses.test(0));
    TEST_ASSERT_FALSE(state.groups[0].active_addresses.test(1));
    TEST_ASSERT_TRUE(state.groups[0].ventilation_addresses.test(1));
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());

    state.conditioners[0].observed.t1_deci_c.value = 285;
    state.conditioners[0].observed.t1_deci_c.updated_ms = 11100;
    state.conditioners[0].last_seen_ms = 11100;
    standby.observed.t1_deci_c.updated_ms = 11100; standby.last_seen_ms = 11100;
    clock.uptime_ms = 11100; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(2, commands.requests.size());
    TEST_ASSERT_EQUAL_UINT(1, commands.requests[1].address);
    TEST_ASSERT_TRUE(commands.requests[1].settings.power.value_or(false));
    TEST_ASSERT_TRUE(commands.requests[1].settings.mode && *commands.requests[1].settings.mode == fl::Mode::Cool);
    TEST_ASSERT_EQUAL_INT(220, commands.requests[1].settings.setpoint_deci_c.value_or(-1));
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Start);
}
void test_recovery_returns_extra_active_unit_to_ventilation() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(71, 2, 1, 2); group.ventilation_reserve = true;
    group.fan = fl::Fan::Low; group.recovery_temp_deci_c = 250; group.recovery_delay_s = 1;
    group.min_on_s = 1;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState first, second; setUnit(first, 0, true, 10000); setUnit(second, 1, true, 10000);
    for (auto* ac : {&first, &second}) {
        ac->observed.mode.value = fl::Mode::Cool; ac->observed.mode.status = fl::ValueStatus::Valid;
        ac->observed.fan_auto.value = false; ac->observed.fan_auto.status = fl::ValueStatus::Valid;
        ac->observed.fan_actual.value = fl::FanSpeed::Low; ac->observed.fan_actual.status = fl::ValueStatus::Valid;
        ac->observed.t1_deci_c.value = 220; ac->last_started_ms = 0;
    }
    state.conditioners.push_back(first); state.conditioners.push_back(second);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000;
    engine.tick(config, state, clock, commands);
    state.conditioners[0].last_seen_ms = 11000; state.conditioners[1].last_seen_ms = 11000;
    state.conditioners[0].observed.t1_deci_c.updated_ms = 11000;
    state.conditioners[1].observed.t1_deci_c.updated_ms = 11000;
    clock.uptime_ms = 11000; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_TRUE(commands.requests[0].settings.power.value_or(false));
    TEST_ASSERT_TRUE(commands.requests[0].settings.mode && *commands.requests[0].settings.mode == fl::Mode::Fan);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests[0].address);

    auto& demoted = state.conditioners[0];
    demoted.desired.transaction_id = commands.requests[0].transaction_id;
    demoted.desired.command_state = fl::CommandState::Acknowledged;
    demoted.observed.mode.value = fl::Mode::Fan; demoted.observed.mode.updated_ms = 12000;
    demoted.observed.power.value = true; demoted.observed.power.updated_ms = 12000;
    demoted.last_seen_ms = 12000;
    state.conditioners[1].last_seen_ms = 12000; state.conditioners[1].observed.power.updated_ms = 12000;
    clock.uptime_ms = 12000; engine.tick(config, state, clock, commands);
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Idle);
    TEST_ASSERT_FALSE(state.groups[0].active_addresses.test(0));
    TEST_ASSERT_TRUE(state.groups[0].ventilation_addresses.test(0));
    TEST_ASSERT_TRUE(demoted.last_stopped_ms.has_value());
}
void test_manual_controlled_auto_member_is_never_reconciled_or_replaced_over_limit() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2); config.registry[0].maintenance = true;
    auto group = testGroup(62, 2, 1, 1); config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState manual, reserve; setUnit(manual, 0, true, 10000); setUnit(reserve, 1, false, 10000);
    manual.maintenance = true;
    manual.observed.mode.value = fl::Mode::Heat; manual.observed.mode.status = fl::ValueStatus::Valid;
    manual.observed.setpoint_deci_c.value = 260; manual.observed.setpoint_deci_c.status = fl::ValueStatus::Valid;
    state.conditioners.push_back(manual); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());
    TEST_ASSERT_TRUE(state.groups[0].state == fl::GroupState::Degraded);
}
void test_rotation_keeps_running_unit_healthy_during_short_poll_gap() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    config.bus.response_timeout_ms = 100; config.bus.retry_limit = 1; config.bus.offline_failures = 3;
    addValidatedUnits(config, 2);
    auto group = testGroup(53, 2, 1, 2); config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState running, reserve;
    setUnit(running, 0, true, 10000);
    setUnit(reserve, 1, false, 13500);
    state.conditioners.push_back(running); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 13500;

    engine.tick(config, state, clock, commands);

    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());
    TEST_ASSERT_TRUE(state.groups[0].active_addresses.test(0));
    TEST_ASSERT_FALSE(state.groups[0].active_addresses.test(1));
    TEST_ASSERT_TRUE(fl::automationHealthIssue(config, state.conditioners[0], clock.uptime_ms).empty());
    TEST_ASSERT_TRUE(fl::automationHealthy(config, state.conditioners[0], clock.uptime_ms));
}
void test_unknown_member_blocks_replacement_until_three_failed_polls() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    config.bus.offline_failures = 3; addValidatedUnits(config, 2);
    auto group = testGroup(54, 2, 1, 2); group.min_off_s = 0; config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState lost, reserve; setUnit(lost, 0, true, 10000); setUnit(reserve, 1, false, 10000);
    lost.observed.power.status = fl::ValueStatus::Unknown;
    reserve.last_stopped_ms = 0; state.conditioners.push_back(lost); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 11000;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());

    state.conditioners[0].availability = fl::Availability::Offline;
    state.conditioners[0].freshness = fl::Freshness::Stale;
    state.conditioners[0].observed.power.value.reset();
    state.conditioners[0].observed.power.status = fl::ValueStatus::Unknown;
    state.conditioners[0].consecutive_failures = 3;
    clock.uptime_ms = 12000; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_TRUE(commands.requests[0].settings.power.value_or(false));
    TEST_ASSERT_EQUAL_UINT(1, commands.requests[0].address);
    TEST_ASSERT_NOT_NULL(std::strstr(state.groups[0].last_action_reason.c_str(), "3 consecutive failed polls"));
}
void test_known_fault_failover_stops_old_unit_before_reserve() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(55, 2, 1, 1); group.min_off_s = 0; config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState failed, reserve; setUnit(failed, 0, true, 10000); setUnit(reserve, 1, false, 10000);
    failed.observed.error_flags.value = 1; failed.observed.error_flags.status = fl::ValueStatus::Valid;
    reserve.last_stopped_ms = 0; state.conditioners.push_back(failed); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_EQUAL_UINT(0, commands.requests[0].address);
    TEST_ASSERT_FALSE(commands.requests[0].settings.power.value_or(true));
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Break);

    auto& stopped = state.conditioners[0];
    stopped.desired.transaction_id = commands.requests[0].transaction_id;
    stopped.desired.command_state = fl::CommandState::Acknowledged;
    stopped.observed.power.value = false; stopped.observed.power.status = fl::ValueStatus::Valid;
    stopped.observed.power.updated_ms = 10100; stopped.last_seen_ms = 10100;
    clock.uptime_ms = 10100; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(2, commands.requests.size());
    TEST_ASSERT_EQUAL_UINT(1, commands.requests[1].address);
    TEST_ASSERT_TRUE(commands.requests[1].settings.power.value_or(false));

    auto& started = state.conditioners[1];
    started.desired.transaction_id = commands.requests[1].transaction_id;
    started.desired.command_state = fl::CommandState::Acknowledged;
    started.observed.power.value = true; started.observed.power.status = fl::ValueStatus::Valid;
    started.observed.power.updated_ms = 10200; started.last_seen_ms = 10200;
    clock.uptime_ms = 10200; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(2, commands.requests.size());
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Idle);
    TEST_ASSERT_FALSE(state.conditioners[0].observed.power.value.value_or(true));
    TEST_ASSERT_TRUE(state.conditioners[1].observed.power.value.value_or(false));
    TEST_ASSERT_FALSE(state.groups[0].failover_break_before_make);
}
void test_single_unit_policy_does_not_expand_even_during_emergency() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(11, 2, 1, 1); group.high_temp_deci_c = 280; group.high_delay_s = 1;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState active, reserve; setUnit(active, 0, true, 10000); setUnit(reserve, 1, false, 10000);
    active.observed.t1_deci_c.value = 300; reserve.observed.t1_deci_c.value = 290;
    active.last_started_ms = 0; reserve.last_stopped_ms = 0;
    state.conditioners.push_back(active); state.conditioners.push_back(reserve);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000;
    engine.tick(config, state, clock, commands);
    clock.uptime_ms = 12000;
    state.conditioners[0].last_seen_ms = 12000; state.conditioners[1].last_seen_ms = 12000;
    state.conditioners[0].observed.t1_deci_c.updated_ms = 12000;
    state.conditioners[1].observed.t1_deci_c.updated_ms = 12000;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_TRUE(state.groups[0].emergency_operation);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());
}
void test_rotation_failed_start_never_stops_existing_unit() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(9, 2, 1, 2); group.high_temp_deci_c = 280; group.high_delay_s = 2; group.min_off_s = 1;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState a, b; setUnit(a, 0, true, 1000); setUnit(b, 1, false, 1000);
    a.observed.t1_deci_c.value = 300; a.last_started_ms = 0; b.last_stopped_ms = 0;
    state.conditioners.push_back(a); state.conditioners.push_back(b);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.epoch = 0; clock.uptime_ms = 1000;
    engine.tick(config, state, clock, commands);
    clock.uptime_ms = 3000; state.conditioners[1].last_seen_ms = 3000;
    state.conditioners[1].observed.power.updated_ms = 3000; state.conditioners[1].observed.t1_deci_c.updated_ms = 3000;
    state.conditioners[0].last_seen_ms = 3000; state.conditioners[0].observed.power.updated_ms = 3000;
    state.conditioners[0].observed.t1_deci_c.updated_ms = 3000;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_TRUE(commands.requests[0].settings.power.value_or(false));
    state.conditioners[1].desired.transaction_id = commands.requests[0].transaction_id;
    state.conditioners[1].desired.command_state = fl::CommandState::Failed;
    clock.uptime_ms = 3001; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_TRUE(state.conditioners[0].observed.power.value.value_or(false));
    TEST_ASSERT_TRUE(state.groups[0].phase == fl::HandoverPhase::Aborted);
}
void test_rotation_reconciles_1_of_2_2_of_3_and_3_of_5() {
    const std::array<std::array<std::uint8_t, 3>, 3> shapes{{{{1, 2, 2}}, {{2, 3, 3}}, {{3, 5, 5}}}};
    for (const auto shape : shapes) {
        fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
        addValidatedUnits(config, shape[1]);
        auto group = testGroup(21, shape[1], shape[0], shape[2]); group.min_off_s = 1; group.min_on_s = 1;
        config.groups.push_back(group);
        fl::RuntimeState state; state.observe_only = false;
        for (std::size_t i = 0; i < shape[1]; ++i) {
            fl::AcState ac; setUnit(ac, static_cast<fl::Address>(i), false, 10000);
            ac.last_stopped_ms = 0; state.conditioners.push_back(ac);
        }
        SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
        CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000;
        for (std::uint8_t active = 0; active < shape[0]; ++active) {
            engine.tick(config, state, clock, commands);
            TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(active) + 1U, commands.requests.size());
            const auto request = commands.requests.back();
            TEST_ASSERT_TRUE(request.settings.power.value_or(false));
            TEST_ASSERT_EQUAL_UINT(21, request.group_id.value_or(0));
            auto& ac = state.conditioners[request.address];
            ac.desired.transaction_id = request.transaction_id;
            ac.desired.command_state = fl::CommandState::Acknowledged;
            ac.observed.power.value = true; ac.observed.power.status = fl::ValueStatus::Valid;
            ac.observed.power.updated_ms = clock.uptime_ms; ac.last_seen_ms = clock.uptime_ms;
            clock.uptime_ms += 1;
        }
        engine.tick(config, state, clock, commands);
        TEST_ASSERT_EQUAL_UINT(shape[0], commands.requests.size());
        std::size_t active_count = 0;
        for (const auto& ac : state.conditioners) if (ac.observed.power.value.value_or(false)) ++active_count;
        TEST_ASSERT_EQUAL_UINT(shape[0], active_count);
    }
}
void test_rotation_waits_for_clock_and_sensors_and_never_fails_back() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(31, 2, 1, 2); group.mode = fl::GroupMode::Auto;
    group.rotation_enabled = true; group.interval_days = 1; group.last_rotation_anchor = 1;
    group.high_delay_s = 1; group.min_on_s = 1; group.min_off_s = 1;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState active, spare; setUnit(active, 0, true, 10000); setUnit(spare, 1, false, 10000);
    active.last_started_ms = 0; spare.last_stopped_ms = 0;
    // A numeric sentinel without a validated status must not trigger the T1 emergency.
    active.observed.t1_deci_c.value = 500; active.observed.t1_deci_c.status = fl::ValueStatus::Unknown;
    spare.observed.t1_deci_c.status = fl::ValueStatus::Unknown;
    state.conditioners.push_back(active); state.conditioners.push_back(spare);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000; // NTP time intentionally unavailable.
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());
    TEST_ASSERT_FALSE(state.groups[0].temperature_deci_c.value.has_value());

    // A reboot during an overlap loses transient phase state; surplus operation
    // is allowed to settle, but no immediate STOP occurs without recovery delay.
    auto& spare_after_reboot = state.conditioners[1];
    setUnit(spare_after_reboot, 1, true, 11000);
    auto& old_unit = state.conditioners[0];
    old_unit.availability = fl::Availability::Offline; old_unit.freshness = fl::Freshness::Stale;
    state.observe_only = true;
    fl::RotationEngine after_reboot(repository, events);
    clock.uptime_ms = 11000; after_reboot.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());
    TEST_ASSERT_TRUE(state.observe_only);

    // Once the old unit recovers while the replacement is carrying the load,
    // the engine does not switch back outside the configured rotation window.
    old_unit.availability = fl::Availability::Online; old_unit.freshness = fl::Freshness::Fresh;
    old_unit.last_seen_ms = 12000; old_unit.observed.power.value = false;
    old_unit.observed.power.status = fl::ValueStatus::Valid; old_unit.observed.power.updated_ms = 12000;
    spare_after_reboot.last_seen_ms = 12000; spare_after_reboot.observed.power.updated_ms = 12000;
    state.observe_only = false;
    clock.uptime_ms = 12000; after_reboot.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());

    // Manual groups remain manual even when the global master is enabled.
    config.groups[0].mode = fl::GroupMode::Manual;
    clock.uptime_ms = 13000; after_reboot.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(0, commands.requests.size());
    TEST_ASSERT_TRUE(state.groups[0].state == fl::GroupState::Manual);
}
void test_emergency_recovery_stop_does_not_advance_rotation_schedule() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.poll_interval_ms = 1000;
    addValidatedUnits(config, 2);
    auto group = testGroup(52, 2, 1, 2); group.recovery_delay_s = 1; group.min_on_s = 1;
    config.groups.push_back(group);
    fl::RuntimeState state; state.observe_only = false;
    fl::AcState a, b; setUnit(a, 0, true, 10000); setUnit(b, 1, true, 10000);
    a.last_started_ms = 0; b.last_started_ms = 0;
    a.observed.t1_deci_c.value = 200; b.observed.t1_deci_c.value = 210;
    state.conditioners.push_back(a); state.conditioners.push_back(b);
    SimRepository repository; fl::EventLog events; fl::RotationEngine engine(repository, events);
    CaptureCommands commands; fl::ClockState clock; clock.uptime_ms = 10000; clock.epoch = 100000;
    engine.tick(config, state, clock, commands);
    clock.uptime_ms = 11000; state.conditioners[0].last_seen_ms = 11000; state.conditioners[1].last_seen_ms = 11000;
    state.conditioners[0].observed.power.updated_ms = 11000; state.conditioners[1].observed.power.updated_ms = 11000;
    engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(1, commands.requests.size());
    TEST_ASSERT_FALSE(commands.requests[0].settings.power.value_or(true));
    const auto request = commands.requests[0];
    auto& stopped = state.conditioners[request.address];
    stopped.desired.transaction_id = request.transaction_id;
    stopped.desired.command_state = fl::CommandState::Acknowledged;
    stopped.observed.power.value = false; stopped.observed.power.updated_ms = 12000; stopped.last_seen_ms = 12000;
    state.conditioners[1].last_seen_ms = 12000; state.conditioners[1].observed.power.updated_ms = 12000;
    clock.uptime_ms = 12000; engine.tick(config, state, clock, commands);
    TEST_ASSERT_EQUAL_UINT(0, repository.stored_groups.size());
}
void test_xye_bus_poll_simulation_for_1_4_15_and_offline() {
    for (std::size_t count : {1U, 4U, 15U}) {
        fl::Config config; config.bus.retry_limit = 0; config.bus.response_timeout_ms = 10;
        for (std::size_t i = 0; i < count; ++i) {
            fl::AcRegistration item; item.address = static_cast<fl::Address>(i);
            item.name.assign("sim"); config.registry.push_back(item);
        }
        SimTransport transport; SimRepository repository; fl::EventLog events; fl::StateStore state;
        fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
        TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.begin()));
        for (fl::MonotonicMs now = 0; now < count * 4 + 10 && state.read().bus.poll_ok < count; ++now)
            bus.tick(config, state.edit(), now);
        TEST_ASSERT_EQUAL_UINT(count, transport.queries);
        TEST_ASSERT_EQUAL_UINT(count, state.read().conditioners.size());
        TEST_ASSERT_EQUAL_UINT(count, state.read().bus.poll_ok);
        TEST_ASSERT_TRUE(state.read().conditioners[count - 1].availability == fl::Availability::Unknown);
        for (fl::MonotonicMs now = 12000; now < 12000 + count * 4 + 10 && state.read().bus.poll_ok < count * 2; ++now)
            bus.tick(config, state.edit(), now);
        TEST_ASSERT_EQUAL_UINT(count * 2, transport.queries);
        TEST_ASSERT_TRUE(state.read().conditioners[count - 1].availability == fl::Availability::Online);
        transport.online = false;
        for (fl::MonotonicMs now = 24000; now < 60000 && state.read().bus.timeouts < 3; ++now)
            bus.tick(config, state.edit(), now);
        if (count == 1) TEST_ASSERT_TRUE(state.read().conditioners[0].availability == fl::Availability::Offline);
    }
}
void test_validated_c0_reply_is_confirmed_before_automation_runs() {
    fl::Config config; config.bus.retry_limit = 0; config.bus.response_timeout_ms = 10;
    config.bus.poll_interval_ms = 1000; config.bus.online_successes = 1;
    fl::AcRegistration registered; registered.address = 0; registered.name.assign("sim");
    registered.model_validated = true; config.registry.push_back(registered);
    SimTransport transport; SimRepository repository; fl::EventLog events; fl::StateStore state;
    fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.begin()));
    bus.tick(config, state.edit(), 0); // Send C0.
    bus.tick(config, state.edit(), 1); // Receive and decode the reply.
    TEST_ASSERT_EQUAL_UINT(1, state.read().bus.poll_ok);
    TEST_ASSERT_TRUE(state.read().conditioners[0].observed.power.confirmed());
    TEST_ASSERT_TRUE(state.read().conditioners[0].observed.power.status == fl::ValueStatus::Valid);
}
fl::ControlRequest coolingRequest(fl::Address address) {
    fl::ControlRequest request; request.address = address; request.origin = fl::CommandOrigin::Web;
    request.settings.power = true; request.settings.mode = fl::Mode::Cool;
    request.settings.fan = fl::Fan::Auto; request.settings.setpoint_deci_c = 260;
    return request;
}
void test_xye_bus_manual_command_queue_and_c0_verification() {
    fl::Config config; config.bus.retry_limit = 1; config.bus.response_timeout_ms = 10;
    config.bus.poll_interval_ms = 1000; config.bus.online_successes = 1;
    fl::AcRegistration registered; registered.address = 3; registered.name.assign("sim"); registered.model_validated = true;
    config.registry.push_back(registered);
    SimTransport transport; SimRepository repository; fl::EventLog events; fl::StateStore state;
    fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
    bus.begin();
    bus.tick(config, state.edit(), 0);
    bus.tick(config, state.edit(), 1);
    TEST_ASSERT_TRUE(state.read().conditioners[0].availability == fl::Availability::Online);
    auto request = coolingRequest(3);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.enqueue(request)));
    for (fl::MonotonicMs now = 2; now < 20 && state.read().conditioners[0].desired.command_state == fl::CommandState::Idle; ++now)
        bus.tick(config, state.edit(), now);
    for (fl::MonotonicMs now = 20; now < 5000 && state.read().conditioners[0].desired.command_state == fl::CommandState::Pending; ++now)
        bus.tick(config, state.edit(), now);
    const auto& ac = state.read().conditioners[0];
    TEST_ASSERT_EQUAL_UINT(1, transport.sets);
    TEST_ASSERT_EQUAL_INT(260, ac.observed.setpoint_deci_c.value.value_or(-1));
    TEST_ASSERT_TRUE(ac.observed.mode.value && *ac.observed.mode.value == fl::Mode::Cool);
    TEST_ASSERT_TRUE(ac.desired.command_state == fl::CommandState::Acknowledged);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Busy), static_cast<int>(bus.enqueue(request)));
    registered.model_validated = false; bus.updateAc(registered);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Forbidden), static_cast<int>(bus.enqueue(request)));
}
void test_xye_command_waits_for_c0_state_without_resending_c3() {
    fl::Config config; config.bus.retry_limit = 1; config.bus.response_timeout_ms = 10;
    config.bus.poll_interval_ms = 1000; config.bus.online_successes = 1;
    fl::AcRegistration registered; registered.address = 3; registered.name.assign("sim"); registered.model_validated = true;
    config.registry.push_back(registered);
    SimTransport transport; transport.stale_queries_after_set = 1;
    SimRepository repository; fl::EventLog events; fl::StateStore state;
    fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
    bus.begin(); bus.tick(config, state.edit(), 0); bus.tick(config, state.edit(), 1);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.enqueue(coolingRequest(3))));
    for (fl::MonotonicMs now = 2; now < 5000 && state.read().conditioners[0].desired.command_state != fl::CommandState::Acknowledged; ++now)
        bus.tick(config, state.edit(), now);
    const auto& ac = state.read().conditioners[0];
    TEST_ASSERT_TRUE(ac.desired.command_state == fl::CommandState::Acknowledged);
    TEST_ASSERT_EQUAL_UINT(1, transport.sets);
    TEST_ASSERT_EQUAL_UINT(3, transport.queries); // Initial poll, stale verification, settled verification.
    TEST_ASSERT_EQUAL_INT(260, ac.observed.setpoint_deci_c.value.value_or(-1));
}
void test_xye_off_command_retains_last_requested_settings() {
    fl::Config config; config.bus.retry_limit = 0; config.bus.response_timeout_ms = 10;
    config.bus.poll_interval_ms = 1000; config.bus.online_successes = 1;
    fl::AcRegistration registered; registered.address = 3; registered.name.assign("sim"); registered.model_validated = true;
    config.registry.push_back(registered);
    SimTransport transport; SimRepository repository; fl::EventLog events; fl::StateStore state;
    fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
    bus.begin(); bus.tick(config, state.edit(), 0); bus.tick(config, state.edit(), 1);

    auto on = coolingRequest(3); on.settings.mode = fl::Mode::Heat;
    on.settings.fan = fl::Fan::High; on.settings.setpoint_deci_c = 270;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.enqueue(on)));
    for (fl::MonotonicMs now = 2; now < 20 && state.read().conditioners[0].desired.command_state == fl::CommandState::Idle; ++now)
        bus.tick(config, state.edit(), now);
    for (fl::MonotonicMs now = 20; now < 5000 && state.read().conditioners[0].desired.command_state == fl::CommandState::Pending; ++now)
        bus.tick(config, state.edit(), now);
    TEST_ASSERT_TRUE(state.read().conditioners[0].desired.command_state == fl::CommandState::Acknowledged);

    for (fl::MonotonicMs now = 40; now <= 2001; ++now) bus.tick(config, state.edit(), now);
    fl::ControlRequest off; off.address = 3; off.origin = fl::CommandOrigin::Web; off.settings.power = false;
    const auto previous_transaction = state.read().conditioners[0].desired.transaction_id;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.enqueue(off)));
    for (fl::MonotonicMs now = 2002; now < 7000 &&
         (state.read().conditioners[0].desired.transaction_id == previous_transaction ||
          state.read().conditioners[0].desired.command_state != fl::CommandState::Acknowledged); ++now)
        bus.tick(config, state.edit(), now);

    const auto& ac = state.read().conditioners[0];
    TEST_ASSERT_TRUE(ac.desired.command_state == fl::CommandState::Acknowledged);
    TEST_ASSERT_FALSE(ac.desired.settings.power.value_or(true));
    TEST_ASSERT_TRUE(ac.desired.settings.mode && *ac.desired.settings.mode == fl::Mode::Heat);
    TEST_ASSERT_TRUE(ac.desired.settings.fan && *ac.desired.settings.fan == fl::Fan::High);
    TEST_ASSERT_EQUAL_INT(270, ac.desired.settings.setpoint_deci_c.value_or(-1));
}
void test_xye_bus_command_reports_unverified_state_without_resending_c3() {
    fl::Config config; config.bus.retry_limit = 1; config.bus.response_timeout_ms = 10;
    config.bus.poll_interval_ms = 1000; config.bus.online_successes = 1;
    fl::AcRegistration registered; registered.address = 4; registered.name.assign("sim"); registered.model_validated = true;
    config.registry.push_back(registered);
    SimTransport transport; transport.ignore_set = true;
    SimRepository repository; fl::EventLog events; fl::StateStore state;
    fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
    bus.begin(); bus.tick(config, state.edit(), 0); bus.tick(config, state.edit(), 1);
    const auto before = state.read().conditioners[0].observed.setpoint_deci_c.value;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.enqueue(coolingRequest(4))));
    for (fl::MonotonicMs now = 2; now < 5000 && state.read().conditioners[0].desired.command_state != fl::CommandState::Failed; ++now)
        bus.tick(config, state.edit(), now);
    const auto& ac = state.read().conditioners[0];
    TEST_ASSERT_TRUE(ac.desired.command_state == fl::CommandState::Failed);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::IoError), static_cast<int>(ac.desired.failure));
    TEST_ASSERT_EQUAL_UINT(1, transport.sets);
    TEST_ASSERT_EQUAL_INT(before.value_or(-1), ac.observed.setpoint_deci_c.value.value_or(-2));
}
void test_auto_group_blocks_web_commands_and_requires_master_for_rotation() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.retry_limit = 0;
    config.bus.response_timeout_ms = 10; config.bus.poll_interval_ms = 1000; config.bus.online_successes = 1;
    fl::AcRegistration item; item.address = 5; item.name.assign("sim"); item.model_validated = true;
    config.registry.push_back(item);
    auto group = testGroup(44, 1, 1, 1); group.members[0] = 5; config.groups.push_back(group);
    SimTransport transport; SimRepository repository; fl::EventLog events; fl::StateStore state;
    state.edit().observe_only = false;
    fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
    bus.begin(); bus.tick(config, state.edit(), 0); bus.tick(config, state.edit(), 1);
    auto web = coolingRequest(5);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Forbidden), static_cast<int>(bus.enqueue(web)));
    auto automatic = web; automatic.origin = fl::CommandOrigin::Rotation; automatic.group_id = 44;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.enqueue(automatic)));
    config.automatic_control_enabled = false;
    automatic.transaction_id = 0; automatic.address = 5;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Forbidden), static_cast<int>(bus.enqueue(automatic)));
    config.automatic_control_enabled = true; state.edit().observe_only = true;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Forbidden), static_cast<int>(bus.enqueue(automatic)));
}
void test_auto_group_manual_control_allows_web_commands_only_when_enabled() {
    fl::Config config; config.automatic_control_enabled = true; config.bus.retry_limit = 0;
    config.bus.response_timeout_ms = 10; config.bus.poll_interval_ms = 1000; config.bus.online_successes = 1;
    fl::AcRegistration item; item.address = 6; item.name.assign("manual"); item.model_validated = true;
    item.maintenance = true; config.registry.push_back(item);
    auto group = testGroup(63, 1, 1, 1); group.members[0] = 6; config.groups.push_back(group);
    SimTransport transport; SimRepository repository; fl::EventLog events; fl::StateStore state;
    state.edit().observe_only = false;
    fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
    bus.begin(); bus.tick(config, state.edit(), 0); bus.tick(config, state.edit(), 1);
    auto web = coolingRequest(6);
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.enqueue(web)));
    auto automatic = web; automatic.origin = fl::CommandOrigin::Rotation; automatic.group_id = 63;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Forbidden), static_cast<int>(bus.enqueue(automatic)));
}
void test_registry_removal_prunes_the_live_state() {
    fl::Config config; config.bus.retry_limit = 0; config.bus.response_timeout_ms = 10;
    config.bus.poll_interval_ms = 1000; config.bus.online_successes = 1;
    fl::AcRegistration item; item.address = 8; item.name.assign("removable"); config.registry.push_back(item);
    SimTransport transport; SimRepository repository; fl::EventLog events; fl::StateStore state;
    fl::XyeBusManager bus(transport, fl::defaultXyeCodec(), config, repository, events);
    bus.begin(); bus.tick(config, state.edit(), 0); bus.tick(config, state.edit(), 1);
    TEST_ASSERT_EQUAL_UINT(1, state.read().conditioners.size());
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(bus.removeAc(8)));
    bus.tick(config, state.edit(), 2);
    TEST_ASSERT_EQUAL_UINT(0, state.read().conditioners.size());
}
void test_application_clock_and_resource_budget() {
    fl::Application app;
    app.begin();
    app.tick(0x100000001ULL);
    TEST_ASSERT_TRUE(app.state().read().clock.uptime_ms == 0x100000001ULL);
    TEST_ASSERT_EQUAL_UINT(fl::kSchemaVersion, app.state().read().schema_version);
    TEST_ASSERT_EQUAL_UINT(0, app.state().read().conditioners.size());
    TEST_ASSERT_TRUE(fl::kModelBytes <= 96 * 1024);
    std::printf("sizeof(Config)=%zu; sizeof(StateStore)=%zu; model=%zu bytes\n",
                sizeof(fl::Config), sizeof(fl::StateStore), fl::kModelBytes);
}
void test_alarm_manager_debounces_offline_and_emits_confirmed_recovery() {
    fl::Config config; config.startup_observe_only_s = 0;
    fl::RuntimeState state; fl::AcState ac; ac.address = 9; ac.name.assign("Server room");
    ac.availability = fl::Availability::Offline; state.conditioners.push_back(ac);
    fl::AlarmManager manager; fl::ClockState clock{}; fl::AlarmEvent alarm;
    manager.tick(state, clock, config); TEST_ASSERT_FALSE(manager.nextEvent(alarm));
    clock.uptime_ms = 9999; manager.tick(state, clock, config); TEST_ASSERT_FALSE(manager.nextEvent(alarm));
    clock.uptime_ms = 10000; manager.tick(state, clock, config);
    TEST_ASSERT_TRUE(manager.nextEvent(alarm)); TEST_ASSERT_TRUE(alarm.alarm.type == fl::AlarmType::AcOffline);
    TEST_ASSERT_TRUE(alarm.alarm.state == fl::AlarmState::Problem); TEST_ASSERT_EQUAL_STRING("Server room", alarm.ac_name.c_str());
    state.conditioners[0].availability = fl::Availability::Online; state.conditioners[0].freshness = fl::Freshness::Fresh;
    clock.uptime_ms = 10001; manager.tick(state, clock, config); TEST_ASSERT_FALSE(manager.nextEvent(alarm));
    clock.uptime_ms = 20001; manager.tick(state, clock, config);
    TEST_ASSERT_TRUE(manager.nextEvent(alarm)); TEST_ASSERT_TRUE(alarm.alarm.state == fl::AlarmState::Recovery);
    TEST_ASSERT_FALSE(manager.nextEvent(alarm));
}
void test_alarm_manager_suppresses_boot_burst_and_debounces_fault() {
    fl::Config config; config.startup_observe_only_s = 60;
    fl::RuntimeState state; fl::AcState ac; ac.address = 3; ac.name.assign("AC-3");
    ac.availability = fl::Availability::Online; ac.freshness = fl::Freshness::Fresh;
    ac.observed.error_flags.value = 4; ac.observed.error_flags.status = fl::ValueStatus::Valid;
    state.conditioners.push_back(ac); fl::AlarmManager manager; fl::AlarmEvent alarm; fl::ClockState clock{};
    manager.tick(state, clock, config);
    clock.uptime_ms = 59999; manager.tick(state, clock, config);
    clock.uptime_ms = 60000; manager.tick(state, clock, config);
    TEST_ASSERT_FALSE(manager.nextEvent(alarm));
    state.conditioners[0].observed.error_flags.value = 0;
    clock.uptime_ms = 60001; manager.tick(state, clock, config); TEST_ASSERT_FALSE(manager.nextEvent(alarm));
    clock.uptime_ms = 70001; manager.tick(state, clock, config);
    TEST_ASSERT_TRUE(manager.nextEvent(alarm)); TEST_ASSERT_TRUE(alarm.alarm.type == fl::AlarmType::AcFault);
    TEST_ASSERT_TRUE(alarm.alarm.state == fl::AlarmState::Recovery);
}
void test_alarm_manager_reports_only_four_categories() {
    fl::Config config; config.startup_observe_only_s = 0;
    fl::GroupConfig saved; saved.id = 42; saved.name.assign("Cooling"); saved.high_temp_deci_c = 280; config.groups.push_back(saved);
    fl::RuntimeState state; fl::GroupRuntime group; group.id = 42; group.name.assign("Cooling");
    group.state = fl::GroupState::Emergency; group.temperature_deci_c.value = 300;
    group.temperature_deci_c.status = fl::ValueStatus::Valid; state.groups.push_back(group);
    fl::AlarmManager manager; fl::ClockState clock{}; fl::AlarmEvent alarm;
    manager.tick(state, clock, config); clock.uptime_ms = 10000; manager.tick(state, clock, config);
    TEST_ASSERT_TRUE(manager.nextEvent(alarm)); TEST_ASSERT_TRUE(alarm.alarm.type == fl::AlarmType::GroupOverTemperature);
    TEST_ASSERT_EQUAL_STRING("Cooling", alarm.group_name.c_str()); TEST_ASSERT_EQUAL_UINT(300, alarm.alarm.code);
}
void test_mqtt_state_contains_observed_values_only_and_json_escapes_names() {
    fl::RuntimeState state; fl::AcState fresh; fresh.address = 5; fresh.name.assign("Room \"A\"");
    fresh.availability = fl::Availability::Online; fresh.freshness = fl::Freshness::Fresh;
    fresh.observed.power.value = true; fresh.observed.power.status = fl::ValueStatus::Valid;
    fresh.observed.t1_deci_c.value = 237; fresh.observed.t1_deci_c.status = fl::ValueStatus::Valid;
    fresh.observed.t2_deci_c.value = 240; fresh.observed.t2_deci_c.status = fl::ValueStatus::Valid;
    fresh.observed.t2b_deci_c.value = 235; fresh.observed.t2b_deci_c.status = fl::ValueStatus::Valid;
    fresh.observed.t3_deci_c.value = 250; fresh.observed.t3_deci_c.status = fl::ValueStatus::Valid;
    fresh.observed.fan_actual.value = fl::FanSpeed::High; fresh.observed.fan_actual.status = fl::ValueStatus::Valid;
    fresh.observed.fan_auto.value = true; fresh.observed.fan_auto.status = fl::ValueStatus::Valid;
    fresh.observed.compressor_running.value = true; fresh.observed.compressor_running.status = fl::ValueStatus::Valid;
    fresh.observed.error_flags.value = 0x12; fresh.observed.error_flags.status = fl::ValueStatus::Valid;
    fresh.observed.protection_flags.value = 2; fresh.observed.protection_flags.status = fl::ValueStatus::Valid;
    state.conditioners.push_back(fresh);
    fl::AcState stale; stale.address = 6; stale.name.assign("Spare"); stale.availability = fl::Availability::Offline;
    stale.observed.power.value = true; stale.observed.power.status = fl::ValueStatus::Valid; state.conditioners.push_back(stale);
    char payload[2048]{}; std::size_t written = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::encodeMqttState(state, 123400, payload, sizeof(payload), written)));
    TEST_ASSERT_EQUAL_UINT(std::strlen(payload), written);
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\\\"A\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"power\":true"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"t1_deci_c\":237"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"t2_deci_c\":240"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"t2b_deci_c\":235"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"t3_deci_c\":250"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"fan_actual\":3"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"fan_auto\":true"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"compressor_running\":true"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"error_flags\":18"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"protection_flags\":2"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"t2b_deci_c\":null"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"address\":6,\"name\":\"Spare\",\"availability\":\"offline\",\"fresh\":false,\"power\":null"));
    TEST_ASSERT_NULL(std::strstr(payload, "desired")); TEST_ASSERT_NULL(std::strstr(payload, "last_seen"));
    TEST_ASSERT_NULL(std::strstr(payload, "checksum_errors"));
    char small[8]{};
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Full), static_cast<int>(fl::encodeMqttState(state, 0, small, sizeof(small), written)));
}
void test_mqtt_payload_fits_maximum_registry_and_group_capacity() {
    fl::RuntimeState state;
    std::array<char, 96> long_name{}; long_name.fill('A');
    const std::string_view name(long_name.data(), long_name.size());
    for (std::size_t i = 0; i < fl::kMaxAc; ++i) {
        fl::AcState ac; ac.address = static_cast<fl::Address>(i); ac.name.assign(name);
        ac.availability = fl::Availability::Online; ac.freshness = fl::Freshness::Fresh;
        ac.observed.power.value = true; ac.observed.power.status = fl::ValueStatus::Valid;
        ac.observed.mode.value = fl::Mode::Cool; ac.observed.mode.status = fl::ValueStatus::Valid;
        ac.observed.setpoint_deci_c.value = 220; ac.observed.setpoint_deci_c.status = fl::ValueStatus::Valid;
        ac.observed.fan_actual.value = fl::FanSpeed::Stopped; ac.observed.fan_actual.status = fl::ValueStatus::Valid;
        ac.observed.t1_deci_c.value = 240; ac.observed.t1_deci_c.status = fl::ValueStatus::Valid;
        ac.observed.t2_deci_c.value = 250; ac.observed.t2_deci_c.status = fl::ValueStatus::Valid;
        ac.observed.error_flags.value = 0; ac.observed.error_flags.status = fl::ValueStatus::Valid;
        TEST_ASSERT_TRUE(state.conditioners.push_back(ac));
    }
    for (std::size_t i = 0; i < fl::kMaxGroups; ++i) {
        fl::GroupRuntime group; group.id = static_cast<fl::GroupId>(i + 1); group.name.assign(name);
        group.state = fl::GroupState::Normal; group.required_active = 1; group.maximum_active = 2;
        group.temperature_deci_c.value = 250; group.temperature_deci_c.status = fl::ValueStatus::Valid;
        TEST_ASSERT_TRUE(state.groups.push_back(group));
    }
    std::array<char, fl::kMqttPayloadCapacity> payload{}; std::size_t written = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::encodeMqttState(
        state, 86400000, payload.data(), payload.size(), written)));
    TEST_ASSERT_EQUAL_UINT(std::strlen(payload.data()), written);
    TEST_ASSERT_TRUE(written < payload.size());
}
void test_mqtt_alarm_payload_and_connection_retry_backoff() {
    fl::AlarmEvent event; event.alarm.type = fl::AlarmType::AcOffline; event.alarm.state = fl::AlarmState::Problem;
    event.alarm.severity = fl::Severity::Warning; event.alarm.code = 0;
    event.ac_name.assign("Boiler room"); event.text.assign("AC is offline");
    char payload[512]{}; std::size_t written = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(fl::Result::Accepted), static_cast<int>(fl::encodeMqttEvent(event, payload, sizeof(payload), written)));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"type\":\"acOffline\""));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"state\":\"PROBLEM\""));
    TEST_ASSERT_NOT_NULL(std::strstr(payload, "\"ac_name\":\"Boiler room\""));
    fl::MqttRetrySchedule retry; TEST_ASSERT_TRUE(retry.due(0)); retry.failed(0);
    TEST_ASSERT_FALSE(retry.due(9999)); TEST_ASSERT_TRUE(retry.due(10000));
    retry.failed(10000); TEST_ASSERT_FALSE(retry.due(19999)); TEST_ASSERT_TRUE(retry.due(20000));
    retry.succeeded(); TEST_ASSERT_TRUE(retry.due(20001));
}

static_assert(std::is_abstract_v<fl::IBusManager>);
static_assert(std::is_abstract_v<fl::IRotationEngine>);
static_assert(std::is_abstract_v<fl::IWebService>);

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unknown_is_not_zero_or_confirmed);
    RUN_TEST(test_safe_defaults_and_empty_registry);
    RUN_TEST(test_persistent_config_roundtrip_without_full_config_temporary);
    RUN_TEST(test_bounded_registry_1_4_15_16);
    RUN_TEST(test_utf8_names_and_rejected_overflow);
    RUN_TEST(test_snmp_trap_text_is_ascii_and_transliterates_cyrillic);
    RUN_TEST(test_desired_does_not_change_observed);
    RUN_TEST(test_xye_query_frame_has_documented_layout_and_checksum);
    RUN_TEST(test_xye_homeops_captured_response_and_reject_corruption);
    RUN_TEST(test_xye_bus_poll_simulation_for_1_4_15_and_offline);
    RUN_TEST(test_validated_c0_reply_is_confirmed_before_automation_runs);
    RUN_TEST(test_xye_bus_manual_command_queue_and_c0_verification);
    RUN_TEST(test_xye_command_waits_for_c0_state_without_resending_c3);
    RUN_TEST(test_xye_off_command_retains_last_requested_settings);
    RUN_TEST(test_xye_bus_command_reports_unverified_state_without_resending_c3);
    RUN_TEST(test_auto_group_blocks_web_commands_and_requires_master_for_rotation);
    RUN_TEST(test_auto_group_manual_control_allows_web_commands_only_when_enabled);
    RUN_TEST(test_registry_removal_prunes_the_live_state);
    RUN_TEST(test_group_n_of_m_validation_and_storage_codec);
    RUN_TEST(test_ventilation_reserve_is_maintained_and_promoted_for_emergency);
    RUN_TEST(test_recovery_returns_extra_active_unit_to_ventilation);
    RUN_TEST(test_group_rotation_allows_single_capacity_without_overlap);
    RUN_TEST(test_rotation_starts_reserve_verifies_then_stops_old_unit);
    RUN_TEST(test_rotation_without_overlap_stops_old_before_starting_reserve);
    RUN_TEST(test_group_does_not_start_second_unit_below_high_temperature_threshold);
    RUN_TEST(test_auto_group_restores_remote_mode_and_setpoint_changes);
    RUN_TEST(test_manual_controlled_auto_member_is_never_reconciled_or_replaced_over_limit);
    RUN_TEST(test_rotation_keeps_running_unit_healthy_during_short_poll_gap);
    RUN_TEST(test_unknown_member_blocks_replacement_until_three_failed_polls);
    RUN_TEST(test_known_fault_failover_stops_old_unit_before_reserve);
    RUN_TEST(test_single_unit_policy_does_not_expand_even_during_emergency);
    RUN_TEST(test_rotation_failed_start_never_stops_existing_unit);
    RUN_TEST(test_rotation_reconciles_1_of_2_2_of_3_and_3_of_5);
    RUN_TEST(test_rotation_waits_for_clock_and_sensors_and_never_fails_back);
    RUN_TEST(test_emergency_recovery_stop_does_not_advance_rotation_schedule);
    RUN_TEST(test_alarm_manager_debounces_offline_and_emits_confirmed_recovery);
    RUN_TEST(test_alarm_manager_suppresses_boot_burst_and_debounces_fault);
    RUN_TEST(test_alarm_manager_reports_only_four_categories);
    RUN_TEST(test_mqtt_state_contains_observed_values_only_and_json_escapes_names);
    RUN_TEST(test_mqtt_payload_fits_maximum_registry_and_group_capacity);
    RUN_TEST(test_mqtt_alarm_payload_and_connection_retry_backoff);
    RUN_TEST(test_application_clock_and_resource_budget);
    return UNITY_END();
}
