#include "fl/mqtt/Mqtt.h"
#include "fl/mqtt/Telemetry.h"
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <ETH.h>
#include <ESP.h>
#include <cstdio>

namespace fl {
namespace {
bool topicPrefixValid(std::string_view value) {
    if (value.empty() || value.size() > 96 || value.back() == '/') return false;
    for (const unsigned char c : value) if (c < 0x21 || c == '#' || c == '+') return false;
    return true;
}
bool sameConfig(const MqttConfig& a, const MqttConfig& b) {
    return a.enabled == b.enabled && a.broker.view() == b.broker.view() && a.port == b.port &&
        a.username.view() == b.username.view() && a.password.view() == b.password.view() &&
        a.topic_prefix.view() == b.topic_prefix.view() && a.publish_period_s == b.publish_period_s;
}
}

struct MqttService::Impl {
    static constexpr std::size_t kPayloadMax = kMqttPayloadCapacity;
    static constexpr std::size_t kEventQueue = 8;
    WiFiClient tcp{}; PubSubClient client{tcp}; MqttConfig config{};
    std::array<char, kPayloadMax> payload{}; std::array<AlarmEvent, kEventQueue> events{};
    std::size_t event_head{0}, event_count{0}; MqttRetrySchedule retry{}; MonotonicMs next_publish_ms{0};
    std::array<char, 128> client_id{}; std::array<char, 160> state_topic{}, event_topic{}, status_topic{};
    bool enabled{false}, configured{false};

    Result configure(const MqttConfig& next) {
        if (next.port == 0 || next.publish_period_s < 5 || next.publish_period_s > 3600 || !topicPrefixValid(next.topic_prefix.view()) ||
            (next.enabled && next.broker.view().empty())) return Result::Invalid;
        if (configured && sameConfig(config, next)) return Result::Accepted;
        if (client.connected()) client.disconnect();
        config = next; enabled = config.enabled;
        const std::uint64_t mac = ESP.getEfuseMac();
        std::snprintf(client_id.data(), client_id.size(), "FLAC-%012llX", static_cast<unsigned long long>(mac & 0xffffffffffffULL));
        char device_id[20]; std::snprintf(device_id, sizeof(device_id), "%012llx", static_cast<unsigned long long>(mac & 0xffffffffffffULL));
        const auto prefix = config.topic_prefix.view();
        std::snprintf(state_topic.data(), state_topic.size(), "%.*s/%s/state", static_cast<int>(prefix.size()), prefix.data(), device_id);
        std::snprintf(event_topic.data(), event_topic.size(), "%.*s/%s/event", static_cast<int>(prefix.size()), prefix.data(), device_id);
        std::snprintf(status_topic.data(), status_topic.size(), "%.*s/%s/status", static_cast<int>(prefix.size()), prefix.data(), device_id);
        if (enabled) {
            client.setServer(config.broker.c_str(), config.port);
            tcp.setTimeout(1); // Bound synchronous TCP connect to one second.
            if (!client.setBufferSize(static_cast<std::uint16_t>(payload.size()))) return Result::IoError;
            client.setSocketTimeout(1); client.setKeepAlive(20);
        }
        configured = true;
        return Result::Accepted;
    }
    bool connect() {
        char will[16]; std::snprintf(will, sizeof(will), "offline");
        bool connected;
        if (!config.username.view().empty()) connected = client.connect(client_id.data(), config.username.c_str(), config.password.c_str(),
            status_topic.data(), 0, true, will);
        else connected = client.connect(client_id.data(), status_topic.data(), 0, true, will);
        if (!connected) return false;
        client.publish(status_topic.data(), "online", true); next_publish_ms = 0; return true;
    }
    void buildState(const RuntimeState& state, MonotonicMs now) {
        std::size_t length = 0;
        if (encodeMqttState(state, now, payload.data(), payload.size(), length) != Result::Accepted) payload[0] = '\0';
    }
    void buildEvent(const AlarmEvent& event) {
        std::size_t length = 0;
        if (encodeMqttEvent(event, payload.data(), payload.size(), length) != Result::Accepted) payload[0] = '\0';
    }
    void tick(const IStateView& view, MonotonicMs now) {
        const auto& state = view.read();
        if (!enabled) return;
        if (client.connected()) client.loop();
        if (!state.network.link) return;
        if (!client.connected()) {
            if (!retry.due(now)) return;
            if (connect()) retry.succeeded(); else retry.failed(now);
            return;
        }
        if (event_count) {
            buildEvent(events[event_head]);
            if (payload[0] && client.publish(event_topic.data(), payload.data(), false)) {
                event_head = (event_head + 1) % kEventQueue; --event_count;
            }
        }
        if (now >= next_publish_ms) {
            buildState(state, now);
            if (payload[0] && client.publish(state_topic.data(), payload.data(), true))
                next_publish_ms = now + static_cast<MonotonicMs>(config.publish_period_s) * 1000;
            else next_publish_ms = now + 5000;
        }
    }
    Result publishAlarm(const AlarmEvent& event) {
        if (!enabled) return Result::NotFound;
        if (event_count == kEventQueue) { event_head = (event_head + 1) % kEventQueue; --event_count; }
        events[(event_head + event_count) % kEventQueue] = event; ++event_count; return Result::Accepted;
    }
};

MqttService::MqttService() : impl_(new Impl) {}
MqttService::~MqttService() { delete impl_; }
Result MqttService::configure(const MqttConfig& config) { return impl_->configure(config); }
void MqttService::tick(const IStateView& state, MonotonicMs now) { impl_->tick(state, now); }
Result MqttService::publishAlarm(const AlarmEvent& event) { return impl_->publishAlarm(event); }
} // namespace fl
