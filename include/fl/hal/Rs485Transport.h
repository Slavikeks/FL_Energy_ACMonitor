#pragma once
#include "fl/core/Types.h"

#ifndef FL_RS485_AUTO_DIRECTION
#define FL_RS485_AUTO_DIRECTION 1
#endif
static_assert(FL_RS485_AUTO_DIRECTION == 1,
              "Only the documented auto-direction transceiver is supported");
namespace fl {
inline constexpr int kRs485RxPin = 5;
inline constexpr int kRs485TxPin = 17;
inline constexpr std::uint32_t kRs485Baud = 4800;
struct UartFormat { std::uint8_t data_bits{8}, stop_bits{1}; bool parity{false}; };
class IRs485Transport {
public:
    virtual ~IRs485Transport() = default;
    virtual Result begin() = 0; // 4800 8N1, auto-direction, no DE/RE GPIO.
    virtual std::size_t write(const std::uint8_t*, std::size_t) = 0;
    virtual std::size_t read(std::uint8_t*, std::size_t capacity) = 0;
    virtual bool txIdle() const = 0;
}; // Sole owner: bus manager. No UART access from network adapters.
class IClock {
public:
    virtual ~IClock() = default;
    virtual ClockState now() const = 0;
};
IRs485Transport& defaultRs485Transport();
} // namespace fl
