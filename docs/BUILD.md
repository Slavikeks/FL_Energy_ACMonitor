# Build and test

## Requirements

- Python 3.11 or newer.
- PlatformIO Core 6.1.18, installed in a user-level Python environment or through the PlatformIO IDE extension.
- A desktop GCC toolchain for the `native` test environment. On Windows, make sure `gcc.exe` is available in `PATH`.

The embedded versions are pinned in `platformio.ini`: Espressif32 6.9.0, Arduino-ESP32 2.0.17, Xtensa GCC 8.4.0, SNMP_Agent 2.1.0, and PubSubClient 2.8. PlatformIO downloads these packages when they are not already cached.

## Install PlatformIO Core

Create a virtual environment outside the checkout, then install the pinned Core requirement:

```powershell
$pioHome = Join-Path $env:USERPROFILE ".platformio"
python -m venv (Join-Path $pioHome "venv")
& (Join-Path $pioHome "venv/Scripts/python.exe") -m pip install -r requirements-dev.txt
```

Add `.platformio/venv/Scripts` to the user PATH, open a new terminal, and verify:

```powershell
pio --version
gcc --version
```

Alternatively, install PlatformIO Core using the official PlatformIO IDE extension. Do not copy an existing virtual environment between machines.

## Build and validate

Run from the repository root:

```sh
pio test -e native
pio run -e wt32-eth01
pio run -e wt32-eth01 -t size
```

Validate the 4 MiB partition table and the produced OTA image:

```powershell
$firmware = Join-Path $env:USERPROFILE ".platformio/workspaces/FL_Energy_ACMonitor/build/wt32-eth01/firmware.bin"
python scripts/check_partitions.py --firmware $firmware
```

The current PlatformIO configuration places build products and downloaded project libraries under the PlatformIO Core workspace, outside the checkout. On systems with a different Core directory, use the equivalent `platformio.core_dir/workspaces/FL_Energy_ACMonitor` location.

## Create a merged image for a serial programmer

The OTA `firmware.bin` contains only the application. To make a single image for first installation or recovery, merge the bootloader, partition table, and application at their ESP32 flash offsets. With esptool 5 installed, run from the project root:

```powershell
$build = Join-Path $env:USERPROFILE ".platformio/workspaces/FL_Energy_ACMonitor/build/wt32-eth01"
esptool --chip esp32 merge-bin -o firmware.merged.bin `
  0x1000 (Join-Path $build "bootloader.bin") `
  0x8000 (Join-Path $build "partitions.bin") `
  0x10000 (Join-Path $build "firmware.bin")
```

The resulting `firmware.merged.bin` is written at flash offset `0x0`. For the serial write command and initial network/login defaults, see [the flashing guide](FLASHING.md).

## First serial flash

The first installation on a blank WT32-ETH01 must write the bootloader, partition table, and firmware over a USB-to-UART programmer. Subsequent updates can use the authenticated web OTA page.

1. Copy `platformio.local.ini.example` to `platformio.local.ini`.
2. Set `upload_port` and, if needed, `monitor_port` to the local serial port. This file is ignored by Git.
3. Build and flash:

   ```sh
   pio run -e wt32-eth01 -t upload
   pio device monitor -b 115200
   ```

No port, password, SNMP community, or MQTT credentials should be committed.

## Flash layout

The selected partition table targets the 4 MiB flash configuration used by WT32-ETH01:

| Partition | Offset | Size |
| --- | ---: | ---: |
| NVS | `0x9000` | 20 KiB |
| OTA metadata | `0xE000` | 8 KiB |
| OTA slot 0 | `0x10000` | 1,966,080 bytes |
| OTA slot 1 | `0x1F0000` | 1,966,080 bytes |
| Unused | `0x3D0000` | 192 KiB |

There is no filesystem partition. OTA accepts a `firmware.bin` that fits one application slot. The current source does not implement a separate rollback policy; keep a serial recovery path available during early deployment.
