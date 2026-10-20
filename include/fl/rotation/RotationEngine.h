#pragma once

#include "fl/core/ConfigRepository.h"
#include "fl/core/EventLog.h"
#include "fl/core/StateStore.h"
#include "fl/rotation/Group.h"

namespace fl {
class RotationEngine final : public IRotationEngine {
public:
    RotationEngine(IConfigRepository&, EventLog&);
    void tick(Config&, RuntimeState&, const ClockState&, ICommandSink&) override;
private:
    IConfigRepository& repository_;
    EventLog& log_;
    TransactionId next_transaction_id_{0x40000000U};
    List<GroupConfig, kMaxGroups> groups_scratch_{};
    bool startCommand(const GroupConfig&, GroupRuntime&, Address, RuntimeState&, ICommandSink&, MonotonicMs, const char* reason);
    bool stopCommand(const Config&, const GroupConfig&, GroupRuntime&, Address, RuntimeState&, ICommandSink&, MonotonicMs, const char* reason, bool allow_below_required = false);
    bool beginBreakRotation(const GroupConfig&, GroupRuntime&, Address, Address, ICommandSink&, MonotonicMs);
    bool beginFailoverBreak(const GroupConfig&, GroupRuntime&, Address, Address, ICommandSink&, MonotonicMs);
    bool reconcileSettings(const Config&, const GroupConfig&, GroupRuntime&, RuntimeState&, ICommandSink&, MonotonicMs);
    void persistAnchor(Config&, GroupId, EpochSeconds);
};
} // namespace fl
