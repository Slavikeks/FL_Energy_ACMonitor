# Installation and first start

## Hardware

- WT32-ETH01 with 4 MiB flash.
- A suitable RS-485 transceiver with automatic transmit/receive direction.
- Wired Ethernet and a stable supply appropriate for the board.
- A USB-to-UART programmer for a blank board or recovery.

Connect the XYE bus only through an RS-485 transceiver. See [XYE connection](XYE_CONNECTION.md) for the firmware pin mapping and bus precautions.

## Build the firmware

Install PlatformIO Core and desktop GCC, then use the repository root:

```sh
pio test -e native
pio run -e wt32-eth01
```

PlatformIO stores build outputs outside the checkout. On the default Core layout, the OTA image is:

```text
<PlatformIO Core directory>/workspaces/FL_Energy_ACMonitor/build/wt32-eth01/firmware.bin
```

Use `python scripts/check_partitions.py --firmware <path-to-firmware.bin>` to verify the image fits the 4 MiB OTA layout. See [BUILD.md](BUILD.md) for the pinned tool versions and complete commands.

## Initial serial flash

For an uninitialized board, upload the bootloader, partition table, and application using PlatformIO:

1. Copy `platformio.local.ini.example` to `platformio.local.ini`.
2. Set the local serial port in `upload_port`. The local file is ignored by Git.
3. Run:

   ```sh
   pio run -e wt32-eth01 -t upload
   pio device monitor -b 115200
   ```

The serial console reports the firmware version and hardware information. Keep a serial programmer available for recovery.

## Network and web login

The factory IPv4 configuration is static and defined in `include/fl/core/Config.h`: **`10.90.90.90`**, subnet mask `255.255.255.0`, gateway `10.90.90.1`, and DNS `10.90.90.1`. DHCP is not used for the initial connection. Before first boot, check that this address does not conflict with your LAN; set the setup computer to an unused address in `10.90.90.0/24`, then open <http://10.90.90.90>. After signing in, configure the address, mask, gateway, DNS, NTP server, and time zone in Settings.

The initial account is `admin` / `admin`. Password change is available in Settings but is not forced. Change it before connecting the controller to a shared or untrusted network. For programmer-based installation or recovery, see [the bilingual flashing guide](FLASHING.md).

Automatic control, SNMP, and MQTT start disabled. Register each conditioner, verify its live readings and manual commands, validate the model, and only then consider enabling group automation.

For interface pages and settings, see [Web UI](WEB_UI.md). For OTA updates, see [OTA](OTA.md).
