#include <Arduino.h>
#include <esp_timer.h>
#include "fl/core/Application.h"

namespace { fl::Application application; }
void setup() {
    Serial.begin(115200);
    application.begin();
    Serial.printf("FL Energy AC Monitor %s started\n", fl::kFirmwareVersion);
    Serial.printf("flash=%u bytes; model budget=%u bytes\n",
                  ESP.getFlashChipSize(), static_cast<unsigned>(fl::kModelBytes));
    Serial.println("UNVERIFIED_HARDWARE: C3 commands require explicit per-model validation; automatic control is disabled");
}
void loop() {
    application.tick(static_cast<fl::MonotonicMs>(esp_timer_get_time() / 1000));
    yield();
}
