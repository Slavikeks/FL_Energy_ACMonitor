# Changelog

## 0.7.4 - 2026-10-20

- Add an English web interface and a Russian/English language switch.
- Keep the selected interface language in the browser across sessions.

## 0.7.3 - 2026-10-20

- Add a plain-language Help guide to group modes, N/M limits, temperature response, failover, rotation, ventilation standby and per-conditioner manual control.
- Replace the outdated stage label with a clear FL Energy AC Monitor startup message in the event log and Serial output.

## 0.7.2 - 2026-10-19

- Add optional fan-only ventilation standby for inactive members of AUTO groups; standby members are excluded from N/M and are promoted or demoted as group demand changes.
- Add a persisted per-group card color, show ventilation reserves in `/json` and the web UI, and remove redundant manual-control helper text.

## 0.7.1 - 2026-10-19

- Add per-conditioner manual control for AUTO-group members without removing them from the group.
- Reapply AUTO-group mode, setpoint and detectable fan-setting changes made from the conditioner remote; count powered manual-control units against M.

## 0.7.0 - 2026-10-19

- Promote model-validated C0 readings immediately so the automation loop cannot mistake a valid power status for a transient `power_unknown`.
- Gate reserve starts while any group member's power state is uncertain; after the configured three consecutive failed C0 polls, allow failover with an explicit log reason.
- For a known powered unit reporting an operational fault, confirm it is off before starting its reserve; expose the consecutive poll-failure count in `/json`.

## 0.6.9 - 2026-10-19

- Keep running group members in the healthy count through brief poll gaps, aligned with the bus's configured offline threshold, to avoid starting a reserve after a transient missed response.
- Expose each conditioner’s automation health reason and `ccm_error_flags` in `/json`; include the excluded member and reason in automatic start diagnostics.

## 0.6.8 - 2026-10-19

- Add break-before-make scheduled rotation (`handover_s=0`) so a group can rotate with `N=M` and never intentionally run a replacement concurrently.
- Keep the rotation interval anchored from the moment rotation is enabled; expose next rotation epoch/countdown and high/recovery delay countdowns in `/json`.
- Explain group-save validation failures in the web toast and record automatic start/stop reasons in the event log and `/json`.
- Label XYE commands from group automation as automatic instead of manual; prevent a single-unit group from starting a reserve while any member's power state is stale or uncertain.

## 0.6.7 - 2026-10-18

- Verify C3 conditioner commands with delayed, repeated C0 reads instead of resending a command when the reported state is still catching up.


## 0.6.6 - 2026-10-18

- Label conditioner temperatures by sensor location on the Status cards and in Help/MIB descriptions.
- Collapse each conditioner control panel by default; open and close controls independently per card.


## 0.6.5 - 2026-10-18

- Show available T1, T2, T2B and T3 readings on conditioner cards; omit tiles for unavailable or stale sensors.
- Add T2B and T3 to MQTT state and append SNMP AC table columns 14 and 15; keep missing readings null in MQTT and absent in SNMP walks.

## 0.6.4 - 2026-10-18

- Decode and expose actual fan speed, automatic fan mode and compressor state in JSON, Status, SNMP and MQTT.
- Add T2, T2B and T3 temperatures to `/json` for hardware inspection.
- Show conditioner error and protection masks on Status cards only when active; identify known defrost protection and preserve unknown bits as a hexadecimal mask.
- Extend the SNMP AC table and MIB with automatic fan mode, compressor state and protection flags.

## 0.6.3 - 2026-10-16

- Add conditioner power and fan speed indicators to Status cards; show reported actual speed when available, otherwise the last selected speed.
- Restore the selected mode, fan speed and setpoint from the last requested settings, including after an OFF command.
- Keep omitted settings intact in runtime desired state when a partial command is queued.

## 0.6.2 - 2026-10-16

- Display the addresses discovered during an XYE bus scan alongside the total and scan progress.
- Preserve conditioner control form values while editing; skip card refreshes during active input and retain submitted values until command confirmation.
- Use the clearer Russian UI label «Установка» for the temperature setpoint throughout the interface and API documentation.

## 0.6.1 - 2026-10-16

- Preserve edited Network/NTP and SNMP/MQTT fields across periodic state refreshes until the user saves them.
- Make the automation toggle update only its own setting, so it cannot submit pending network edits.

## 0.5.3 - 2026-10-16

- Replaced the temporary experimental SNMP root with FLINK LLC's registered IANA Private Enterprise Number 67213.
- Updated the runtime system OID, SNMP tables, notification OIDs, downloadable MIB and structural checks to use the enterprise root.
- Placed notifications under the enterprise notification `.0` subtree, matching the layout used by the owner's working MDB Monitor MIB.

## 0.5.2 - 2026-10-14

- Restored SNMPv2c traps and aligned every `snmpTrapOID.0` value with the four notifications in the main MIB.
- Made trap text printable ASCII and transliterated Cyrillic conditioner/group names for consistent display in Windows receivers.
- Replaced named zones with fixed numeric UTC offsets and balanced the Settings grid.
- Removed the unused SNMPv1 trap MIB and endpoint.

