#include "fl/mqtt/Telemetry.h"
#include <cstdio>
#include <cstring>

namespace fl {
namespace {
class Writer {
public:
    Writer(char* output, std::size_t capacity) : output_(output), capacity_(capacity) {
        if (capacity_) output_[0] = '\0';
    }
    bool raw(const char* text) { return append(text, std::strlen(text)); }
    bool number(long long value) {
        char text[32]; const int size = std::snprintf(text, sizeof(text), "%lld", value);
        return size > 0 && append(text, static_cast<std::size_t>(size));
    }
    bool quoted(std::string_view value) {
        if (!raw("\"")) return false;
        for (const unsigned char c : value) {
            char text[7]; std::size_t size = 0;
            if (c == '"' || c == '\\') { text[0] = '\\'; text[1] = static_cast<char>(c); size = 2; }
            else if (c < 0x20) { const int count = std::snprintf(text, sizeof(text), "\\u%04x", c); size = count > 0 ? static_cast<std::size_t>(count) : 0; }
            else { text[0] = static_cast<char>(c); size = 1; }
            if (!append(text, size)) return false;
        }
        return raw("\"");
    }
    std::size_t size() const { return length_; }
    bool valid() const { return valid_; }
private:
    bool append(const char* data, std::size_t size) {
        if (!valid_ || !capacity_ || length_ >= capacity_ || size >= capacity_ - length_) { valid_ = false; return false; }
        std::memcpy(output_ + length_, data, size); length_ += size; output_[length_] = '\0'; return true;
    }
    char* output_; std::size_t capacity_; std::size_t length_{0}; bool valid_{true};
};
void addSample(Writer& writer, bool fresh, const Sample<bool>& sample) {
    if (fresh && sample.confirmed()) writer.raw(*sample.value ? "true" : "false"); else writer.raw("null");
}
template <typename T> void addSample(Writer& writer, bool fresh, const Sample<T>& sample) {
    if (fresh && sample.confirmed()) writer.number(static_cast<long long>(*sample.value)); else writer.raw("null");
}
const char* alarmType(AlarmType type) {
    switch (type) {
    case AlarmType::AcOffline: return "acOffline";
    case AlarmType::AcFault: return "acFault";
    case AlarmType::GroupDegraded: return "groupDegraded";
    case AlarmType::GroupOverTemperature: return "groupOverTemperature";
    }
    return "unknown";
}
Result finish(Writer& writer, std::size_t& written) {
    written = writer.size(); return writer.valid() ? Result::Accepted : Result::Full;
}
}

Result encodeMqttState(const RuntimeState& state, MonotonicMs now, char* output, std::size_t capacity, std::size_t& written) {
    written = 0; if (!output || !capacity) return Result::Invalid;
    Writer out(output, capacity);
    out.raw("{\"version\":"); out.quoted(kFirmwareVersion); out.raw(",\"uptime_s\":"); out.number(static_cast<long long>(now / 1000));
    out.raw(",\"time_valid\":"); out.raw(state.clock.epoch ? "true" : "false");
    if (state.clock.epoch) { out.raw(",\"epoch\":"); out.number(*state.clock.epoch); }
    out.raw(",\"conditioners\":["); bool first = true;
    for (const auto& ac : state.conditioners) {
        if (!first) out.raw(",");
        first = false;
        const bool fresh = ac.availability == Availability::Online && ac.freshness == Freshness::Fresh;
        out.raw("{\"address\":"); out.number(ac.address); out.raw(",\"name\":"); out.quoted(ac.name.view());
        out.raw(",\"availability\":"); out.quoted(ac.availability == Availability::Online ? "online" : ac.availability == Availability::Offline ? "offline" : "unknown");
        out.raw(",\"fresh\":"); out.raw(fresh ? "true" : "false"); out.raw(",\"power\":"); addSample(out, fresh, ac.observed.power);
        out.raw(",\"mode\":"); addSample(out, fresh, ac.observed.mode); out.raw(",\"setpoint_deci_c\":"); addSample(out, fresh, ac.observed.setpoint_deci_c);
        out.raw(",\"fan_actual\":"); addSample(out, fresh, ac.observed.fan_actual);
        out.raw(",\"fan_auto\":"); addSample(out, fresh, ac.observed.fan_auto);
        out.raw(",\"compressor_running\":"); addSample(out, fresh, ac.observed.compressor_running);
        out.raw(",\"t1_deci_c\":"); addSample(out, fresh, ac.observed.t1_deci_c);
        out.raw(",\"t2_deci_c\":"); addSample(out, fresh, ac.observed.t2_deci_c);
        out.raw(",\"t2b_deci_c\":"); addSample(out, fresh, ac.observed.t2b_deci_c);
        out.raw(",\"t3_deci_c\":"); addSample(out, fresh, ac.observed.t3_deci_c);
        out.raw(",\"error_flags\":"); addSample(out, fresh, ac.observed.error_flags);
        out.raw(",\"protection_flags\":"); addSample(out, fresh, ac.observed.protection_flags); out.raw("}");
    }
    out.raw("],\"groups\":["); first = true;
    for (const auto& group : state.groups) {
        if (!first) out.raw(",");
        first = false;
        out.raw("{\"id\":"); out.number(group.id); out.raw(",\"name\":"); out.quoted(group.name.view());
        out.raw(",\"state\":"); out.number(static_cast<unsigned>(group.state));
        out.raw(",\"active_count\":"); out.number(static_cast<long long>(group.active_addresses.count()));
        out.raw(",\"temperature_deci_c\":"); addSample(out, true, group.temperature_deci_c); out.raw("}");
    }
    out.raw("]}"); return finish(out, written);
}

Result encodeMqttEvent(const AlarmEvent& event, char* output, std::size_t capacity, std::size_t& written) {
    written = 0; if (!output || !capacity) return Result::Invalid;
    Writer out(output, capacity);
    out.raw("{\"type\":"); out.quoted(alarmType(event.alarm.type)); out.raw(",\"state\":");
    out.quoted(event.alarm.state == AlarmState::Problem ? "PROBLEM" : "RECOVERY");
    out.raw(",\"severity\":"); out.quoted(event.alarm.severity == Severity::Critical ? "CRITICAL" : event.alarm.severity == Severity::Warning ? "WARNING" : "INFO");
    out.raw(",\"ac_name\":"); out.quoted(event.ac_name.view()); out.raw(",\"group_name\":"); out.quoted(event.group_name.view());
    out.raw(",\"text\":"); out.quoted(event.text.view()); out.raw(",\"code\":"); out.number(event.alarm.code);
    if (event.alarm.epoch) { out.raw(",\"epoch\":"); out.number(*event.alarm.epoch); }
    out.raw("}"); return finish(out, written);
}
} // namespace fl
