#pragma once
#include "fl/core/Types.h"

namespace fl {
struct AcRegistration {
    Address address{0};
    Name name{};
    // Persisted in the legacy maintenance bit. In an AUTO group this means
    // temporary owner-controlled mode (manual control), not model approval.
    bool maintenance{false};
    std::int16_t min_setpoint_deci_c{160};
    std::int16_t max_setpoint_deci_c{300};
    bool model_validated{false};
};
using Registry = List<AcRegistration, kMaxAc>;
// Group membership is stored only in GroupConfig.members: one authoritative relation.
class IRegistryService {
public:
    virtual ~IRegistryService() = default;
    virtual Result registerAc(const AcRegistration&) = 0;
    virtual Result updateAc(const AcRegistration&) = 0;
    virtual Result removeAc(Address) = 0;
    virtual Result startReadOnlyScan() = 0;
};
class ICommandSink {
public:
    virtual ~ICommandSink() = default;
    // Acceptance means queued, never a successful SET acknowledgement.
    virtual Result enqueue(const ControlRequest&) = 0;
};
} // namespace fl