## 0.5.1 — 2026-10-14

- Added a timezone dropdown with common UTC offsets and daylight-saving zones; unknown saved POSIX zones remain selectable.
- Made the SNMP RO community and MQTT username/password visible and editable, and removed the obsolete clear-secret checkboxes.
- Added a session-protected, no-store settings endpoint for loading saved secrets; `/json` continues to omit secrets.
- Blank editable secret fields now clear the corresponding stored value.
- Kept SNMP GET/WALK on v2c and changed outgoing traps to SNMPv1 enterprise-specific format for the configured legacy Trap Watcher; the MIB describes both v1 and v2 notification OIDs.

## 0.5.0 — 2026-10-14

- Added a compact read-only SNMPv2c system group, live AC/group tables and four debounced alarm notifications with recovery.
- Reserved a temporary root under the IETF experimental subtree; it is explicitly not a Private Enterprise Number. No RS-485 OIDs or AC last-response timestamps are exposed.
- Added authenticated MIB download at `/mib` and SNMP/MQTT configuration with secrets kept out of `/json`.
- Added outbound-only MQTT state, event and retained online/offline status topics; MQTT is disabled by default and accepts no commands.
- Added debounced alarm management, bounded event queues, persistent management settings, and native tests for alarms and telemetry payloads.
- SNMP walk/trap delivery and MQTT broker exchange remain unverified on the physical device; see `docs/IMPLEMENTATION_STATUS.md` for build and test results.

## 0.4.1 — 2026-10-14

- Moved the supplied logo into web/assets, embedded it in the firmware UI, and replaced the placeholder mark.
- Removed redundant page headings and the Status-page scan button; made Settings bus scan prominent.
- Combined registered/online counts, added running-unit count excluding standby units, and showed poll_ok, timeouts, and checksum_errors in the status cards.
- Added local event timestamps with second precision to the Log page.
- Native suite: 20/20 passed. WT32-ETH01 build: RAM 120,232 B, linked flash 962,273 B, firmware.bin 968,848 B; OTA slot has 997,232 B free.
- Physical C0/C3 behavior and the 0.4.1 OTA cycle remain unverified on hardware.

## 0.4.0 — 2026-10-14

- Added responsive Status, Settings, Log, OTA and Help pages, current firmware version, and a formatted standalone /json view.
- Added authenticated AC removal and group create/update/delete. A conditioner referenced by a group must first be removed from that group.
- Added N-of-M group persistence/validation, manual/auto modes, failover, temperature hysteresis, scheduled rotation, and verified make-before-break handovers.
- Automatic control remains disabled by default; the startup observe-only period is 60 seconds. Server policy blocks Web commands to AUTO-group members.
- Native suite: 20/20 passed, including 1-of-2, 2-of-3, 3-of-5, failed replacement, no-failback, missing T1, and missing NTP cases.
- WT32-ETH01 build: RAM 120,232 B, flash 959,261 B, OTA image 965,840 B; OTA slot has 1,000,240 B free.
- Real conditioner behavior and the 0.4.0 OTA cycle remain unverified on hardware.

## 0.0.0 — 2026-10-14

- Stage 0 PlatformIO WT32-ETH01 and native, pinned dependencies.
- Confirmed 4 MiB flash layout, two OTA slots, build size/layout checks.
- Bounded typed Config/StateStore/Observed/Desired/Registry/Group and module APIs.
- Minimal Serial/uptime firmware and host model tests.
- Architecture, build instructions, status and attribution.

### Stage 1 implementation

- Added WT32-ETH01 Ethernet DHCP/static defaults, single-server SNTP and timezone settings.
- Added versioned authenticated `/json`, login/logout/password change, HttpOnly sessions and CSRF.
- Persisted network/time/auth settings in a versioned CRC-checked NVS record.
- Added 200-entry RAM event ring and embedded web settings/status/log interface.
- Added authenticated multipart OTA for `firmware.bin`; firmware image and partition checks pass.
- Owner selected first login `admin/admin` with no forced password rotation.
- Reduced the minimum length for a changed password to 6 characters.
- Runtime capacity is currently 16 registered units and groups; XYE address range is 0–63.

At the time of the Stage 1 build, the board network and full OTA reboot cycle were still awaiting owner verification.

### Stage 2 and Stage 3 progress

- Owner confirmed that the recovered firmware starts, the web interface and authentication work, the changed password persists, and the XYE scan produces RS-485 activity. No conditioner is connected, so C0 data and physical control have not been validated.
- Added C3 frame encoding for ON/OFF, COOL/HEAT/FAN/DRY, fan selection, and whole-degree setpoints. Commands pass through one bounded bus queue, use bounded retries, and require follow-up C0 verification for mode and setpoint.
- Manual commands remain gated until fresh online readings and explicit per-address model validation; automatic control remains disabled. C0 cannot confirm the selected fan speed.
- Added inline invalid-login feedback and reset of the OTA progress/file state after the rebooted device loses its session.
- Native simulation suite: 12/12 tests passed. WT32-ETH01 image and 4 MiB OTA slot check passed.
