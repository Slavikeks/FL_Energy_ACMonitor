#!/usr/bin/env python3
"""Check that the compact MIB schema agrees with the runtime OID branches."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/snmp/SnmpService.cpp").read_text(encoding="utf-8")
MIB = (ROOT / "web/assets/FL-ENERGY-ACMONITOR-MIB.mib").read_text(encoding="utf-8")
WEB = (ROOT / "web/index.html").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"SNMP MIB check failed: {message}")


enterprise = re.search(
    r"flEnergyAcMonitor\s+MODULE-IDENTITY[\s\S]*?::=\s*\{\s*enterprises\s+(\d+)\s*\}",
    MIB,
)
require(enterprise is not None, "module identity must be assigned below enterprises")
require(enterprise.group(1) == "67213", "MIB must use the registered FLINK LLC PEN 67213")
expected_root = f".1.3.6.1.4.1.{enterprise.group(1)}"
root = re.search(r'constexpr char kEnterpriseRoot\[\]\s*=\s*"([^"]+)"', SOURCE)
require(root is not None and root.group(1) == expected_root, "C++ enterprise root differs from MIB root")

require(re.search(r"(?m)^\s*DisplayString,\s*TEXTUAL-CONVENTION\s*$", MIB) is not None,
        "trap DisplayString must use the SNMPv2-TC definition")
for alarm_text in ("flAlarmAcName", "flAlarmGroupName", "flAlarmText"):
    require(re.search(rf"(?m)^{alarm_text}\s+OBJECT-TYPE[\s\S]*?SYNTAX\s+DisplayString\b", MIB) is not None,
            f"{alarm_text} must use the interoperable ASCII DisplayString convention")

expected_branches = {
    "flProduct": ("flEnergyAcMonitor", "1", 'rootedOid(".1")'),
    "flAlarm": ("flEnergyAcMonitor", "2", None),
    "flAcTable": ("flEnergyAcMonitor", "3", "rootedOid(\".3.1\")"),
    "flGroupTable": ("flEnergyAcMonitor", "4", "rootedOid(\".4.1\")"),
}
for name, (parent, arc, runtime_branch) in expected_branches.items():
    pattern = rf"(?m)^{name}\s+(?:OBJECT IDENTIFIER|OBJECT-TYPE)[\s\S]*?::=\s*\{{\s*{parent}\s+{arc}\s*\}}"
    require(re.search(pattern, MIB) is not None, f"MIB branch {name} has the wrong parent/arc")
    if runtime_branch:
        require(runtime_branch in SOURCE, f"runtime is missing {runtime_branch}")
for suffix in (".2.10",):
    require(f'rootedOid("{suffix}")' in SOURCE, f"runtime is missing trap bindings branch {suffix}")


def object_columns(parent: str) -> dict[str, int]:
    columns: dict[str, int] = {}
    for match in re.finditer(
        r"(?m)^([A-Za-z][A-Za-z0-9]*)\s+OBJECT-TYPE\b[\s\S]*?::=\s*\{\s*(\w+)\s+([^}]+)\}", MIB
    ):
        name, branch, arcs = match.groups()
        if branch == parent:
            parts = arcs.split()
            if len(parts) == 1 and parts[0].isdigit():
                columns[name] = int(parts[0])
    return columns


ac = object_columns("flAcEntry")
groups = object_columns("flGroupEntry")
ac_names = [
    "flAcAddress", "flAcName", "flAcAvailability", "flAcObservedPower", "flAcObservedMode",
    "flAcObservedSetpoint", "flAcObservedFan", "flAcObservedT1", "flAcObservedT2", "flAcErrorFlags",
    "flAcObservedFanAuto", "flAcCompressorRunning", "flAcProtectionFlags", "flAcObservedT2B", "flAcObservedT3",
]
group_names = [
    "flGroupId", "flGroupName", "flGroupMode", "flGroupState", "flGroupRequiredN",
    "flGroupMaximumM", "flGroupActive", "flGroupMaxT1",
]
require([ac.get(name) for name in ac_names] == list(range(1, 16)), "AC columns must be exactly 1..15 in declared order")
require([groups.get(name) for name in group_names] == list(range(1, 9)), "group columns must be exactly 1..8 in declared order")

sync = SOURCE.split("void sync(const RuntimeState& state", 1)[1].split("\n};", 1)[0]
ac_sync = sync.split('rootedOid(".3.1")', 1)[1].split("std::array<bool, kMaxGroups>", 1)[0]
group_sync = sync.split('rootedOid(".4.1")', 1)[1].split("for (std::size_t i = 0; i < group_rows.size()", 1)[0]
ac_runtime_columns = [int(x) for x in re.findall(r"sync(?:Integer|Name)\([^;]*?root\.c_str\(\),\s*(\d+)", ac_sync, re.S)]
group_runtime_columns = [int(x) for x in re.findall(r"sync(?:Integer|Name)\([^;]*?root\.c_str\(\),\s*(\d+)", group_sync, re.S)]
require(ac_runtime_columns == list(range(1, 16)), "runtime AC callbacks do not match the 1..15 MIB columns")
require(group_runtime_columns == list(range(1, 9)), "runtime group callbacks do not match the 1..8 MIB columns")

require(not re.search(r"(?im)^\s*fl\w*(?:Bus|RS485|LastSeen|LastResponse)\w*\s+(?:OBJECT-TYPE|NOTIFICATION-TYPE)", MIB),
        "bus or last-response object found in MIB")
notifications = re.findall(r"(?im)^\s*(fl\w+)\s+NOTIFICATION-TYPE\b", MIB)
require(notifications == ["flAcOffline", "flAcFault", "flGroupDegraded", "flGroupOverTemperature"],
        "MIB must define exactly the four supported alarm notifications")
require("SNMP_VERSION_2C" in SOURCE and "SNMP_VERSION_1" not in SOURCE,
        "runtime traps must use SNMPv2c")
require('rootedOid(".2.0.") + std::to_string(impl_->trap_type)' in SOURCE,
        "runtime notification OID must match the MIB alarm type arc")
require(re.search(r"(?m)^flAlarmNotifications OBJECT IDENTIFIER ::= \{ flAlarm 0 \}$", MIB) is not None,
        "notifications must use their dedicated .0 subtree below flAlarm")
notification_start = MIB.find("flAcOffline NOTIFICATION-TYPE")
notification_arcs = re.findall(r"(?im)^\s*::=\s*\{\s*flAlarmNotifications\s+(\d+)\s*\}", MIB[notification_start:])
require(notification_arcs[:4] == ["1", "2", "3", "4"],
        "the four v2 notifications must use alarm arcs 1..4")

help_start = WEB.find('data-page="help"')
help_end = WEB.find("</section>", help_start)
require(help_start >= 0 and help_end > help_start, "web Help page is missing")
help_page = WEB[help_start:help_end]
require("Midea XYE" in help_page and "SNMPv2c" in help_page and "MQTT" in help_page,
        "Help page must explain the bus protocol and network integrations")
for column in range(1, 16):
    require(f".3.1.{column}.A" in help_page, f"Help page is missing AC column {column}")
for column in range(1, 9):
    require(f".4.1.{column}.G" in help_page, f"Help page is missing group column {column}")
for arc in range(1, 5):
    require(f"{expected_root}.2.0.{arc}" in help_page,
            f"Help page is missing notification OID {arc}")
require(".2.10.8.0" in help_page, "Help page is missing the final trap varbind")

print("SNMP MIB, runtime OIDs, notifications and Help reference agree")
