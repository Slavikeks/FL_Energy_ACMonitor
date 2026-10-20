"""Check the OTA layout without PlatformIO or a connected device."""
import argparse
import csv
from pathlib import Path

FLASH_BYTES = 4 * 1024 * 1024


def validate(path: Path) -> int:
    rows = []
    for row in csv.reader(path.read_text(encoding="utf-8").splitlines()):
        if not row or row[0].lstrip().startswith("#"):
            continue
        name, kind, subtype, offset, size = [item.strip() for item in row[:5]]
        rows.append((name, kind, subtype, int(offset, 0), int(size, 0)))
    names = [row[0] for row in rows]
    if len(names) != len(set(names)):
        raise ValueError("Duplicate partition names")
    previous_end = 0x9000
    for name, kind, _, offset, size in sorted(rows, key=lambda row: row[3]):
        alignment = 0x10000 if kind == "app" else 0x1000
        if size <= 0 or size % 0x1000 or offset % alignment:
            raise ValueError(f"Invalid size/alignment: {name}")
        if offset < previous_end or offset + size > FLASH_BYTES:
            raise ValueError(f"Overlap or flash overflow: {name}")
        previous_end = offset + size
    apps = [row for row in rows if row[1] == "app"]
    if len(apps) != 2 or {row[2] for row in apps} != {"ota_0", "ota_1"}:
        raise ValueError("Expected exactly two OTA app slots")
    if apps[0][4] != apps[1][4]:
        raise ValueError("OTA slot sizes must match")
    if not any(row[1:3] == ("data", "ota") and row[4] == 0x2000 for row in rows):
        raise ValueError("Expected 8 KiB otadata")
    if not any(row[1:3] == ("data", "nvs") for row in rows):
        raise ValueError("NVS partition missing")
    return apps[0][4]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--table", type=Path, default=Path(__file__).resolve().parents[1] / "partitions_4mb_ota.csv")
    parser.add_argument("--firmware", type=Path)
    args = parser.parse_args()
    slot_size = validate(args.table)
    print(f"Partitions OK: flash={FLASH_BYTES}; OTA slot={slot_size} bytes")
    if args.firmware:
        size = args.firmware.stat().st_size
        if size > slot_size:
            raise ValueError(f"Firmware {size} bytes exceeds OTA slot {slot_size}")
        print(f"firmware.bin={size} bytes; OTA headroom={slot_size - size} bytes")


if __name__ == "__main__":
    main()
