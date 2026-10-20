#pragma once
#include "fl/xye/Bus.h"
#include "fl/core/StateStore.h"
#include "fl/core/EventLog.h"
#include <array>

namespace fl {
class XyeBusManager final : public IBusManager, public IRegistryService {
public:
    XyeBusManager(IRs485Transport&, IXyeCodec&, Config&, IConfigRepository&, EventLog&);
    Result begin();
    Result startScan() override;
    Result startReadOnlyScan() override { return startScan(); }
    Result registerAc(const AcRegistration&) override;
    Result updateAc(const AcRegistration&) override;
    Result removeAc(Address) override;
    Result enqueue(const ControlRequest&) override;
    void tick(const Config&, RuntimeState&, MonotonicMs) override;
private:
    enum class Operation : std::uint8_t { Idle, Scan, Poll, Command, Verify };
    Result saveRegistry(Registry&);
    void advanceScan(RuntimeState&, MonotonicMs, bool success);
    void advancePoll(RuntimeState&, MonotonicMs, bool success);
    void applySuccess(RuntimeState&, Address, ObservedState, MonotonicMs);
    void applyFailure(RuntimeState&, Address);
    void startCommand(RuntimeState&, MonotonicMs);
    void finishCommand(RuntimeState&, MonotonicMs, Result);
    void scheduleVerifyRetry(MonotonicMs);
    bool matchesDesired(const ObservedState&) const;
    Result encodeCurrent(TxFrame&) const;
    void resetAwaiting(MonotonicMs);
    IRs485Transport& transport_; IXyeCodec& codec_; Config& config_;
    IConfigRepository& repository_; EventLog& log_;
    bool scan_reset_pending_{false};
    Operation operation_{Operation::Idle}; bool awaiting_{false}; bool tx_complete_{false};
    Address current_address_{0}; std::size_t poll_index_{0}; std::uint8_t retries_{0};
    MonotonicMs deadline_{0}, next_action_ms_{0}, next_cycle_ms_{0};
    RxFrame rx_{}; std::size_t rx_length_{0};
    std::array<ControlRequest, kBusQueueCapacity> queue_{};
    std::size_t queue_head_{0}, queue_count_{0};
    ControlRequest active_command_{};
    bool active_command_valid_{false};
    std::uint8_t command_retries_{0}, verify_retries_{0}, commands_since_poll_{0};
    TransactionId next_transaction_id_{1};
    std::array<MonotonicMs, kAddressCount> last_command_ms_{};
    std::bitset<kAddressCount> has_command_time_{};
    RuntimeState* current_state_{nullptr};
    MonotonicMs now_ms_{0};
};
} // namespace fl
