#pragma once
#include "fl/ac/Registry.h"
#include "fl/core/Config.h"
#include "fl/hal/Rs485Transport.h"

namespace fl {
class IXyeCodec;
IXyeCodec& defaultXyeCodec();
enum class XyeCommand : std::uint8_t { Query = 0xC0, Set = 0xC3, QueryExtended = 0xC4 };
using TxFrame = std::array<std::uint8_t, 16>;
using RxFrame = std::array<std::uint8_t, 32>;
bool validXyeChecksum(const RxFrame&);
inline constexpr std::size_t kBusQueueCapacity = 16;
struct ObservedState;
struct RuntimeState;
class IXyeCodec {
public:
    virtual ~IXyeCodec() = default;
    virtual Result encodeQuery(Address, TxFrame&) const = 0;
    virtual Result encodeSet(const ControlRequest&, TxFrame&) const = 0;
    virtual Result decode(const RxFrame&, Address expected, XyeCommand,
                          ObservedState&) const = 0;
}; // Stage 2: checksum including 0x55; replies 0x00/0x80; no Modbus CRC.
class IBusManager : public ICommandSink {
public:
    virtual Result startScan() = 0;
    virtual void tick(const Config&, RuntimeState&, MonotonicMs) = 0;
}; // One in-flight transaction, bounded queue, fair polling, no blocking waits.
} // namespace fl
