#pragma once
#include "fl/alarm/Alarm.h"
#include "fl/core/StateStore.h"
#include <cstddef>

namespace fl {
inline constexpr std::size_t kMqttPayloadCapacity = 9216;
Result encodeMqttState(const RuntimeState&, MonotonicMs now, char* output, std::size_t capacity, std::size_t& written);
Result encodeMqttEvent(const AlarmEvent&, char* output, std::size_t capacity, std::size_t& written);

class MqttRetrySchedule {
public:
    bool due(MonotonicMs now) const { return now >= next_attempt_ms_; }
    void failed(MonotonicMs now) { next_attempt_ms_ = now + 10000; }
    void succeeded() { next_attempt_ms_ = 0; }
private:
    MonotonicMs next_attempt_ms_{0};
};
} // namespace fl
