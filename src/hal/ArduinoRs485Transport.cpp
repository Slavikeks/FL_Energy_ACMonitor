#include "fl/hal/Rs485Transport.h"
#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <HardwareSerial.h>
#include <esp_timer.h>

namespace fl {
namespace {
HardwareSerial& busUart() { static HardwareSerial uart(2); return uart; }
std::int64_t txDoneUs = 0;
}
class ArduinoRs485Transport final : public IRs485Transport {
public:
    Result begin() override {
        busUart().setRxBufferSize(256);
        busUart().setTxBufferSize(256);
        busUart().begin(kRs485Baud, SERIAL_8N1, kRs485RxPin, kRs485TxPin);
        while (busUart().available()) busUart().read();
        return Result::Accepted;
    }
    std::size_t write(const std::uint8_t* data, std::size_t size) override {
        if (!data || !size || !txIdle()) return 0;
        const auto written = busUart().write(data, size);
        // 8N1 is ten wire bits per byte; don't block the main loop on UART flush.
        txDoneUs = esp_timer_get_time() + static_cast<std::int64_t>((size * 10000000ULL + kRs485Baud - 1) / kRs485Baud);
        return written;
    }
    std::size_t read(std::uint8_t* output, std::size_t capacity) override {
        if (!output) return 0;
        std::size_t count = 0;
        while (count < capacity && busUart().available() > 0) output[count++] = static_cast<std::uint8_t>(busUart().read());
        return count;
    }
    bool txIdle() const override { return esp_timer_get_time() >= txDoneUs; }
};
IRs485Transport& defaultRs485Transport() { static ArduinoRs485Transport transport; return transport; }
} // namespace fl
#endif
