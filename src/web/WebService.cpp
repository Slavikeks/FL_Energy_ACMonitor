#include "fl/web/Web.h"
#include "fl/core/AutomationHealth.h"
#include "EmbeddedWebAssets.h"
#include <WebServer.h>
#include <Update.h>
#include <ETH.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace fl {
namespace {
std::string cookieToken(WebServer& server) {
    String cookies = server.header("Cookie");
    int start = 0;
    while (start < cookies.length()) {
        int end = cookies.indexOf(';', start); if (end < 0) end = cookies.length();
        String part = cookies.substring(start, end); part.trim();
        if (part.startsWith("fl_session=")) return std::string(part.c_str() + 11);
        start = end + 1;
    }
    return {};
}
bool parseIp(const String& input, Ipv4& output) {
    IPAddress parsed;
    if (!parsed.fromString(input)) return false;
    for (unsigned i = 0; i < 4; ++i) output.octets[i] = parsed[i];
    return true;
}
bool parseLong(const String& input, long& output) {
    const char* text = input.c_str(); char* end = nullptr; errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    output = parsed; return true;
}
bool parseColor(const String& input, std::uint32_t& output) {
    if (input.length() != 7 || input[0] != '#') return false;
    std::uint32_t value = 0;
    for (unsigned i = 1; i < 7; ++i) {
        const char c = input[i];
        const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0') :
            c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10) :
            c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10) : 16U;
        if (digit > 15) return false;
        value = (value << 4) | digit;
    }
    output = value; return true;
}
String colorText(std::uint32_t color) {
    char text[8]{};
    std::snprintf(text, sizeof(text), "#%06lX", static_cast<unsigned long>(color & 0xFFFFFFU));
    return text;
}
String ipText(const Ipv4& ip) {
    char buffer[16]; std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u",
        ip.octets[0], ip.octets[1], ip.octets[2], ip.octets[3]); return buffer;
}
void jsonString(String& out, std::string_view value) {
    out += '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) out += ' ';
        else out += static_cast<char>(c);
    }
    out += '"';
}
String prettyJson(const String& compact) {
    String pretty; pretty.reserve(compact.length() + compact.length() / 3);
    unsigned depth = 0; bool quoted = false; bool escaped = false;
    auto indent = [&] { pretty += '\n'; for (unsigned i = 0; i < depth * 2; ++i) pretty += ' '; };
    for (unsigned i = 0; i < compact.length(); ++i) {
        const char c = compact[i];
        if (quoted) {
            pretty += c;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') { quoted = true; pretty += c; }
        else if (c == '{' || c == '[') { pretty += c; ++depth; indent(); }
        else if (c == '}' || c == ']') {
            if (i > 0 && compact[i - 1] != '{' && compact[i - 1] != '[') { --depth; indent(); }
            else if (depth) --depth;
            pretty += c;
        } else if (c == ',') { pretty += c; indent(); }
        else if (c == ':') pretty += ": ";
        else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') pretty += c;
    }
    return pretty;
}
const char* severityName(Severity severity) {
    switch (severity) { case Severity::Info: return "info"; case Severity::Warning: return "warning";
        case Severity::Critical: return "critical"; }
    return "info";
}
void event(EventLog& log, Severity severity, const char* message) {
    LogEvent entry; entry.severity = severity; entry.message.assign(message); log.append(entry);
}
const char* groupValidationError(const Config& config, const GroupConfig& group) {
    if (group.members.size() == 0) return "group_requires_members";
    if (group.required_active > group.maximum_active) return "maximum_below_required";
    if (group.maximum_active > group.members.size()) return "maximum_exceeds_members";
    if (group.high_temp_deci_c <= group.recovery_temp_deci_c) return "recovery_must_be_below_high";
    if (group.rotation_enabled && group.mode != GroupMode::Auto) return "rotation_requires_auto";
    if (group.ventilation_reserve && group.mode != GroupMode::Auto) return "ventilation_reserve_requires_auto";
    if (group.ventilation_reserve && group.ac_mode == Mode::Fan) return "ventilation_reserve_invalid_mode";
    if (group.rotation_enabled && group.handover_s > 0 &&
        group.maximum_active < group.required_active + 1) return "rotation_needs_parallel_capacity";
    for (std::size_t i = 0; i < group.members.size(); ++i) {
        const Address address = group.members[i];
        if (!validAddress(address)) return "invalid_group_members";
        for (std::size_t j = 0; j < i; ++j)
            if (group.members[j] == address) return "duplicate_group_member";
        bool registered = false;
        for (const auto& ac : config.registry) if (ac.address == address) { registered = true; break; }
        if (!registered) return "group_member_not_registered";
        for (const auto& other : config.groups) if (other.id != group.id)
            for (const Address existing : other.members)
                if (existing == address) return "group_member_in_another_group";
    }
    return nullptr;
}
}

