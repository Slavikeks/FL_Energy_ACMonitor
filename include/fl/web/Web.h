#pragma once
#include "fl/ac/Registry.h"
#include "fl/core/Config.h"
#include "fl/core/StateStore.h"
#include "fl/auth/Auth.h"
#include "fl/ota/Ota.h"
#include "fl/core/EventLog.h"
#include "fl/network/Network.h"
#include "fl/xye/Bus.h"

namespace fl {
class ISettingsService {
public:
    virtual ~ISettingsService() = default;
    virtual Result updateConfig(const Config&) = 0;
    virtual Result putGroup(const GroupConfig&) = 0;
    virtual Result deleteGroup(GroupId) = 0;
};
class IWebService {
public:
    virtual ~IWebService() = default;
    virtual Result begin() = 0;
    virtual void tick(MonotonicMs) = 0;
};
class WebService final : public IWebService {
public:
    WebService(IStateView&, Config&, ISettingsService&, IRegistryService&, ICommandSink&,
               IAuthService&, IOtaService&, EventLog&, IClock&);
    Result begin() override;
    void tick(MonotonicMs) override;
private:
    struct Impl;
    Impl* impl_;
}; // Stage-1 HTTP service; mutations enforce session/CSRF policy.
// HTTP controls enqueue only; server enforces auth/CSRF/AUTO/maintenance policy.
} // namespace fl
