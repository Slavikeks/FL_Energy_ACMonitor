#include "fl/network/Network.h"
#include <Arduino.h>
#include <ETH.h>
#include <time.h>

namespace fl {
namespace {
IPAddress toAddress(const Ipv4& value) {
    return IPAddress(value.octets[0], value.octets[1], value.octets[2], value.octets[3]);
}
Ipv4 fromAddress(const IPAddress& value) {
    Ipv4 result;
    for (unsigned i = 0; i < 4; ++i) result.octets[i] = value[i];
    return result;
}
void logEvent(EventLog& log, Severity severity, const char* message) {
    LogEvent entry; entry.severity = severity; entry.message.assign(message); log.append(entry);
}
}

EthernetTimeService::EthernetTimeService(Config& config, EventLog& log)
    : config_(config), log_(log) {}

Result EthernetTimeService::begin(const NetworkConfig& network) {
    network_ = network;
    ETH.setHostname("FL_Energy_ACMonitor");
    if (!ETH.begin()) return Result::IoError;
    const IPAddress none(0, 0, 0, 0);
    if (!ETH.config(network.dhcp ? none : toAddress(network.ip), toAddress(network.gateway),
                    toAddress(network.mask), toAddress(network.dns))) return Result::IoError;
    was_link_up_ = ETH.linkUp();
    return Result::Accepted;
}

Result EthernetTimeService::configure(const TimeConfig& time) {
    if (time.server.view().empty() || time.timezone.view().empty()) return Result::Invalid;
    time_ = time;
    ntp_started_ = false;
    return Result::Accepted;
}

Result EthernetTimeService::applyNetwork(const NetworkConfig& network) {
    const IPAddress none(0, 0, 0, 0);
    if (!ETH.config(network.dhcp ? none : toAddress(network.ip), toAddress(network.gateway),
                    toAddress(network.mask), toAddress(network.dns))) return Result::IoError;
    network_ = network;
    return Result::Accepted;
}

void EthernetTimeService::tick(NetworkState& state) {
    state.link = ETH.linkUp();
    state.dhcp = network_.dhcp;
    const auto address = ETH.localIP();
    if (state.link && static_cast<std::uint32_t>(address) != 0) state.ip = fromAddress(address);
    else state.ip.reset();
    if (state.link != was_link_up_) {
        logEvent(log_, state.link ? Severity::Info : Severity::Warning,
                 state.link ? "Ethernet link up" : "Ethernet link down");
        was_link_up_ = state.link;
        ntp_started_ = false;
    }
    if (state.link && !ntp_started_ && !time_.server.view().empty()) {
        configTzTime(time_.timezone.c_str(), time_.server.c_str());
        ntp_started_ = true;
    }
}

void EthernetTimeService::tick(ClockState& state) {
    state.uptime_ms = static_cast<MonotonicMs>(esp_timer_get_time() / 1000);
    const time_t epoch = ::time(nullptr);
    if (epoch >= 1704067200) state.epoch = static_cast<EpochSeconds>(epoch);
    else state.epoch.reset();
}
} // namespace fl
