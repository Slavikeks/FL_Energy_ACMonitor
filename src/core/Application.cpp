#include "fl/core/Application.h"
#if defined(ARDUINO_ARCH_ESP32)
#include "fl/core/ConfigRepository.h"
#include "fl/auth/AuthService.h"
#include "fl/network/Network.h"
#include "fl/web/Web.h"
#include "fl/xye/XyeBusManager.h"
#include "fl/ota/Ota.h"
#include "fl/rotation/RotationEngine.h"
#include "fl/alarm/AlarmManager.h"
#include "fl/snmp/Snmp.h"
#include "fl/mqtt/Mqtt.h"
#include "fl/rotation/GroupCodec.h"
#include <Arduino.h>
#include <new>
#endif

namespace fl {
#if defined(ARDUINO_ARCH_ESP32)
namespace {
void logApp(EventLog& log, Severity severity, const char* message) {
    LogEvent event; event.severity = severity; event.message.assign(message); log.append(event);
}
class SettingsService final : public ISettingsService {
public:
    SettingsService(Config& active, IConfigRepository& repository, EthernetTimeService& network,
                    IOtaService& ota, ISnmpService& snmp, IMqttService& mqtt)
        : active_(active), repository_(repository), network_(network), ota_(ota), snmp_(snmp), mqtt_(mqtt) {}
    Result updateConfig(const Config& next) override {
        if (ota_.state() == OtaState::Receiving || next.time.server.view().empty() ||
            next.time.timezone.view().empty() || next.auth.username.view().empty()) return Result::Busy;
        const NetworkConfig previous_network = active_.network;
        const TimeConfig previous_time = active_.time;
        const bool network_changed = next.network.dhcp != previous_network.dhcp || next.network.ip.octets != previous_network.ip.octets ||
            next.network.mask.octets != previous_network.mask.octets || next.network.gateway.octets != previous_network.gateway.octets ||
            next.network.dns.octets != previous_network.dns.octets;
        const bool time_changed = next.time.server.view() != previous_time.server.view() || next.time.timezone.view() != previous_time.timezone.view();
        auto result = Result::Accepted;
        if (network_changed) { result = network_.applyNetwork(next.network); if (result != Result::Accepted) return result; }
        if (time_changed) {
            result = network_.configure(next.time);
            if (result != Result::Accepted) { if (network_changed) network_.applyNetwork(previous_network); return result; }
        }
        result = snmp_.configure(next.snmp);
        if (result != Result::Accepted) {
            if (network_changed) network_.applyNetwork(previous_network);
            if (time_changed) network_.configure(previous_time);
            return result;
        }
        result = mqtt_.configure(next.mqtt);
        if (result != Result::Accepted) {
            snmp_.configure(active_.snmp);
            if (network_changed) network_.applyNetwork(previous_network);
            if (time_changed) network_.configure(previous_time);
            return result;
        }
        result = repository_.save(next);
        if (result != Result::Accepted) {
            mqtt_.configure(active_.mqtt); snmp_.configure(active_.snmp);
            if (network_changed) network_.applyNetwork(previous_network);
            if (time_changed) network_.configure(previous_time);
            return result;
        }
        active_ = next;
        return Result::Accepted;
    }
    Result putGroup(const GroupConfig& group) override {
        if (ota_.state() == OtaState::Receiving) return Result::Busy;
        groups_scratch_ = active_.groups;
        bool replaced = false;
        for (auto& current : groups_scratch_) if (current.id == group.id) { current = group; replaced = true; break; }
        if (!replaced && !groups_scratch_.push_back(group)) return Result::Full;
        const auto valid = validateGroupSet(active_, groups_scratch_);
        if (valid != Result::Accepted) return valid;
        const auto result = repository_.saveGroups(groups_scratch_);
        if (result == Result::Accepted) active_.groups = groups_scratch_;
        return result;
    }
    Result deleteGroup(GroupId id) override {
        if (ota_.state() == OtaState::Receiving) return Result::Busy;
        groups_scratch_ = active_.groups;
        for (std::size_t i = 0; i < groups_scratch_.size(); ++i) if (groups_scratch_[i].id == id) {
            groups_scratch_.erase(i);
            const auto result = repository_.saveGroups(groups_scratch_);
            if (result == Result::Accepted) active_.groups = groups_scratch_;
            return result;
        }
        return Result::NotFound;
    }
private:
    Config& active_; IConfigRepository& repository_; EthernetTimeService& network_; IOtaService& ota_;
    ISnmpService& snmp_; IMqttService& mqtt_;
    List<GroupConfig, kMaxGroups> groups_scratch_{};
};
}
#endif

#if defined(ARDUINO_ARCH_ESP32)
struct Application::ServiceStorage {
    NvsConfigRepository repository;
    EthernetTimeService network;
    AuthService auth;
    OtaService ota;
    SnmpService snmp;
    MqttService mqtt;
    AlarmManager alarms;
    SettingsService settings;
    RotationEngine rotation;
    IRs485Transport& transport;
    XyeBusManager bus;
    WebService web;
    explicit ServiceStorage(Application& app)
        : network(app.config_, app.log_), auth(app.config_, repository),
          ota(app.state_.edit(), app.log_), settings(app.config_, repository, network, ota, snmp, mqtt),
          rotation(repository, app.log_),
          transport(defaultRs485Transport()), bus(transport, defaultXyeCodec(), app.config_, repository, app.log_),
          web(app.state_, app.config_, settings, bus, bus, auth, ota, app.log_, app.clock_) {}
};
#endif

Application::~Application() {
#if defined(ARDUINO_ARCH_ESP32)
    if (service_) {
        delete service_;
    }
#endif
}

void Application::begin() {
#if defined(ARDUINO_ARCH_ESP32)
    static_assert(sizeof(ServiceStorage) <= kServiceStorageMaxBytes,
                  "Application service composition exceeds its bounded memory budget");
    state_.edit().observe_only = true;
    Serial.printf("service_storage=%u B; heap_before=%u B\n",
                  static_cast<unsigned>(sizeof(ServiceStorage)), ESP.getFreeHeap());
    auto* services = new (std::nothrow) ServiceStorage(*this);
    if (!services) {
        logApp(log_, Severity::Critical, "Not enough memory for application services");
        return;
    }
    service_ = services;
    Serial.printf("heap_after_services=%u B\n", ESP.getFreeHeap());
    const Result loaded = services->repository.load(config_);
    if (loaded != Result::Accepted && loaded != Result::NotFound)
        logApp(log_, Severity::Warning, "Configuration invalid; using defaults");
    const Result registry_loaded = services->repository.loadRegistry(config_.registry);
    if (registry_loaded != Result::Accepted && registry_loaded != Result::NotFound)
        logApp(log_, Severity::Warning, "AC registry invalid; ignoring saved registry");
    const Result groups_loaded = services->repository.loadGroups(config_.groups);
    if (groups_loaded != Result::Accepted && groups_loaded != Result::NotFound)
        logApp(log_, Severity::Warning, "Group configuration invalid; ignoring saved groups");
    if (validateGroupSet(config_, config_.groups) != Result::Accepted) {
        config_.groups.clear();
        logApp(log_, Severity::Warning, "Saved group membership invalid; groups disabled");
    }
    for (const auto& item : config_.registry) {
        AcState ac; ac.address = item.address; ac.name = item.name; ac.maintenance = item.maintenance;
        state_.edit().conditioners.push_back(ac);
    }
    state_.edit().device_name = config_.snmp.system_name;
    const auto auth_result = services->auth.begin();
    if (auth_result != Result::Accepted) logApp(log_, Severity::Critical, "Authentication configuration failed");
    if (services->network.begin(config_.network) != Result::Accepted)
        logApp(log_, Severity::Warning, "Ethernet initialization failed");
    if (services->network.configure(config_.time) != Result::Accepted)
        logApp(log_, Severity::Warning, "Time configuration invalid");
    if (services->snmp.begin(config_.snmp) != Result::Accepted)
        logApp(log_, Severity::Warning, "SNMP configuration invalid; agent disabled");
    if (services->mqtt.configure(config_.mqtt) != Result::Accepted)
        logApp(log_, Severity::Warning, "MQTT configuration invalid; telemetry disabled");
    state_.edit().bus.baud = kRs485Baud;
    if (services->bus.begin() != Result::Accepted) logApp(log_, Severity::Critical, "RS-485 UART initialization failed");
    if (services->web.begin() != Result::Accepted) logApp(log_, Severity::Critical, "Web server initialization failed");
    for (const auto& group : config_.groups) {
        GroupRuntime runtime; runtime.id = group.id; runtime.name = group.name;
        runtime.members = group.members; runtime.mode = group.mode;
        runtime.required_active = group.required_active; runtime.maximum_active = group.maximum_active;
        runtime.ac_mode = group.ac_mode; runtime.setpoint_deci_c = group.setpoint_deci_c;
        runtime.fan = group.fan; runtime.rotation_enabled = group.rotation_enabled;
        state_.edit().groups.push_back(runtime);
    }
    logApp(log_, Severity::Info, "FL Energy AC Monitor started");
#else
    state_.edit().observe_only = true;
#endif
}

void Application::tick(MonotonicMs uptime_ms) {
    auto& state = state_.edit();
    state.clock.uptime_ms = uptime_ms;
#if defined(ARDUINO_ARCH_ESP32)
    if (!service_) return;
    service_->network.tick(state.network);
    service_->network.tick(state.clock);
    service_->bus.tick(config_, state, uptime_ms);
    state.observe_only = uptime_ms < static_cast<MonotonicMs>(config_.startup_observe_only_s) * 1000;
    service_->rotation.tick(config_, state, state.clock, service_->bus);
    service_->alarms.tick(state, state.clock, config_);
    AlarmEvent alarm;
    while (service_->alarms.nextEvent(alarm)) {
        service_->snmp.sendAlarm(alarm);
        service_->mqtt.publishAlarm(alarm);
    }
    service_->snmp.tick(state_, uptime_ms);
    service_->mqtt.tick(state_, uptime_ms);
    service_->web.tick(uptime_ms);
#endif
}
} // namespace fl
