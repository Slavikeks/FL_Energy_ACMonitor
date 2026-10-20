# Web UI

The controller serves its management UI over HTTP on the configured Ethernet address. Sign in with the initial `admin` / `admin` account; passwords can be changed in Settings, but rotation is not forced. The interface language is Russian or English, selected by the button on the login page or in the top bar. The browser remembers the choice.

## Pages

| Page | Purpose |
| --- | --- |
| Status | Conditioner readings, bus counters, group state, and per-conditioner controls. |
| Settings | Ethernet/NTP, SNMP/MQTT, global automation, conditioner registry, and group configuration. |
| Log | Recent device events. The event ring buffer is in RAM and is cleared by reboot. |
| OTA | Authenticated upload of a PlatformIO `firmware.bin` image. |
| Help | Device, protocol, OID, integration, and group-operation reference. |
| `/json` | Authenticated, formatted runtime snapshot for diagnostics and integrations. |

## Conditioner controls

Conditioner cards show only measurements that are currently available. Sensor channels may differ between models. A member of an AUTO group is controlled by its group unless its per-conditioner Manual control option is enabled. While manual control is enabled, the group leaves its settings alone; clear the option to return control to the group.

## Groups

Groups can be visual-only (MANUAL) or managed by the RotationEngine (AUTO). AUTO groups require the global automation switch to be enabled. Parameters include N and M limits, operating mode, setpoint, fan, temperature thresholds and delays, schedule, handover, verification, and minimum on/off times. See [RotationEngine](ROTATION_ENGINE.md) for how these settings interact.

## Security notes

The UI uses HTTP without TLS. Use it on a trusted network and do not forward it directly to the public Internet. Change the default password. SNMP/MQTT secrets can be edited in Settings; the general `/json` response does not include their values.
