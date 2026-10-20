#include "fl/xye/Bus.h"
#include "fl/core/StateStore.h"

namespace fl {
namespace {
std::uint8_t checksum(const std::uint8_t* bytes, std::size_t length, std::size_t checksum_index) {
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < length; ++i) if (i != checksum_index) sum += bytes[i];
    return static_cast<std::uint8_t>(0xFFU - (sum & 0xFFU));
}
void set(Sample<std::int16_t>& sample, std::uint8_t raw) {
    if (raw == 0xFF) { sample.status = ValueStatus::Unsupported; return; }
    // HomeOps documents this Celsius scaling, but reports model variants.
    sample.value = static_cast<std::int16_t>((static_cast<int>(raw) - 0x28) * 5);
    sample.status = ValueStatus::Experimental;
}
}
bool validXyeChecksum(const RxFrame& frame) { return checksum(frame.data(), frame.size(), 30) == frame[30]; }

class XyeCodec final : public IXyeCodec {
public:
    Result encodeQuery(Address address, TxFrame& frame) const override {
        if (!validAddress(address)) return Result::Invalid;
        frame.fill(0);
        frame[0] = 0xAA; frame[1] = static_cast<std::uint8_t>(XyeCommand::Query);
        frame[2] = address; frame[3] = 0; frame[4] = 0x80; frame[5] = 0;
        frame[15] = 0x55;
        frame[13] = static_cast<std::uint8_t>(0xFFU - frame[1]);
        frame[14] = checksum(frame.data(), frame.size(), 14);
        return Result::Accepted;
    }
    Result encodeSet(const ControlRequest& request, TxFrame& frame) const override {
        if (!validAddress(request.address) || !request.settings.power) return Result::Invalid;

        std::uint8_t mode = 0x00, fan = 0x00, temperature = 0xFF;
        if (*request.settings.power) {
            if (!request.settings.mode || !request.settings.fan) return Result::Invalid;
            switch (*request.settings.mode) {
            case Mode::Fan: mode = 0x81; break;
            case Mode::Dry: mode = 0x82; break;
            case Mode::Heat: mode = 0x84; break;
            case Mode::Cool: mode = 0x88; break;
            default: return Result::Invalid;
            }
            switch (*request.settings.fan) {
            case Fan::Auto: fan = 0x80; break;
            case Fan::High: fan = 0x01; break;
            case Fan::Medium: fan = 0x02; break;
            case Fan::Low: fan = 0x04; break;
            default: return Result::Invalid;
            }
            if (*request.settings.mode == Mode::Fan) {
                if (request.settings.setpoint_deci_c) return Result::Invalid;
            } else {
                if (!request.settings.setpoint_deci_c ||
                    *request.settings.setpoint_deci_c < 0 ||
                    *request.settings.setpoint_deci_c > 2550 ||
                    *request.settings.setpoint_deci_c % 10 != 0) return Result::Invalid;
                // XYE command/setpoint is raw whole Celsius. Sensor temperatures
                // use the separate offset-and-half-degree encoding.
                temperature = static_cast<std::uint8_t>(*request.settings.setpoint_deci_c / 10);
            }
        } else if (request.settings.mode || request.settings.fan || request.settings.setpoint_deci_c) {
            return Result::Invalid;
        }

        frame.fill(0);
        frame[0] = 0xAA; frame[1] = static_cast<std::uint8_t>(XyeCommand::Set);
        frame[2] = request.address; frame[3] = 0; frame[4] = 0x80; frame[5] = 0;
        frame[6] = mode; frame[7] = fan; frame[8] = temperature;
        frame[13] = static_cast<std::uint8_t>(0xFFU - frame[1]); frame[15] = 0x55;
        frame[14] = checksum(frame.data(), frame.size(), 14);
        return Result::Accepted;
    }
    Result decode(const RxFrame& frame, Address expected, XyeCommand command,
                  ObservedState& out) const override {
        if (!validAddress(expected) || (command != XyeCommand::Query && command != XyeCommand::Set) || frame[0] != 0xAA ||
            frame[1] != static_cast<std::uint8_t>(command) ||
            (frame[2] != 0x00 && frame[2] != 0x80) || frame[3] != 0 ||
            frame[4] != expected || frame[5] != 0 || frame[31] != 0x55 ||
            !validXyeChecksum(frame)) return Result::Invalid;

        const auto mode = frame[8];
        out.power.value = mode != 0;
        out.power.status = ValueStatus::Experimental;
        switch (mode) {
        case 0x00: out.mode.value.reset(); out.mode.status = ValueStatus::Unsupported; break;
        case 0x81: out.mode.value = Mode::Fan; out.mode.status = ValueStatus::Experimental; break;
        case 0x82: out.mode.value = Mode::Dry; out.mode.status = ValueStatus::Experimental; break;
        case 0x84: out.mode.value = Mode::Heat; out.mode.status = ValueStatus::Experimental; break;
        case 0x88: out.mode.value = Mode::Cool; out.mode.status = ValueStatus::Experimental; break;
        default: out.mode.value.reset(); out.mode.status = ValueStatus::Unsupported; break;
        }
        out.capabilities_raw.value = frame[7]; out.capabilities_raw.status = ValueStatus::Experimental;
        if (frame[10] == 0xFF) out.setpoint_deci_c.status = ValueStatus::Unsupported;
        else {
            out.setpoint_deci_c.value = static_cast<std::int16_t>(frame[10] & 0xBFU) * 10;
            out.setpoint_deci_c.status = ValueStatus::Experimental;
        }
        set(out.t1_deci_c, frame[11]); set(out.t2_deci_c, frame[12]);
        set(out.t2b_deci_c, frame[13]); set(out.t3_deci_c, frame[14]);
        // C0 byte 9 reports actual speed in the low nibble and AUTO in bit 7.
        // In AUTO with no physical speed (0x80), the mode is known but speed is not.
        const auto fan = frame[9];
        out.fan_auto.value = (fan & 0x80U) != 0;
        out.fan_auto.status = ValueStatus::Experimental;
        out.fan_actual.value.reset();
        out.fan_actual.status = ValueStatus::Unsupported;
        if ((fan & 0x70U) == 0) {
            switch (fan & 0x0FU) {
            case 0x00:
                if ((fan & 0x80U) == 0) {
                    out.fan_actual.value = FanSpeed::Stopped;
                    out.fan_actual.status = ValueStatus::Experimental;
                }
                break;
            case 0x01: out.fan_actual.value = FanSpeed::High; out.fan_actual.status = ValueStatus::Experimental; break;
            case 0x02: out.fan_actual.value = FanSpeed::Medium; out.fan_actual.status = ValueStatus::Experimental; break;
            case 0x03: case 0x04:
                out.fan_actual.value = FanSpeed::Low; out.fan_actual.status = ValueStatus::Experimental; break;
            default: break;
            }
        }
        out.fan_selected.status = ValueStatus::Unsupported;
        out.compressor_running.status = ValueStatus::Experimental;
        if (frame[19] == 0 || frame[19] == 1) out.compressor_running.value = frame[19] != 0;
        else { out.compressor_running.value.reset(); out.compressor_running.status = ValueStatus::Unsupported; }
        out.drain_pump.status = ValueStatus::Unsupported;
        out.water_lock.status = ValueStatus::Unsupported;
        out.error_flags.value = static_cast<std::uint16_t>(frame[22] | (std::uint16_t(frame[23]) << 8));
        out.error_flags.status = ValueStatus::Experimental;
        out.protection_flags.value = static_cast<std::uint16_t>(frame[24] | (std::uint16_t(frame[25]) << 8));
        out.protection_flags.status = ValueStatus::Experimental;
        out.ccm_error_flags.value = frame[26]; out.ccm_error_flags.status = ValueStatus::Experimental;
        return Result::Accepted;
    }
};

IXyeCodec& defaultXyeCodec() { static XyeCodec codec; return codec; }
} // namespace fl
