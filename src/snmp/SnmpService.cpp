#include "fl/snmp/Snmp.h"
#include "fl/snmp/TrapText.h"
#include <SNMP_Agent.h>
#include <SNMPTrap.h>
#include <ETH.h>
#include <WiFiUdp.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

namespace fl {
namespace {
// Registered FLINK LLC enterprise root (IANA Private Enterprise Number 67213).
constexpr char kEnterpriseRoot[] = ".1.3.6.1.4.1.67213";
std::string rootedOid(const char* suffix) { return std::string(kEnterpriseRoot) + suffix; }
std::string* g_system_name = nullptr;
std::string* g_location = nullptr;
std::string* g_contact = nullptr;
const std::string getSystemName() { return g_system_name ? *g_system_name : std::string("FL_Energy_ACMonitor"); }
const std::string getLocation() { return g_location ? *g_location : std::string{}; }
const std::string getContact() { return g_contact ? *g_contact : std::string{}; }

class CompliantTrap final : public SNMPTrap {
public:
    CompliantTrap(const char* community, SNMP_VERSION version) : SNMPTrap(community, version) {
        delete snmpTrapOID;
        snmpTrapOID = new OIDType(".1.3.6.1.6.3.1.1.4.1.0");
    }
    ~CompliantTrap() override { delete trapOID; trapOID = nullptr; }
};

bool assignText(char* out, std::size_t capacity, std::string_view value) {
    if (value.size() >= capacity) return false;
    std::memcpy(out, value.data(), value.size()); out[value.size()] = '\0'; return true;
}
std::string_view trapMessage(AlarmType type, bool recovery) {
    switch (type) {
    case AlarmType::AcOffline: return recovery ? "Conditioner is responding" : "Conditioner is offline";
    case AlarmType::AcFault: return recovery ? "Conditioner fault cleared" : "Conditioner fault detected";
    case AlarmType::GroupDegraded: return recovery ? "Group capacity restored" : "Group capacity degraded";
    case AlarmType::GroupOverTemperature: return recovery ? "Group temperature normalized" : "Group temperature above limit";
    }
    return "System alarm";
}
bool sameConfig(const SnmpConfig& a, const SnmpConfig& b) {
    if (a.enabled != b.enabled || a.ro_community.view() != b.ro_community.view() ||
        a.system_name.view() != b.system_name.view() || a.location.view() != b.location.view() ||
        a.contact.view() != b.contact.view() || a.trap_port != b.trap_port ||
        a.trap_receiver.has_value() != b.trap_receiver.has_value()) return false;
    return !a.trap_receiver || a.trap_receiver->octets == b.trap_receiver->octets;
}
void makeOid(char* out, std::size_t capacity, const char* branch, unsigned column, unsigned index) {
    std::snprintf(out, capacity, "%s.%u.%u", branch, column, index);
}
template <typename Callback> void destroyCallback(ValueCallback* callback) {
    auto* typed = static_cast<Callback*>(callback); typed->~Callback(); ::operator delete(static_cast<void*>(typed));
}
}

struct SnmpService::Impl {
    static constexpr std::size_t kAcColumns = 15;
    static constexpr std::size_t kGroupColumns = 8;
    struct Row {
        bool used{false}; unsigned index{0};
        std::array<int, kAcColumns> values{};
        std::array<ValueCallback*, kAcColumns> callbacks{};
        std::array<char, 97> name{}; char* name_pointer{name.data()};
    };
    struct GroupRow {
        bool used{false}; unsigned index{0};
        std::array<int, kGroupColumns> values{};
        std::array<ValueCallback*, kGroupColumns> callbacks{};
        std::array<char, 97> name{}; char* name_pointer{name.data()};
    };