struct WebService::Impl {
    IStateView& view; Config& config; ISettingsService& settings; IRegistryService& registry;
    ICommandSink& commands; IAuthService& auth;
    IOtaService& ota; EventLog& log; IClock& clock; WebServer server{80};
    Config settings_scratch{};
    MonotonicMs now{0}; bool restart_pending{false}; bool ota_failed{false}; TransactionId next_transaction_id_{1};
    Impl(IStateView& v, Config& c, ISettingsService& s, IRegistryService& r, ICommandSink& q,
         IAuthService& a, IOtaService& o, EventLog& l, IClock& k)
        : view(v), config(c), settings(s), registry(r), commands(q), auth(a), ota(o), log(l), clock(k) {}
    std::string token() { return cookieToken(server); }
    bool requireAuth() {
        if (auth.authorized(token(), now)) return true;
        server.send(401, "application/json", "{\"error\":\"authentication_required\"}"); return false;
    }
    bool requireMutation() {
        if (!requireAuth()) return false;
        if (!auth.validCsrf(token(), server.header("X-CSRF-Token").c_str(), now)) {
            server.send(403, "application/json", "{\"error\":\"csrf\"}"); return false;
        }
        return true;
    }
    void sendState(bool pretty) {
        if (!requireAuth()) return;
        server.sendHeader("Cache-Control", "no-store");
        const auto& state = view.read();
        String out; out.reserve(11000); out += "{\"schema_version\":1,\"device\":{\"name\":\"FL_Energy_ACMonitor\",\"version\":\"";
        out += kFirmwareVersion; out += "\",\"uptime_s\":"; out += String(state.clock.uptime_ms / 1000);
        out += ",\"time_valid\":"; out += state.clock.epoch.has_value() ? "true" : "false";
        out += ",\"time_local\":";
        if (state.clock.epoch) {
            std::time_t raw = static_cast<std::time_t>(*state.clock.epoch); std::tm tm{}; localtime_r(&raw, &tm);
            char timebuf[40]{}; std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S%z", &tm);
            String iso(timebuf); if (iso.length() >= 5) iso = iso.substring(0, iso.length()-2) + ":" + iso.substring(iso.length()-2);
            jsonString(out, iso.c_str());
        } else out += "null";
        out += "},\"network\":{\"link\":"; out += state.network.link ? "true" : "false";
        out += ",\"dhcp\":"; out += state.network.dhcp ? "true" : "false";
        out += ",\"ip\":"; if (state.network.ip) jsonString(out, ipText(*state.network.ip).c_str()); else out += "null";
        out += ",\"config_ip\":"; jsonString(out, ipText(config.network.ip).c_str());
        out += ",\"config_mask\":"; jsonString(out, ipText(config.network.mask).c_str());
        out += ",\"config_gateway\":"; jsonString(out, ipText(config.network.gateway).c_str());
        out += ",\"config_dns\":"; jsonString(out, ipText(config.network.dns).c_str()); out += "},\"time\":{\"server\":";
        jsonString(out, config.time.server.view()); out += ",\"timezone\":"; jsonString(out, config.time.timezone.view());
        out += "},\"management\":{\"snmp_enabled\":"; out += config.snmp.enabled ? "true" : "false";
        out += ",\"community_configured\":"; out += config.snmp.ro_community.view().empty() ? "false" : "true";
        out += ",\"system_name\":"; jsonString(out, config.snmp.system_name.view());
        out += ",\"location\":"; jsonString(out, config.snmp.location.view());
        out += ",\"contact\":"; jsonString(out, config.snmp.contact.view());
        out += ",\"trap_receiver\":";
        if (config.snmp.trap_receiver) jsonString(out, ipText(*config.snmp.trap_receiver).c_str()); else out += "null";
        out += ",\"trap_port\":"; out += String(config.snmp.trap_port);
        out += ",\"mqtt_enabled\":"; out += config.mqtt.enabled ? "true" : "false";
        out += ",\"broker\":"; jsonString(out, config.mqtt.broker.view());
        out += ",\"mqtt_port\":"; out += String(config.mqtt.port);
        out += ",\"mqtt_username_configured\":"; out += config.mqtt.username.view().empty() ? "false" : "true";
        out += ",\"mqtt_password_configured\":"; out += config.mqtt.password.view().empty() ? "false" : "true";
        out += ",\"topic_prefix\":"; jsonString(out, config.mqtt.topic_prefix.view());
        out += ",\"publish_period_s\":"; out += String(config.mqtt.publish_period_s);
        out += "},\"bus\":{\"baud\":"; out += String(state.bus.baud); out += ",\"poll_ok\":"; out += String(state.bus.poll_ok);
        out += ",\"timeouts\":"; out += String(state.bus.timeouts); out += ",\"checksum_errors\":"; out += String(state.bus.checksum_errors);
        out += ",\"rejected_frames\":"; out += String(state.bus.rejected_frames); out += ",\"scan_running\":";
        out += state.bus.scan_running ? "true" : "false"; out += ",\"scan_completed\":"; out += String(state.bus.scan_completed);
        out += ",\"discovered\":["; bool first = true; for (std::size_t i = 0; i < kAddressCount; ++i) if (state.bus.discovered.test(i)) {
            if (!first) out += ',';
            first = false; out += String(static_cast<unsigned>(i));
        } out += "]},\"automation\":{\"enabled\":";
        out += config.automatic_control_enabled ? "true" : "false";
        out += ",\"observe_only\":"; out += state.observe_only ? "true" : "false";
        out += "},\"conditioners\":[";
        for (std::size_t i = 0; i < state.conditioners.size(); ++i) {
            const auto& ac = state.conditioners[i]; if (i) out += ',';
            out += "{\"address\":"; out += String(static_cast<unsigned>(ac.address)); out += ",\"name\":";
            jsonString(out, ac.name.view()); out += ",\"availability\":";
            const char* availability = ac.availability == Availability::Online ? "online" : ac.availability == Availability::Offline ? "offline" : "unknown";
            jsonString(out, availability);
        const bool fresh = ac.availability == Availability::Online && ac.freshness == Freshness::Fresh;
        out += ",\"fresh\":"; out += fresh ? "true" : "false";
            out += ",\"automation_health\":";
            const auto health_issue = automationHealthIssue(config, ac, state.clock.uptime_ms);
            jsonString(out, health_issue.empty() ? std::string_view{"healthy"} : health_issue);
            out += ",\"consecutive_poll_failures\":"; out += String(static_cast<unsigned>(ac.consecutive_failures));
            out += ",\"power\":"; if (ac.observed.power.value.has_value()) out += *ac.observed.power.value ? "true" : "false"; else out += "null"; out += ",\"maintenance\":"; out += ac.maintenance ? "true" : "false"; out += ",\"manual_control\":"; out += ac.maintenance ? "true" : "false";
            out += ",\"fan_actual\":";
        if (fresh && ac.observed.fan_actual.value)
                out += String(static_cast<unsigned>(*ac.observed.fan_actual.value));
            else out += "null";
        out += ",\"fan_auto\":";
        if (fresh && ac.observed.fan_auto.value) out += *ac.observed.fan_auto.value ? "true" : "false";
        else out += "null";
        out += ",\"compressor_running\":";
        if (fresh && ac.observed.compressor_running.value) out += *ac.observed.compressor_running.value ? "true" : "false";
        else out += "null";
            out += ",\"model_validated\":"; out += ac.model_validated ? "true" : "false";
            out += ",\"group_id\":"; if (ac.group_id) out += String(*ac.group_id); else out += "null";
            out += ",\"last_seen_ms\":"; if (ac.last_seen_ms) out += String(static_cast<unsigned long long>(*ac.last_seen_ms)); else out += "null";
            out += ",\"mode\":"; if (ac.observed.mode.value) out += String(static_cast<unsigned>(*ac.observed.mode.value)); else out += "null";
            out += ",\"mode_status\":"; jsonString(out, ac.observed.mode.status == ValueStatus::Experimental ? "experimental" : ac.observed.mode.status == ValueStatus::Valid ? "valid" : "unknown");
            out += ",\"setpoint_deci_c\":"; if (ac.observed.setpoint_deci_c.value) out += String(*ac.observed.setpoint_deci_c.value); else out += "null";
            out += ",\"t1_deci_c\":"; if (fresh && ac.observed.t1_deci_c.value) out += String(*ac.observed.t1_deci_c.value); else out += "null";
            out += ",\"t2_deci_c\":"; if (fresh && ac.observed.t2_deci_c.value) out += String(*ac.observed.t2_deci_c.value); else out += "null";
            out += ",\"t2b_deci_c\":"; if (fresh && ac.observed.t2b_deci_c.value) out += String(*ac.observed.t2b_deci_c.value); else out += "null";
            out += ",\"t3_deci_c\":"; if (fresh && ac.observed.t3_deci_c.value) out += String(*ac.observed.t3_deci_c.value); else out += "null";
            out += ",\"error_flags\":"; if (fresh && ac.observed.error_flags.value) out += String(*ac.observed.error_flags.value); else out += "null";
            out += ",\"protection_flags\":"; if (fresh && ac.observed.protection_flags.value) out += String(*ac.observed.protection_flags.value); else out += "null";
            out += ",\"ccm_error_flags\":"; if (fresh && ac.observed.ccm_error_flags.value) out += String(static_cast<unsigned>(*ac.observed.ccm_error_flags.value)); else out += "null";
            out += ",\"temperature_status\":"; jsonString(out, ac.observed.setpoint_deci_c.status == ValueStatus::Experimental ? "experimental" : ac.observed.setpoint_deci_c.status == ValueStatus::Valid ? "valid" : "unknown");
            out += ",\"desired\":{\"power\":";
            if (ac.desired.settings.power) out += *ac.desired.settings.power ? "true" : "false"; else out += "null";
            out += ",\"mode\":";
            if (ac.desired.settings.mode) out += String(static_cast<unsigned>(*ac.desired.settings.mode)); else out += "null";
            out += ",\"fan\":";
            if (ac.desired.settings.fan) out += String(static_cast<unsigned>(*ac.desired.settings.fan)); else out += "null";
            out += ",\"setpoint_deci_c\":";
            if (ac.desired.settings.setpoint_deci_c) out += String(*ac.desired.settings.setpoint_deci_c); else out += "null";
            out += "},\"command_state\":";
            const char* command_state = ac.desired.command_state == CommandState::Pending ? "pending" :
                ac.desired.command_state == CommandState::Acknowledged ? "acknowledged" :
                ac.desired.command_state == CommandState::Failed ? "failed" : "idle";
            jsonString(out, command_state); out += ",\"transaction_id\":"; out += String(ac.desired.transaction_id);
            out += '}';
        }
        out += "],\"groups\":[";
        for (std::size_t i = 0; i < state.groups.size(); ++i) {
            const auto& group = state.groups[i]; if (i) out += ',';
            out += "{\"id\":"; out += String(group.id); out += ",\"name\":"; jsonString(out, group.name.view());
            out += ",\"mode\":"; jsonString(out, group.mode == GroupMode::Auto ? "auto" : "manual");
            out += ",\"state\":"; out += String(static_cast<unsigned>(group.state));
            out += ",\"required_active\":"; out += String(group.required_active);
            out += ",\"maximum_active\":"; out += String(group.maximum_active);
            out += ",\"ventilation_reserves\":["; bool first_ventilation = true;
            for (const auto member : group.members) if (group.ventilation_addresses.test(member)) {
                if (!first_ventilation) out += ',';
                first_ventilation = false; out += String(member);
            }
            out += ']';
            out += ",\"rotation_enabled\":"; out += group.rotation_enabled ? "true" : "false";
            out += ",\"emergency_active\":"; out += group.state == GroupState::Emergency ? "true" : "false";
            out += ",\"last_action_reason\":"; jsonString(out, group.last_action_reason.view());
            out += ",\"rotation_next_epoch\":";
            if (group.rotation_next_epoch) out += String(static_cast<long long>(*group.rotation_next_epoch)); else out += "null";
            out += ",\"rotation_seconds_remaining\":";
            if (group.rotation_next_epoch && state.clock.epoch) {
                const auto remaining = std::max<EpochSeconds>(0, *group.rotation_next_epoch - *state.clock.epoch);
                out += String(static_cast<unsigned long long>(remaining));
            } else out += "null";
            const GroupConfig* saved = nullptr;
            for (const auto& item : config.groups) if (item.id == group.id) { saved = &item; break; }
            if (saved) {
                out += ",\"ac_mode\":"; out += String(static_cast<unsigned>(saved->ac_mode));
                out += ",\"setpoint_c\":"; out += String(saved->setpoint_deci_c / 10);
                out += ",\"fan\":"; out += String(static_cast<unsigned>(saved->fan));
                out += ",\"ventilation_reserve\":"; out += saved->ventilation_reserve ? "true" : "false";
                out += ",\"color\":"; jsonString(out, colorText(saved->color_rgb).c_str());
                out += ",\"interval_days\":"; out += String(saved->interval_days);
                out += ",\"rotation_hour\":"; out += String(saved->rotation_hour);
                out += ",\"rotation_minute\":"; out += String(saved->rotation_minute);
                out += ",\"high_temp_c\":"; out += String(saved->high_temp_deci_c / 10);
                out += ",\"high_delay_s\":"; out += String(saved->high_delay_s);
                out += ",\"recovery_temp_c\":"; out += String(saved->recovery_temp_deci_c / 10);
                out += ",\"recovery_delay_s\":"; out += String(saved->recovery_delay_s);
                out += ",\"handover_s\":"; out += String(saved->handover_s);
                out += ",\"verify_s\":"; out += String(saved->start_verification_s);
                out += ",\"min_on_s\":"; out += String(saved->min_on_s);
                out += ",\"min_off_s\":"; out += String(saved->min_off_s);
                auto remainingDelay = [&](const std::optional<MonotonicMs>& since, std::uint32_t delay_s) -> std::optional<std::uint32_t> {
                    if (!since || state.clock.uptime_ms < *since) return std::nullopt;
                    const MonotonicMs total_ms = static_cast<MonotonicMs>(delay_s) * 1000;
                    const MonotonicMs elapsed_ms = state.clock.uptime_ms - *since;
                    if (elapsed_ms >= total_ms) return 0;
                    return static_cast<std::uint32_t>((total_ms - elapsed_ms + 999) / 1000);
                };
                const auto high_remaining = remainingDelay(group.high_since_ms, saved->high_delay_s);
                const auto recovery_remaining = remainingDelay(group.recovery_since_ms, saved->recovery_delay_s);
                out += ",\"high_delay_remaining_s\":";
                if (high_remaining) out += String(*high_remaining); else out += "null";
                out += ",\"recovery_delay_remaining_s\":";
                if (recovery_remaining) out += String(*recovery_remaining); else out += "null";
            }
            out += ",\"members\":[";
            for (std::size_t j = 0; j < group.members.size(); ++j) { if (j) out += ','; out += String(group.members[j]); }
            out += "],\"active\":["; bool first_active = true;
            for (const auto member : group.members) if (group.active_addresses.test(member)) {
                if (!first_active) out += ',';
                first_active = false; out += String(member);
            }
            out += "],\"temperature_deci_c\":";
            if (group.temperature_deci_c.value) out += String(*group.temperature_deci_c.value); else out += "null";
            out += ",\"phase\":"; out += String(static_cast<unsigned>(group.phase)); out += '}';
        }
        out += "],\"alarms\":[";
        for (std::size_t i = 0; i < state.alarms.size(); ++i) {
            const auto& alarm = state.alarms[i]; if (i) out += ',';
            out += "{\"type\":"; jsonString(out, alarm.type == AlarmType::AcOffline ? "acOffline" : alarm.type == AlarmType::AcFault ? "acFault" : alarm.type == AlarmType::GroupDegraded ? "groupDegraded" : "groupOverTemperature");
            out += ",\"severity\":"; jsonString(out, severityName(alarm.severity)); out += ",\"address\":";
            if (alarm.address) out += String(*alarm.address); else out += "null";
            out += ",\"group_id\":"; if (alarm.group_id) out += String(*alarm.group_id); else out += "null";
            out += ",\"code\":"; out += String(alarm.code); out += '}';
        }
        out += "]}";
        if (pretty) out = prettyJson(out);
        server.send(200, "application/json; charset=utf-8", out);
    }
    void sendLogs() {
        if (!requireAuth()) return;
        String out; out.reserve(10000); out = "{\"events\":[";
        const auto count = log.size(); const auto first = count > 50 ? count - 50 : 0;
        for (std::size_t i = first; i < count; ++i) {
            LogEvent entry; if (!log.get(i, entry)) continue;
            if (i != first) out += ',';
            out += "{\"uptime_ms\":"; out += String(static_cast<unsigned long long>(entry.uptime_ms));
            out += ",\"timestamp\":";
            if (entry.epoch) {
                const std::time_t raw = static_cast<std::time_t>(*entry.epoch); std::tm local{};
                char stamp[24]{};
                if (localtime_r(&raw, &local) && std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local)) jsonString(out, stamp);
                else out += "null";
            } else out += "null";
            out += ",\"severity\":"; jsonString(out, severityName(entry.severity));
            out += ",\"message\":";
            jsonString(out, entry.message.view()); out += '}';
        }
        out += "]}"; server.send(200, "application/json", out);
    }
    void sendManagementSecrets() {
        if (!requireAuth()) return;
        server.sendHeader("Cache-Control", "no-store");
        String out; out.reserve(512);
        out = "{\"snmp_community\":"; jsonString(out, config.snmp.ro_community.view());
        out += ",\"mqtt_username\":"; jsonString(out, config.mqtt.username.view());
        out += ",\"mqtt_password\":"; jsonString(out, config.mqtt.password.view());
        out += '}';
        server.send(200, "application/json; charset=utf-8", out);
    }
    bool validateNetwork(const NetworkConfig& n) {
        if (n.dhcp) return true;
        const std::uint32_t ip = (std::uint32_t(n.ip.octets[0])<<24)|(std::uint32_t(n.ip.octets[1])<<16)|(std::uint32_t(n.ip.octets[2])<<8)|n.ip.octets[3];
        const std::uint32_t mask = (std::uint32_t(n.mask.octets[0])<<24)|(std::uint32_t(n.mask.octets[1])<<16)|(std::uint32_t(n.mask.octets[2])<<8)|n.mask.octets[3];
        const std::uint32_t gw = (std::uint32_t(n.gateway.octets[0])<<24)|(std::uint32_t(n.gateway.octets[1])<<16)|(std::uint32_t(n.gateway.octets[2])<<8)|n.gateway.octets[3];
        if (ip == 0 || (ip & 0xf0000000U) == 0xe0000000U || (ip & ~mask) == 0 || (ip & ~mask) == ~mask) return false;
        const std::uint32_t inverse = ~mask; if ((inverse & (inverse + 1U)) != 0 || mask == 0) return false;
        return gw != 0 && gw != ip && (ip & mask) == (gw & mask);
    }
    void updateSettings() {
        if (!requireMutation()) return;
        settings_scratch = config;
        Config& next = settings_scratch;
        if (server.hasArg("dhcp")) next.network.dhcp = server.arg("dhcp") == "1";
        if (server.hasArg("automation_enabled")) next.automatic_control_enabled = server.arg("automation_enabled") == "1";
        if (next.automatic_control_enabled) {
            for (const auto& group : next.groups) if (group.mode == GroupMode::Auto)
                for (const auto address : group.members) {
                    bool validated = false;
                    for (const auto& ac : next.registry)
                        if (ac.address == address && ac.model_validated) validated = true;
                    if (!validated) { server.send(409, "application/json", "{\"error\":\"all_group_models_must_be_validated\"}"); return; }
                }
        }
        auto parseOptionalIp = [&](const char* key, Ipv4& destination) {
            return !server.hasArg(key) || server.arg(key).isEmpty() || parseIp(server.arg(key), destination);
        };
        if (!parseOptionalIp("ip", next.network.ip) || !parseOptionalIp("mask", next.network.mask) ||
            !parseOptionalIp("gateway", next.network.gateway) || !parseOptionalIp("dns", next.network.dns) ||
            (server.hasArg("ntp") && !next.time.server.assign(server.arg("ntp").c_str())) ||
            (server.hasArg("timezone") && !next.time.timezone.assign(server.arg("timezone").c_str())) ||
            !validateNetwork(next.network)) { server.send(400, "application/json", "{\"error\":\"invalid_settings\"}"); return; }

        if (server.hasArg("snmp_enabled")) next.snmp.enabled = server.arg("snmp_enabled") == "1";
        const String community = server.arg("snmp_community");
        if (server.hasArg("snmp_community") && !next.snmp.ro_community.assign(community.c_str())) {
            server.send(400, "application/json", "{\"error\":\"invalid_snmp_community\"}"); return;
        }
        const String sysName = server.arg("snmp_system_name"), sysLocation = server.arg("snmp_location"), sysContact = server.arg("snmp_contact");
        if ((server.hasArg("snmp_system_name") && !next.snmp.system_name.assign(sysName.c_str())) ||
            (server.hasArg("snmp_location") && !next.snmp.location.assign(sysLocation.c_str())) ||
            (server.hasArg("snmp_contact") && !next.snmp.contact.assign(sysContact.c_str()))) {
            server.send(400, "application/json", "{\"error\":\"invalid_snmp_system_info\"}"); return;
        }
        if (server.hasArg("trap_receiver")) {
            const String receiver = server.arg("trap_receiver");
            if (receiver.isEmpty()) next.snmp.trap_receiver.reset();
            else { Ipv4 parsed; if (!parseIp(receiver, parsed)) { server.send(400, "application/json", "{\"error\":\"invalid_trap_receiver\"}"); return; } next.snmp.trap_receiver = parsed; }
        }
        long port = 0;
        if (server.hasArg("trap_port")) {
            if (!parseLong(server.arg("trap_port"), port) || port < 1 || port > 65535) { server.send(400, "application/json", "{\"error\":\"invalid_trap_port\"}"); return; }
            next.snmp.trap_port = static_cast<std::uint16_t>(port);
        }
        if (server.hasArg("mqtt_enabled")) next.mqtt.enabled = server.arg("mqtt_enabled") == "1";
        const String broker = server.arg("mqtt_broker"), mqttUser = server.arg("mqtt_username"), mqttPass = server.arg("mqtt_password");
        const String topic = server.arg("mqtt_topic_prefix");
        if ((server.hasArg("mqtt_broker") && !broker.isEmpty() && !next.mqtt.broker.assign(broker.c_str())) ||
            (server.hasArg("mqtt_username") && !next.mqtt.username.assign(mqttUser.c_str())) ||
            (server.hasArg("mqtt_password") && !next.mqtt.password.assign(mqttPass.c_str())) ||
            (server.hasArg("mqtt_topic_prefix") && !topic.isEmpty() && !next.mqtt.topic_prefix.assign(topic.c_str()))) {
            server.send(400, "application/json", "{\"error\":\"invalid_mqtt_settings\"}"); return;
        }
        if (server.hasArg("mqtt_port")) {
            if (!parseLong(server.arg("mqtt_port"), port) || port < 1 || port > 65535) { server.send(400, "application/json", "{\"error\":\"invalid_mqtt_port\"}"); return; }
            next.mqtt.port = static_cast<std::uint16_t>(port);
        }
        if (server.hasArg("mqtt_publish_period_s")) {
            if (!parseLong(server.arg("mqtt_publish_period_s"), port) || port < 5 || port > 3600) { server.send(400, "application/json", "{\"error\":\"invalid_mqtt_period\"}"); return; }
            next.mqtt.publish_period_s = static_cast<std::uint32_t>(port);
        }
        const Result result = settings.updateConfig(next);
        if (result != Result::Accepted) { server.send(400, "application/json", "{\"error\":\"settings_rejected\"}"); return; }
        event(log, Severity::Info, "System, SNMP and MQTT settings updated"); server.send(200, "application/json", "{\"ok\":true}");
    }
    void scanXye() {
        if (!requireMutation()) return;
        const auto result = registry.startReadOnlyScan();
        if (result != Result::Accepted) { server.send(result == Result::Busy ? 409 : 400, "application/json", "{\"error\":\"scan_rejected\"}"); return; }
        event(log, Severity::Info, "Manual read-only XYE scan started");
        server.send(202, "application/json", "{\"ok\":true}");
    }
    void registerXye() {
        if (!requireMutation()) return;
        long address = -1;
        AcRegistration item;
        if (!parseLong(server.arg("address"), address) || address < 0 || address >= static_cast<long>(kAddressCount) ||
            !item.name.assign(server.arg("name").c_str())) { server.send(400, "application/json", "{\"error\":\"invalid_device\"}"); return; }
        item.address = static_cast<Address>(address);
        const auto result = registry.registerAc(item);
        if (result != Result::Accepted) { server.send(result == Result::Full ? 409 : 400, "application/json", "{\"error\":\"registration_rejected\"}"); return; }
        event(log, Severity::Info, "AC registered for read-only polling");
        server.send(200, "application/json", "{\"ok\":true}");
    }
    void removeXye() {
        if (!requireMutation()) return;
        long address = -1;
        if (!parseLong(server.arg("address"), address) || address < 0 || address >= static_cast<long>(kAddressCount)) {
            server.send(400, "application/json", "{\"error\":\"invalid_address\"}"); return;
        }
        const auto result = registry.removeAc(static_cast<Address>(address));
        if (result != Result::Accepted) {
            const int status = result == Result::Busy || result == Result::Forbidden ? 409 : result == Result::NotFound ? 404 : 400;
            server.send(status, "application/json", result == Result::Forbidden ?
                "{\"error\":\"device_in_group\"}" : "{\"error\":\"device_remove_rejected\"}"); return;
        }
        event(log, Severity::Info, "AC removed from registry");
        server.send(200, "application/json", "{\"ok\":true}");
    }
    void setManualControl() {
        if (!requireMutation()) return;
        long address = -1;
        if (!parseLong(server.arg("address"), address) || address < 0 || address >= static_cast<long>(kAddressCount)) {
            server.send(400, "application/json", "{\"error\":\"invalid_address\"}"); return;
        }
        const String value = server.arg("manual_control");
        if (value != "0" && value != "1") {
            server.send(400, "application/json", "{\"error\":\"invalid_manual_control\"}"); return;
        }
        const Address target = static_cast<Address>(address);
        GroupId group_id = 0;
        for (const auto& group : config.groups) if (group.mode == GroupMode::Auto)
            for (const auto member : group.members) if (member == target) group_id = group.id;
        if (!group_id) {
            server.send(409, "application/json", "{\"error\":\"manual_control_requires_auto_group\"}"); return;
        }
        for (const auto& group : view.read().groups) if (group.id == group_id) {
            if ((group.phase != HandoverPhase::Idle && group.phase != HandoverPhase::Aborted) ||
                group.settings_sync_address == target) {
                server.send(409, "application/json", "{\"error\":\"group_busy\"}"); return;
            }
        }
        for (const auto& ac : view.read().conditioners) if (ac.address == target &&
            ac.desired.command_state == CommandState::Pending) {
            server.send(409, "application/json", "{\"error\":\"command_busy\"}"); return;
        }
        AcRegistration item; bool found = false;
        for (const auto& registered : config.registry) if (registered.address == target) {
            item = registered; found = true; break;
        }
        if (!found) { server.send(404, "application/json", "{\"error\":\"device_not_registered\"}"); return; }
        const bool enabled = value == "1";
        if (item.maintenance == enabled) { server.send(200, "application/json", "{\"ok\":true}"); return; }
        item.maintenance = enabled;
        const Result result = registry.updateAc(item);
        if (result != Result::Accepted) {
            server.send(result == Result::Busy ? 409 : 400, "application/json", "{\"error\":\"manual_control_save_rejected\"}"); return;
        }
        event(log, enabled ? Severity::Warning : Severity::Info,
            enabled ? "Manual control enabled for AUTO group conditioner" :
                      "AUTO group control restored for conditioner");
        server.send(200, "application/json", "{\"ok\":true}");
    }
    void upsertGroup() {
        if (!requireMutation()) return;
        GroupConfig group; long value = 0;
        if (!parseLong(server.arg("id"), value) || value < 0 || value > 65535) {
            server.send(400, "application/json", "{\"error\":\"invalid_group_id\"}"); return;
        }
        group.id = static_cast<GroupId>(value);
        for (const auto& runtime : view.read().groups)
            if (runtime.id == group.id && (runtime.phase != HandoverPhase::Idle || runtime.settings_sync_address)) {
                server.send(409, "application/json", "{\"error\":\"group_busy\"}"); return;
            }
        if (!group.id) {
            GroupId next = 1;
            for (const auto& existing : config.groups) if (existing.id >= next) next = existing.id + 1;
            if (!next) { server.send(409, "application/json", "{\"error\":\"group_id_exhausted\"}"); return; }
            group.id = next;
        }
        const String name = server.arg("name");
        if (!group.name.assign(name.c_str())) { server.send(400, "application/json", "{\"error\":\"invalid_group_name\"}"); return; }
        const String mode = server.arg("mode");
        if (mode == "auto") group.mode = GroupMode::Auto;
        else if (mode != "manual") { server.send(400, "application/json", "{\"error\":\"invalid_group_mode\"}"); return; }
        auto readNumber = [&](const char* key, long min, long max, long& destination) {
            return parseLong(server.arg(key), destination) && destination >= min && destination <= max;
        };
        long required = 0, maximum = 0, setpoint = 0, interval = 0, hour = 0, minute = 0;
        long high = 0, high_delay = 0, recovery = 0, recovery_delay = 0, handover = 0;
        long verify = 0, min_on = 0, min_off = 0;
        if (!readNumber("required_active", 1, static_cast<long>(kMaxAc), required) ||
            !readNumber("maximum_active", 1, static_cast<long>(kMaxAc), maximum) ||
            !readNumber("setpoint_c", 16, 30, setpoint) || !readNumber("interval_days", 1, 3650, interval) ||
            !readNumber("rotation_hour", 0, 23, hour) || !readNumber("rotation_minute", 0, 59, minute) ||
            !readNumber("high_temp_c", 16, 60, high) || !readNumber("high_delay_s", 1, 86400, high_delay) ||
            !readNumber("recovery_temp_c", 0, 59, recovery) || !readNumber("recovery_delay_s", 1, 86400, recovery_delay) ||
            !readNumber("handover_s", 0, 86400, handover) || !readNumber("verify_s", 1, 3600, verify) ||
            !readNumber("min_on_s", 1, 604800, min_on) || !readNumber("min_off_s", 1, 604800, min_off)) {
            server.send(400, "application/json", "{\"error\":\"invalid_group_settings\"}"); return;
        }
        group.required_active = static_cast<std::uint8_t>(required);
        group.maximum_active = static_cast<std::uint8_t>(maximum);
        group.setpoint_deci_c = static_cast<std::int16_t>(setpoint * 10);
        group.interval_days = static_cast<std::uint16_t>(interval);
        group.rotation_hour = static_cast<std::uint8_t>(hour); group.rotation_minute = static_cast<std::uint8_t>(minute);
        group.high_temp_deci_c = static_cast<std::int16_t>(high * 10); group.high_delay_s = static_cast<std::uint32_t>(high_delay);
        group.recovery_temp_deci_c = static_cast<std::int16_t>(recovery * 10);
        group.recovery_delay_s = static_cast<std::uint32_t>(recovery_delay);
        group.handover_s = static_cast<std::uint32_t>(handover); group.start_verification_s = static_cast<std::uint32_t>(verify);
        group.min_on_s = static_cast<std::uint32_t>(min_on); group.min_off_s = static_cast<std::uint32_t>(min_off);
        const String acMode = server.arg("ac_mode");
        if (acMode == "cool") group.ac_mode = Mode::Cool;
        else if (acMode == "heat") group.ac_mode = Mode::Heat; else if (acMode == "fan") group.ac_mode = Mode::Fan;
        else if (acMode == "dry") group.ac_mode = Mode::Dry;
        else { server.send(400, "application/json", "{\"error\":\"invalid_ac_mode\"}"); return; }
        const String fan = server.arg("fan");
        if (fan == "auto") group.fan = Fan::Auto; else if (fan == "low") group.fan = Fan::Low;
        else if (fan == "medium") group.fan = Fan::Medium; else if (fan == "high") group.fan = Fan::High;
        else { server.send(400, "application/json", "{\"error\":\"invalid_fan\"}"); return; }
        group.rotation_enabled = server.arg("rotation_enabled") == "1";
        group.ventilation_reserve = server.arg("ventilation_reserve") == "1";
        if (!parseColor(server.arg("color"), group.color_rgb)) {
            server.send(400, "application/json", "{\"error\":\"invalid_group_color\"}"); return;
        }
        const String list = server.arg("members"); int start = 0;
        while (start < list.length()) {
            int end = list.indexOf(',', start); if (end < 0) end = list.length();
            String part = list.substring(start, end); part.trim(); long address = -1;
            if (!parseLong(part, address) || address < 0 || address >= static_cast<long>(kAddressCount) ||
                !group.members.push_back(static_cast<Address>(address))) {
                server.send(400, "application/json", "{\"error\":\"invalid_group_members\"}"); return;
            }
            start = end + 1;
        }
        for (const auto& existing : config.groups) if (existing.id == group.id &&
            existing.members.size() == group.members.size()) {
            bool unchanged = true;
            for (std::size_t i = 0; i < group.members.size(); ++i)
                if (existing.members[i] != group.members[i]) unchanged = false;
            if (unchanged && existing.mode == group.mode && existing.rotation_enabled == group.rotation_enabled)
                group.last_rotation_anchor = existing.last_rotation_anchor;
        }
        if (const char* error = groupValidationError(config, group)) {
            String body = "{\"error\":\""; body += error; body += "\"}";
            server.send(400, "application/json", body); return;
        }
        const auto result = settings.putGroup(group);
        if (result != Result::Accepted) {
            const int status = result == Result::Full || result == Result::Busy ? 409 : result == Result::NotFound ? 404 : 400;
            const char* error = result == Result::Full ? "group_limit_reached" :
                result == Result::Busy ? "group_busy" : result == Result::NotFound ? "group_member_not_registered" :
                result == Result::Invalid ? "invalid_group_settings" : "group_save_rejected";
            String body = "{\"error\":\""; body += error; body += "\"}";
            server.send(status, "application/json", body); return;
        }
        event(log, Severity::Info, "Group settings saved");
        String body = "{\"ok\":true,\"id\":"; body += String(group.id); body += '}';
        server.send(200, "application/json", body);
    }
    void deleteGroup() {
        if (!requireMutation()) return;
        long id = 0;
        if (!parseLong(server.arg("id"), id) || id <= 0 || id > 65535) {
            server.send(400, "application/json", "{\"error\":\"invalid_group_id\"}"); return;
        }
        for (const auto& group : view.read().groups) if (group.id == id &&
            (group.phase != HandoverPhase::Idle || group.settings_sync_address)) {
            server.send(409, "application/json", "{\"error\":\"group_busy\"}"); return;
        }
        const auto result = settings.deleteGroup(static_cast<GroupId>(id));
        if (result != Result::Accepted) { server.send(400, "application/json", "{\"error\":\"group_delete_rejected\"}"); return; }
        event(log, Severity::Warning, "Group removed"); server.send(200, "application/json", "{\"ok\":true}");
    }
    void validateXyeModel() {
        if (!requireMutation()) return;
        long address = -1;
        if (!parseLong(server.arg("address"), address) || address < 0 || address >= static_cast<long>(kAddressCount)) {
            server.send(400, "application/json", "{\"error\":\"invalid_address\"}"); return;
        }
        const auto& state = view.read();
        const AcState* observed = nullptr;
        for (const auto& ac : state.conditioners) if (ac.address == static_cast<Address>(address)) { observed = &ac; break; }
        if (!observed || observed->availability != Availability::Online || observed->freshness != Freshness::Fresh ||
            !observed->last_seen_ms || now < *observed->last_seen_ms || now - *observed->last_seen_ms > 2 * config.bus.poll_interval_ms) {
            server.send(409, "application/json", "{\"error\":\"device_not_fresh\"}"); return;
        }
        AcRegistration item; bool found = false;
        for (const auto& registered : config.registry) if (registered.address == static_cast<Address>(address)) {
            item = registered; found = true; break;
        }
        if (!found) { server.send(404, "application/json", "{\"error\":\"device_not_registered\"}"); return; }
        item.model_validated = true;
        const Result result = registry.updateAc(item);
        if (result != Result::Accepted) { server.send(400, "application/json", "{\"error\":\"validation_rejected\"}"); return; }
        event(log, Severity::Warning, "Owner enabled manual control after model verification");
        server.send(200, "application/json", "{\"ok\":true}");
    }
    void controlXye() {
        if (!requireMutation()) return;
        long address = -1;
        if (!parseLong(server.arg("address"), address) || address < 0 || address >= static_cast<long>(kAddressCount)) {
            server.send(400, "application/json", "{\"error\":\"invalid_address\"}"); return;
        }
        const String power = server.arg("power");
        if (power != "0" && power != "1") { server.send(400, "application/json", "{\"error\":\"invalid_power\"}"); return; }
        ControlRequest request; request.address = static_cast<Address>(address); request.origin = CommandOrigin::Web;
        request.transaction_id = next_transaction_id_++;
        if (!request.transaction_id) request.transaction_id = next_transaction_id_++;
        request.settings.power = power == "1";
        if (*request.settings.power) {
            const String mode = server.arg("mode");
            if (mode == "cool") request.settings.mode = Mode::Cool;
            else if (mode == "heat") request.settings.mode = Mode::Heat;
            else if (mode == "fan") request.settings.mode = Mode::Fan;
            else if (mode == "dry") request.settings.mode = Mode::Dry;
            else { server.send(400, "application/json", "{\"error\":\"invalid_mode\"}"); return; }
            const String fan = server.arg("fan");
            if (fan == "auto") request.settings.fan = Fan::Auto;
            else if (fan == "low") request.settings.fan = Fan::Low;
            else if (fan == "medium") request.settings.fan = Fan::Medium;
            else if (fan == "high") request.settings.fan = Fan::High;
            else { server.send(400, "application/json", "{\"error\":\"invalid_fan\"}"); return; }
            if (*request.settings.mode != Mode::Fan) {
                long setpoint = 0;
                if (!parseLong(server.arg("setpoint_deci_c"), setpoint) || setpoint < 0 || setpoint > 2550) {
                    server.send(400, "application/json", "{\"error\":\"invalid_setpoint\"}"); return;
                }
                request.settings.setpoint_deci_c = static_cast<std::int16_t>(setpoint);
            }
        }
        const Result result = commands.enqueue(request);
        if (result != Result::Accepted) {
            const int status = result == Result::Forbidden ? 403 :
                result == Result::Busy || result == Result::Full ? 409 : result == Result::NotFound ? 404 : 400;
            server.send(status, "application/json", "{\"error\":\"control_rejected\"}"); return;
        }
        event(log, Severity::Info, "Manual XYE command queued");
        String body = "{\"ok\":true,\"transaction_id\":"; body += String(request.transaction_id); body += '}';
        server.send(202, "application/json", body);
    }
    void setup() {
        const char* headers[] = {"Cookie", "X-CSRF-Token"}; server.collectHeaders(headers, 2);
        server.on("/", HTTP_GET, [this] { server.send_P(200, "text/html; charset=utf-8", kEmbeddedIndexHtml); });
        server.on("/mib", HTTP_GET, [this] {
            if (!requireAuth()) return;
            server.sendHeader("Cache-Control", "no-store");
            server.sendHeader("Content-Disposition", "attachment; filename=FL-ENERGY-ACMONITOR-MIB.mib");
            server.send_P(200, "text/plain; charset=utf-8", kEmbeddedMib);
        });
        server.on("/json", HTTP_GET, [this] { sendState(true); });
        server.on("/api/v1/state", HTTP_GET, [this] { sendState(false); });
        server.on("/api/v1/auth/login", HTTP_POST, [this] {
            AuthSession session;
            if (auth.login(server.arg("username").c_str(), server.arg("password").c_str(), now, session) != Result::Accepted) {
                event(log, Severity::Warning, "Authentication failed"); server.send(401, "application/json", "{\"error\":\"invalid_credentials\"}"); return;
            }
            String cookie = "fl_session="; cookie += session.cookie.c_str(); cookie += "; Path=/; HttpOnly; SameSite=Strict; Max-Age="; cookie += String(config.auth.session_lifetime_s);
            server.sendHeader("Set-Cookie", cookie); event(log, Severity::Info, "Authentication successful"); server.send(200, "application/json", "{\"ok\":true}");
        });
        server.on("/api/v1/auth/me", HTTP_GET, [this] {
            if (!requireAuth()) return;
            CsrfToken csrf;
            if (!auth.csrfToken(token(), now, csrf)) { server.send(401, "application/json", "{}"); return; }
            String body = "{\"csrf\":"; jsonString(body, csrf.view()); body += '}';
            server.sendHeader("Cache-Control", "no-store");
            server.send(200, "application/json", body);
        });
        server.on("/api/v1/auth/logout", HTTP_POST, [this] { if (!requireMutation()) return; auth.logout(token()); server.sendHeader("Set-Cookie", "fl_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"); server.send(200, "application/json", "{\"ok\":true}"); });
        server.on("/api/v1/auth/password", HTTP_POST, [this] { if (!requireMutation()) return; const auto r = auth.changePassword(server.arg("username").c_str(), server.arg("password").c_str()); server.send(r == Result::Accepted ? 200 : 400, "application/json", r == Result::Accepted ? "{\"ok\":true}" : "{\"error\":\"invalid_password\"}"); });
        server.on("/api/v1/settings", HTTP_POST, [this] { updateSettings(); });
        server.on("/api/v1/ac/scan", HTTP_POST, [this] { scanXye(); });
        server.on("/api/v1/ac/register", HTTP_POST, [this] { registerXye(); });
        server.on("/api/v1/ac/remove", HTTP_POST, [this] { removeXye(); });
        server.on("/api/v1/ac/manual-control", HTTP_POST, [this] { setManualControl(); });
        server.on("/api/v1/groups/upsert", HTTP_POST, [this] { upsertGroup(); });
        server.on("/api/v1/groups/delete", HTTP_POST, [this] { deleteGroup(); });
        server.on("/api/v1/ac/validate", HTTP_POST, [this] { validateXyeModel(); });
        server.on("/api/v1/ac/control", HTTP_POST, [this] { controlXye(); });
        server.on("/api/v1/log", HTTP_GET, [this] { sendLogs(); });
        server.on("/api/v1/management/secrets", HTTP_GET, [this] { sendManagementSecrets(); });
        server.on("/api/v1/ota", HTTP_POST, [this] {
            if (ota_failed || ota.state() != OtaState::ReadyToReboot) {
                server.send(500, "application/json", "{\"error\":\"ota_failed\"}"); return;
            }
            server.send(200, "application/json", "{\"ok\":true}"); restart_pending = true;
        }, [this] {
            HTTPUpload& upload = server.upload();
            if (upload.status == UPLOAD_FILE_START) {
                ota_failed = false;
                if (!auth.authorized(token(), now) || !auth.validCsrf(token(), server.header("X-CSRF-Token").c_str(), now)) {
                    ota.abort(); ota_failed = true; upload.status = UPLOAD_FILE_ABORTED; return;
                }
                if (upload.filename != "firmware.bin" || ota.begin(ESP.getFreeSketchSpace()) != Result::Accepted) { ota.abort(); ota_failed = true; upload.status = UPLOAD_FILE_ABORTED; }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (ota.write(upload.buf, upload.currentSize) != Result::Accepted) { ota_failed = true; upload.status = UPLOAD_FILE_ABORTED; }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (ota.finish() != Result::Accepted) { ota_failed = true; upload.status = UPLOAD_FILE_ABORTED; }
            } else if (upload.status == UPLOAD_FILE_ABORTED) { ota.abort(); ota_failed = true; }
        });
        server.onNotFound([this] { server.send(404, "application/json", "{\"error\":\"not_found\"}"); });
        server.begin();
    }
};

WebService::WebService(IStateView& view, Config& config, ISettingsService& settings,
                       IRegistryService& registry, ICommandSink& commands, IAuthService& auth,
                       IOtaService& ota, EventLog& log, IClock& clock)
    : impl_(new Impl(view, config, settings, registry, commands, auth, ota, log, clock)) {}
Result WebService::begin() { impl_->setup(); return Result::Accepted; }
void WebService::tick(MonotonicMs now) {
    impl_->now = now; impl_->server.handleClient();
    if (impl_->restart_pending && impl_->ota.state() == OtaState::ReadyToReboot) { delay(700); ESP.restart(); }
}
} // namespace fl
