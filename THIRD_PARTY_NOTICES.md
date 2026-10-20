# Attribution and dependencies

Application code is maintained in this repository. The XYE research links below are references, not runtime dependencies; no source fragments are intentionally copied from them.

Research references, not runtime dependencies:

- [HomeOps/ESPHome-Midea-XYE](https://github.com/HomeOps/ESPHome-Midea-XYE),
  [protocol](https://github.com/HomeOps/ESPHome-Midea-XYE/blob/main/esphome/components/midea_xye/PROTOCOL.md),
  [MIT license](https://github.com/HomeOps/ESPHome-Midea-XYE/blob/main/LICENSE).
  Preserve notices for any future copied code.
- [Bunicutz/ESP32_Midea_RS485](https://github.com/Bunicutz/ESP32_Midea_RS485)
  was also consulted as a protocol reference; no code is included from it.

Firmware and test dependencies retain their upstream licenses:

- [SNMP_Agent 2.1.0](https://github.com/0neblock/Arduino_SNMP/tree/master), MIT.
- [PubSubClient 2.8](https://github.com/knolleary/pubsubclient), MIT.
- [Arduino-ESP32 2.0.17](https://github.com/espressif/arduino-esp32/tree/2.0.17),
  LGPL-2.1; the framework distribution also contains ESP-IDF and other
  components with their own notices.
- [Unity 2.6.0](https://github.com/ThrowTheSwitch/Unity/tree/v2.6.0), MIT,
  test-only.

Build tools are not included in the source checkout or firmware release:

- [PlatformIO Core 6.1.18](https://github.com/platformio/platformio-core), Apache-2.0.
- [Espressif32 PlatformIO platform 6.9.0](https://github.com/platformio/platform-espressif32/tree/v6.9.0), Apache-2.0.
- Xtensa GCC and supporting esptool packages are downloaded by PlatformIO;
  observe the licenses provided with each package.
- [w64devkit 2.1.0](https://github.com/skeeto/w64devkit/releases/tag/v2.1.0)
  is an optional local native-test toolchain, not a runtime dependency.

The repository's MIT license applies to this project's code only. It does not
relicense these dependencies or replace their required notices. Before
redistributing a firmware binary, include the applicable notices and review the
license materials shipped with the exact framework/package versions used.

## Firmware distribution note

The firmware links Arduino-ESP32 2.0.17, which is licensed under LGPL-2.1.
Before distributing firmware binaries, include the applicable license text and
notices and make the exact framework source available. Review the LGPL terms for
the combined firmware and provide the corresponding source and any required
relinkable form so recipients can modify the library and use it with this
application. The exact obligations depend on the package components and the
distribution method; verify them for the final release bundle. Also check the
licenses and notices of ESP-IDF components and other runtime packages included
by the selected Arduino-ESP32 distribution.