    SNMPAgent agent{}; WiFiUDP udp{}; SnmpConfig config{}; bool started{false}; bool enabled{false};
    std::uint32_t uptime_ticks{0}; std::string system_name{"FL_Energy_ACMonitor"}, location{}, contact{};
    std::array<Row, kMaxAc> ac_rows{}; std::array<GroupRow, kMaxGroups> group_rows{};
    CompliantTrap* trap{nullptr};
    int trap_state{0}, trap_severity{0}, trap_type{0}, trap_code{0};
    std::array<char, 97> trap_ac_name{}, trap_group_name{};
    std::array<char, 161> trap_text{}; std::array<char, 32> trap_timestamp{};
    char* trap_ac_ptr{trap_ac_name.data()}; char* trap_group_ptr{trap_group_name.data()};
    char* trap_text_ptr{trap_text.data()}; char* trap_timestamp_ptr{trap_timestamp.data()};
    std::array<ValueCallback*, 9> trap_bindings{};
    bool handlers_dirty{false};

    Impl() { g_system_name = &system_name; g_location = &location; g_contact = &contact; }
    ~Impl() {
        for (auto& row : ac_rows) removeRow(row);
        for (auto& row : group_rows) removeGroup(row);
        delete trap;
        if (trap_bindings[0]) destroyCallback<TimestampCallback>(trap_bindings[0]);
        for (std::size_t i = 1; i <= 3; ++i) if (trap_bindings[i]) destroyCallback<IntegerCallback>(trap_bindings[i]);
        for (std::size_t i = 4; i <= 7; ++i) if (trap_bindings[i]) destroyCallback<StringCallback>(trap_bindings[i]);
        if (trap_bindings[8]) destroyCallback<IntegerCallback>(trap_bindings[8]);
        if (g_system_name == &system_name) g_system_name = nullptr;
        if (g_location == &location) g_location = nullptr;
        if (g_contact == &contact) g_contact = nullptr;
    }
    void remove(ValueCallback*& callback, bool string_value) {
        if (!callback) return;
        agent.removeHandler(callback);
        if (string_value) destroyCallback<StringCallback>(callback);
        else destroyCallback<IntegerCallback>(callback);
        callback = nullptr; handlers_dirty = true;
    }
    void removeRow(Row& row) { for (std::size_t i = 0; i < row.callbacks.size(); ++i) remove(row.callbacks[i], i == 1); row.used = false; }
    void removeGroup(GroupRow& row) { for (std::size_t i = 0; i < row.callbacks.size(); ++i) remove(row.callbacks[i], i == 1); row.used = false; }
    void addInteger(ValueCallback*& callback, const char* branch, unsigned column, unsigned index, int* value) {
        char oid[96]; makeOid(oid, sizeof(oid), branch, column, index);
        callback = agent.addIntegerHandler(oid, value, false, true); handlers_dirty = true;
    }
    void syncInteger(ValueCallback*& callback, const char* branch, unsigned column, unsigned index, int& value, bool valid) {
        if (!valid) { remove(callback, false); return; }
        if (!callback) addInteger(callback, branch, column, index, &value);
    }
    void syncName(ValueCallback*& callback, char** pointer, const char* branch, unsigned column, unsigned index) {
        if (!callback) {
            char oid[96]; makeOid(oid, sizeof(oid), branch, column, index);
            callback = agent.addReadWriteStringHandler(oid, pointer, 96, false, true);
            handlers_dirty = true;
        }
    }
    void installBase() {
        const std::string description("FL Energy AC Monitor, firmware " + std::string(kFirmwareVersion));
        const std::string product_oid = rootedOid(".1");
        agent.addReadOnlyStaticStringHandler(".1.3.6.1.2.1.1.1.0", description, true);
        agent.addOIDHandler(".1.3.6.1.2.1.1.2.0", product_oid, true);
        agent.addTimestampHandler(".1.3.6.1.2.1.1.3.0", &uptime_ticks, false, true);
        agent.addDynamicReadOnlyStringHandler(".1.3.6.1.2.1.1.4.0", getContact, true);
        agent.addDynamicReadOnlyStringHandler(".1.3.6.1.2.1.1.5.0", getSystemName, true);
        agent.addDynamicReadOnlyStringHandler(".1.3.6.1.2.1.1.6.0", getLocation, true);
        installTrapBindings(); agent.sortHandlers();
    }
    void installTrapBindings() {
        const std::string root = rootedOid(".2.10");
        char oid[96];
        auto intBinding = [&](unsigned col, int& value) -> ValueCallback* {
            makeOid(oid, sizeof(oid), root.c_str(), col, 0);
            auto* cb = new IntegerCallback(new SortableOIDType(oid), &value); cb->isSettable = false; return cb;
        };
        auto stringBinding = [&](unsigned col, char** value, std::size_t max_len) -> ValueCallback* {
            makeOid(oid, sizeof(oid), root.c_str(), col, 0);
            auto* cb = new StringCallback(new SortableOIDType(oid), value, max_len); cb->isSettable = false; return cb;
        };
        trap_bindings[0] = new TimestampCallback(new SortableOIDType(".1.3.6.1.2.1.1.3.0"), &uptime_ticks);
        trap_bindings[1] = intBinding(1, trap_state); trap_bindings[2] = intBinding(2, trap_severity);
        trap_bindings[3] = intBinding(3, trap_type); trap_bindings[4] = stringBinding(4, &trap_ac_ptr, 96);
        trap_bindings[5] = stringBinding(5, &trap_group_ptr, 96); trap_bindings[6] = stringBinding(6, &trap_text_ptr, 160);
        trap_bindings[7] = stringBinding(7, &trap_timestamp_ptr, 31); trap_bindings[8] = intBinding(8, trap_code);
    }
    void configureTrap() {
        delete trap; trap = nullptr;
        if (!enabled || !config.trap_receiver) return;
        // Keep notifications on v2c; legacy receivers can still decode these
        // packets, and the notification OID is defined in the main SMIv2 MIB.
        trap = new CompliantTrap(config.ro_community.c_str(), SNMP_VERSION_2C);
        trap->setUDP(&udp); trap->setUDPport(static_cast<short>(config.trap_port));
        trap->setUptimeCallback(static_cast<TimestampCallback*>(trap_bindings[0]));
        for (std::size_t i = 1; i < trap_bindings.size(); ++i) trap->addOIDPointer(trap_bindings[i]);
    }
    Result configure(const SnmpConfig& next) {
        if (next.enabled && next.ro_community.view().empty()) return Result::Invalid;
        if (next.enabled && next.system_name.view().empty()) return Result::Invalid;
        if (next.trap_port == 0) return Result::Invalid;
        if (started && sameConfig(config, next)) return Result::Accepted;
        config = next; enabled = config.enabled;
        system_name.assign(config.system_name.view()); location.assign(config.location.view()); contact.assign(config.contact.view());
        agent.setReadOnlyCommunity(std::string(config.ro_community.view()));
        agent.setReadWriteCommunity("__FL_ENERGY_READ_ONLY__");
        if (!started) { installBase(); agent.setUDP(&udp); started = true; }
        if (enabled) agent.restartUDP(); else agent.stop();
        configureTrap(); return Result::Accepted;
    }
    int ensureAcRow(Address address) {
        for (std::size_t i = 0; i < ac_rows.size(); ++i) if (ac_rows[i].used && ac_rows[i].index == address) return static_cast<int>(i);
        for (std::size_t i = 0; i < ac_rows.size(); ++i) if (!ac_rows[i].used) {
            ac_rows[i].used = true; ac_rows[i].index = address; return static_cast<int>(i);
        }
        return -1;
    }
    int ensureGroupRow(GroupId id) {
        for (std::size_t i = 0; i < group_rows.size(); ++i) if (group_rows[i].used && group_rows[i].index == id) return static_cast<int>(i);
        for (std::size_t i = 0; i < group_rows.size(); ++i) if (!group_rows[i].used) {
            group_rows[i].used = true; group_rows[i].index = id; return static_cast<int>(i);
        }
        return -1;
    }
    void sync(const RuntimeState& state, MonotonicMs now) {
        uptime_ticks = static_cast<std::uint32_t>(now / 10);
        std::array<bool, kMaxAc> ac_seen{};
        for (const auto& ac : state.conditioners) {
            const int position = ensureAcRow(ac.address); if (position < 0) continue;
            auto& row = ac_rows[static_cast<std::size_t>(position)]; ac_seen[static_cast<std::size_t>(position)] = true;
            const std::string root = rootedOid(".3.1");
            row.values[0] = ac.address; syncInteger(row.callbacks[0], root.c_str(), 1, ac.address, row.values[0], true);
            assignText(row.name.data(), row.name.size(), ac.name.view()); syncName(row.callbacks[1], &row.name_pointer, root.c_str(), 2, ac.address);
            const bool fresh = ac.availability == Availability::Online && ac.freshness == Freshness::Fresh;
            row.values[2] = ac.availability == Availability::Online ? 1 : ac.availability == Availability::Offline ? 2 : 0;
            syncInteger(row.callbacks[2], root.c_str(), 3, ac.address, row.values[2], true);
            row.values[3] = ac.observed.power.value.value_or(false) ? 1 : 2;
            syncInteger(row.callbacks[3], root.c_str(), 4, ac.address, row.values[3], fresh && ac.observed.power.confirmed());
            row.values[4] = ac.observed.mode.value ? static_cast<int>(*ac.observed.mode.value) : 0;
            syncInteger(row.callbacks[4], root.c_str(), 5, ac.address, row.values[4], fresh && ac.observed.mode.confirmed());
            row.values[5] = ac.observed.setpoint_deci_c.value.value_or(0);
            syncInteger(row.callbacks[5], root.c_str(), 6, ac.address, row.values[5], fresh && ac.observed.setpoint_deci_c.confirmed());
            row.values[6] = ac.observed.fan_actual.value ? static_cast<int>(*ac.observed.fan_actual.value) : 0;
            syncInteger(row.callbacks[6], root.c_str(), 7, ac.address, row.values[6], fresh && ac.observed.fan_actual.confirmed());
            row.values[7] = ac.observed.t1_deci_c.value.value_or(0);
            syncInteger(row.callbacks[7], root.c_str(), 8, ac.address, row.values[7], fresh && ac.observed.t1_deci_c.confirmed());
            row.values[8] = ac.observed.t2_deci_c.value.value_or(0);
            syncInteger(row.callbacks[8], root.c_str(), 9, ac.address, row.values[8], fresh && ac.observed.t2_deci_c.confirmed());
            row.values[9] = ac.observed.error_flags.value.value_or(0);
            syncInteger(row.callbacks[9], root.c_str(), 10, ac.address, row.values[9], fresh && ac.observed.error_flags.confirmed());
            row.values[10] = ac.observed.fan_auto.value.value_or(false) ? 1 : 0;
            syncInteger(row.callbacks[10], root.c_str(), 11, ac.address, row.values[10], fresh && ac.observed.fan_auto.confirmed());
            row.values[11] = ac.observed.compressor_running.value.value_or(false) ? 1 : 0;
            syncInteger(row.callbacks[11], root.c_str(), 12, ac.address, row.values[11], fresh && ac.observed.compressor_running.confirmed());
            row.values[12] = ac.observed.protection_flags.value.value_or(0);
            syncInteger(row.callbacks[12], root.c_str(), 13, ac.address, row.values[12], fresh && ac.observed.protection_flags.confirmed());
            row.values[13] = ac.observed.t2b_deci_c.value.value_or(0);
            syncInteger(row.callbacks[13], root.c_str(), 14, ac.address, row.values[13], fresh && ac.observed.t2b_deci_c.confirmed());
            row.values[14] = ac.observed.t3_deci_c.value.value_or(0);
            syncInteger(row.callbacks[14], root.c_str(), 15, ac.address, row.values[14], fresh && ac.observed.t3_deci_c.confirmed());
        }
        for (std::size_t i = 0; i < ac_rows.size(); ++i) if (ac_rows[i].used && !ac_seen[i]) removeRow(ac_rows[i]);

        std::array<bool, kMaxGroups> group_seen{};
        for (const auto& group : state.groups) {
            const int position = ensureGroupRow(group.id); if (position < 0) continue;
            auto& row = group_rows[static_cast<std::size_t>(position)]; group_seen[static_cast<std::size_t>(position)] = true;
            const std::string root = rootedOid(".4.1");
            row.values[0] = group.id; syncInteger(row.callbacks[0], root.c_str(), 1, group.id, row.values[0], true);
            assignText(row.name.data(), row.name.size(), group.name.view()); syncName(row.callbacks[1], &row.name_pointer, root.c_str(), 2, group.id);
            row.values[2] = group.mode == GroupMode::Auto ? 1 : 0; syncInteger(row.callbacks[2], root.c_str(), 3, group.id, row.values[2], true);
            row.values[3] = static_cast<int>(group.state); syncInteger(row.callbacks[3], root.c_str(), 4, group.id, row.values[3], true);
            row.values[4] = group.required_active; syncInteger(row.callbacks[4], root.c_str(), 5, group.id, row.values[4], true);
            row.values[5] = group.maximum_active; syncInteger(row.callbacks[5], root.c_str(), 6, group.id, row.values[5], true);
            row.values[6] = static_cast<int>(group.active_addresses.count()); syncInteger(row.callbacks[6], root.c_str(), 7, group.id, row.values[6], true);
            row.values[7] = group.temperature_deci_c.value.value_or(0);
            syncInteger(row.callbacks[7], root.c_str(), 8, group.id, row.values[7], group.temperature_deci_c.confirmed());
        }
        for (std::size_t i = 0; i < group_rows.size(); ++i) if (group_rows[i].used && !group_seen[i]) removeGroup(group_rows[i]);
        if (handlers_dirty) { agent.sortHandlers(); handlers_dirty = false; }
    }
};

SnmpService::SnmpService() : impl_(new Impl) {}
SnmpService::~SnmpService() { delete impl_; }
Result SnmpService::begin(const SnmpConfig& config) { return impl_->configure(config); }
Result SnmpService::configure(const SnmpConfig& config) { return impl_->configure(config); }
void SnmpService::tick(const IStateView& state, MonotonicMs now) {
    if (!impl_->started) return;
    impl_->uptime_ticks = static_cast<std::uint32_t>(now / 10);
    if (!impl_->enabled) return;
    impl_->sync(state.read(), now); impl_->agent.loop();
}
Result SnmpService::sendAlarm(const AlarmEvent& event) {
    if (!impl_->enabled || !impl_->trap || !impl_->config.trap_receiver) return Result::NotFound;
    impl_->trap_state = event.alarm.state == AlarmState::Problem ? 1 : 2;
    impl_->trap_severity = event.alarm.severity == Severity::Critical ? 2 : event.alarm.severity == Severity::Warning ? 1 : 0;
    impl_->trap_type = static_cast<int>(event.alarm.type) + 1; impl_->trap_code = static_cast<int>(event.alarm.code);
    toSnmpDisplayText(event.ac_name.view(), impl_->trap_ac_name.data(), impl_->trap_ac_name.size());
    toSnmpDisplayText(event.group_name.view(), impl_->trap_group_name.data(), impl_->trap_group_name.size());
    assignText(impl_->trap_text.data(), impl_->trap_text.size(), trapMessage(event.alarm.type, event.alarm.state == AlarmState::Recovery));
    impl_->trap_timestamp.fill('\0');
    if (event.alarm.epoch) {
        const std::time_t raw = static_cast<std::time_t>(*event.alarm.epoch); std::tm utc{};
        if (gmtime_r(&raw, &utc)) std::strftime(impl_->trap_timestamp.data(), impl_->trap_timestamp.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    }
    const std::string notification_oid = rootedOid(".2.0.") + std::to_string(impl_->trap_type);
    delete impl_->trap->trapOID; impl_->trap->setTrapOID(new OIDType(notification_oid));
    const auto ip = *impl_->config.trap_receiver;
    impl_->trap->setIP(ETH.localIP());
    return impl_->trap->sendTo(IPAddress(ip.octets[0], ip.octets[1], ip.octets[2], ip.octets[3])) ? Result::Accepted : Result::IoError;
}
} // namespace fl
