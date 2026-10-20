# MQTT

MQTT is optional and disabled by default. Configure a broker, port, optional username/password, topic prefix, and publication interval in Settings.

The controller publishes telemetry only. It does not subscribe to topics and cannot receive MQTT control commands. The current connection uses plain TCP without TLS; use a trusted, isolated network.

## Topics

The device identifier is the lower-case 12-hex-digit MAC suffix of the WT32-ETH01. With the default topic prefix `fl_energy`, the topics are:

| Topic | Retained | Payload |
| --- | --- | --- |
| `fl_energy/<device-id>/state` | Yes | Firmware version, uptime, time validity, conditioner observations, and group state. |
| `fl_energy/<device-id>/event` | No | One alarm transition: availability, fault, degraded group, or high group temperature; includes problem/recovery state and event text. |
| `fl_energy/<device-id>/status` | Yes | `online` while connected; the retained Last Will sets `offline` after an unexpected disconnect. |

Replace `fl_energy` with the configured prefix in each topic. The state topic is published at the configured interval. Unknown, stale, or unconfirmed measurements are JSON `null`; temperatures are integer tenths of °C in fields ending in `_deci_c`.

Credentials are stored in NVS and excluded from the general `/json` snapshot. Broker credentials and payloads are not protected by TLS in this firmware.
