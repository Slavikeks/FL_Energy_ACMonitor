#include "fl/ota/Ota.h"
#include <Arduino.h>
#include <Update.h>

namespace fl {
namespace {
void logOta(EventLog& log, Severity severity, const char* message) {
    LogEvent entry; entry.severity = severity; entry.message.assign(message); log.append(entry);
}
}
OtaService::OtaService(RuntimeState& runtime, EventLog& log) : runtime_(runtime), log_(log) {}
Result OtaService::begin(std::size_t image_bytes) {
    if (state_ == OtaState::Receiving || image_bytes == 0 || image_bytes > ESP.getFreeSketchSpace())
        return Result::Invalid;
    runtime_.ota_active = true;
    state_ = OtaState::Receiving;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        state_ = OtaState::Failed; runtime_.ota_active = false;
        logOta(log_, Severity::Critical, "OTA start failed");
        return Result::IoError;
    }
    logOta(log_, Severity::Info, "OTA firmware upload started");
    return Result::Accepted;
}
Result OtaService::write(const std::uint8_t* data, std::size_t length) {
    if (state_ != OtaState::Receiving || !data || Update.write(const_cast<std::uint8_t*>(data), length) != length) {
        state_ = OtaState::Failed; logOta(log_, Severity::Critical, "OTA write failed");
        return Result::IoError;
    }
    return Result::Accepted;
}
Result OtaService::finish() {
    if (state_ != OtaState::Receiving) return Result::Invalid;
    state_ = OtaState::Verifying;
    if (!Update.end(true) || !Update.isFinished()) {
        state_ = OtaState::Failed; runtime_.ota_active = false;
        logOta(log_, Severity::Critical, "OTA verification failed");
        return Result::IoError;
    }
    state_ = OtaState::ReadyToReboot;
    logOta(log_, Severity::Info, "OTA firmware verified; reboot pending");
    return Result::Accepted;
}
void OtaService::abort() {
    Update.abort();
    state_ = OtaState::Idle; runtime_.ota_active = false;
}
} // namespace fl
