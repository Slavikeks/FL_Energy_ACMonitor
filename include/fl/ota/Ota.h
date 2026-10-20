#pragma once
#include "fl/core/Types.h"
#include "fl/core/StateStore.h"
#include "fl/core/EventLog.h"

namespace fl {
enum class OtaState : std::uint8_t { Idle, Receiving, Verifying, ReadyToReboot, Failed };
class IOtaService {
public:
    virtual ~IOtaService() = default;
    virtual Result begin(std::size_t image_bytes) = 0;
    virtual Result write(const std::uint8_t*, std::size_t) = 0;
    virtual Result finish() = 0;
    virtual void abort() = 0;
    virtual OtaState state() const = 0;
};
class OtaService final : public IOtaService {
public:
    OtaService(RuntimeState&, EventLog&);
    Result begin(std::size_t) override;
    Result write(const std::uint8_t*, std::size_t) override;
    Result finish() override;
    void abort() override;
    OtaState state() const override { return state_; }
private:
    RuntimeState& runtime_;
    EventLog& log_;
    OtaState state_{OtaState::Idle};
}; // Firmware upload adapter; WebService owns authorization policy.
} // namespace fl
