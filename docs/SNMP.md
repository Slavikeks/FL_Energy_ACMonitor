# SNMP

SNMP is optional and disabled by default. Enable it in Settings and set a read-only community. The same configured community is used by the SNMP agent and outgoing traps.

## Agent

- Protocol: SNMPv2c, read-only; UDP port 161.
- Supported queries: GET, GETNEXT, and GETBULK. SET is not accepted.
- Enterprise root: `1.3.6.1.4.1.67213` (FLINK LLC PEN 67213).
- MIB: [FL-ENERGY-ACMONITOR-MIB.mib](../web/assets/FL-ENERGY-ACMONITOR-MIB.mib), also downloadable from the Help page and `/mib`.

The standard system group is available under `1.3.6.1.2.1.1`. The conditioner table is `1.3.6.1.4.1.67213.3.1` and the group table is `1.3.6.1.4.1.67213.4.1`. Conditioner rows are indexed by XYE address; group rows by group ID. The MIB names and describes each column.

Only fresh, confirmed observations are exposed for optional sensor/state columns. An unknown or unsupported reading is omitted from a walk rather than represented as a false zero. The OID tree intentionally omits RS-485 counters and last-response timestamps.

Example walk using Net-SNMP:

```sh
snmpwalk -v2c -c '<read-only-community>' <device-address> 1.3.6.1.4.1.67213
```

## Traps / notifications

Set one receiver IPv4 address and UDP port in Settings (162 is the conventional receiver port). Notifications are SNMPv2c and use the same community as the agent.

| Notification OID | Meaning |
| --- | --- |
| `1.3.6.1.4.1.67213.2.0.1` | Conditioner availability changed. |
| `1.3.6.1.4.1.67213.2.0.2` | Conditioner fault appeared or cleared. |
| `1.3.6.1.4.1.67213.2.0.3` | Group became degraded or recovered. |
| `1.3.6.1.4.1.67213.2.0.4` | Group temperature alarm appeared or recovered. |

Each notification includes standard sysUpTime.0 and snmpTrapOID.0 plus event state, severity, event type, conditioner name, group name, short text, timestamp, and code/value bindings under `.1.3.6.1.4.1.67213.2.10`. Event text is ASCII-safe for monitoring tools with limited UTF-8 display support.

SNMPv2c provides no encryption or authentication beyond the community string. Use it only on a trusted, isolated network. For exact types and enumerations, import the supplied MIB.
