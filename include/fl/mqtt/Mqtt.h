#pragma once
#include "fl/core/Config.h"
#include "fl/core/StateStore.h"

namespace fl {
class IMqttService {
public:
    virtual ~IMqttService() = default;
    virtual Result configure(const MqttConfig&) = 0;
    virtual void tick(const IStateView&, MonotonicMs) = 0;
    virtual Result publishAlarm(const AlarmEvent&) = 0;
}; // Publish telemetry only; no subscribe, commands or UART reference.

class MqttService final : public IMqttService {
public:
    MqttService();
    ~MqttService() override;
    MqttService(const MqttService&) = delete;
    MqttService& operator=(const MqttService&) = delete;
    Result configure(const MqttConfig&) override;
    void tick(const IStateView&, MonotonicMs) override;
    Result publishAlarm(const AlarmEvent&) override;
private:
    struct Impl;
    Impl* impl_;
};
} // namespace fl
