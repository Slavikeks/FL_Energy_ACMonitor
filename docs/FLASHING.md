# Firmware flashing / Прошивка устройства

This guide describes the prebuilt first-release images for WT32-ETH01 with 4 MiB flash. Use `firmware.bin` for web OTA on an already initialized device. Use `firmware.merged.bin` with a serial programmer for first installation or recovery.

## English

### Flash with a USB-to-UART programmer

1. Install esptool from Command Prompt:

   ```bat
   pip install esptool
   ```

   If `pip` is not found, use:

   ```bat
   python -m pip install esptool
   ```

2. Connect the programmer to the board, put the ESP32 into download mode if your board/programmer does not do this automatically, then run this command from the directory containing `firmware.merged.bin`:

   ```bat
   esptool --chip esp32 --port COM8 --baud 460800 write-flash 0x0 firmware.merged.bin
   ```

   `COM8` is an example. Replace it with the actual serial port, such as `COM5`. If the `esptool` command is not on `PATH`, use `python -m esptool` in its place.

3. Wait for esptool to report success and let the board restart. Connect a computer to the same Ethernet subnet and open <http://10.90.90.90>.

The merged image writes the bootloader, partition table, OTA metadata initializer, and application from flash offset `0x0`. A full serial flash also resets settings stored in NVS; the device returns to its initial configuration (`10.90.90.90`, login `admin`, password `admin`).

### Update through the web interface

Use the web OTA page to upload `firmware.bin` on an already running compatible device. Do not upload `firmware.merged.bin` through OTA.

## Русский

### Прошивка через USB–UART-программатор

1. Установите esptool из командной строки Windows:

   ```bat
   pip install esptool
   ```

   Если команда `pip` не найдена, выполните:

   ```bat
   python -m pip install esptool
   ```

2. Подключите программатор к плате. Если плата или программатор не делают это автоматически, переведите ESP32 в режим загрузки прошивки. Откройте командную строку в папке с `firmware.merged.bin` и выполните:

   ```bat
   esptool --chip esp32 --port COM8 --baud 460800 write-flash 0x0 firmware.merged.bin
   ```

   `COM8` — пример. Замените его на фактический COM-порт, например `COM5`. Если команда `esptool` не найдена, используйте `python -m esptool` вместо `esptool`.

3. Дождитесь сообщения esptool об успешной записи и перезапуска платы. Подключите компьютер к той же Ethernet-подсети и откройте <http://10.90.90.90>.

Объединённый образ записывает загрузчик, таблицу разделов, данные инициализации OTA и приложение начиная со смещения `0x0`. Полная прошивка через программатор также сбрасывает сохранённые в NVS настройки: устройство вернётся к исходным параметрам (`10.90.90.90`, логин `admin`, пароль `admin`).

### Обновление через веб-интерфейс

Для OTA на уже работающем совместимом устройстве загрузите файл `firmware.bin` на странице OTA. Не загружайте через OTA файл `firmware.merged.bin`.
