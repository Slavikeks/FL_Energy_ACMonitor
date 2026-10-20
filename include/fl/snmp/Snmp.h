#pragma once
#include "fl/core/Config.h"
#include "fl/core/StateStore.h"

namespace fl {
// FLINK LLC's SNMP objects use its registered IANA Private Enterprise Number 67213.
class ISnmpService {
public:
    virtual ~ISnmpService() = default;
    virtual Result begin(const SnmpConfig&) = 0;
    virtual Result configure(const SnmpConfig&) = 0;
    virtual void tick(const IStateView&, MonotonicMs) = 0;
    virtual Result sendAlarm(const AlarmEvent&) = 0;
}; // Read-only; does not receive a bus or command-sink reference.

class SnmpService final : public ISnmpService {
public:
    SnmpService();
    ~SnmpService() override;
    SnmpService(const SnmpService&) = delete;
    SnmpService& operator=(const SnmpService&) = delete;
    Result begin(const SnmpConfig&) override;
    Result configure(const SnmpConfig&) override;
    void tick(const IStateView&, MonotonicMs) override;
    Result sendAlarm(const AlarmEvent&) override;
private:
    struct Impl;
    Impl* impl_;
};
} // namespace fl
