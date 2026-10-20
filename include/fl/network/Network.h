#pragma once
#include "fl/core/Config.h"
#include "fl/core/StateStore.h"
#include "fl/core/EventLog.h"

namespace fl {
class INetworkService {
public:
    virtual ~INetworkService() = default;
    virtual Result begin(const NetworkConfig&) = 0;
    virtual void tick(NetworkState&) = 0;
};
class ITimeService {
public:
    virtual ~ITimeService() = default;
    virtual Result configure(const TimeConfig&) = 0;
    virtual void tick(ClockState&) = 0;
};
class EthernetTimeService final : public INetworkService, public ITimeService {
public:
    EthernetTimeService(Config&, EventLog&);
    Result begin(const NetworkConfig&) override;
    void tick(NetworkState&) override;
    Result configure(const TimeConfig&) override;
    Result applyNetwork(const NetworkConfig&);
    void tick(ClockState&) override;
private:
    Config& config_;
    EventLog& log_;
    NetworkConfig network_{};
    TimeConfig time_{};
    bool ntp_started_{false};
    bool was_link_up_{false};
};
} // namespace fl
