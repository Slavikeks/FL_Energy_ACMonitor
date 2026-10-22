"""Patch the pinned SNMP_Agent BER encoder's 256-byte length boundary."""
Import("env")

from pathlib import Path


libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")
source = libdeps / "SNMP_Agent" / "src" / "BEREncode.cpp"
if not source.is_file():
    raise RuntimeError("SNMP_Agent BEREncode.cpp was not found in PlatformIO libdeps")

content = source.read_text(encoding="utf-8")
buggy = "if(integer > 256){"
fixed = "if(integer >= 256){"
occurrences = content.count(buggy)

if occurrences == 2:
    source.write_text(content.replace(buggy, fixed), encoding="utf-8")
    print("Patched SNMP_Agent BER length encoding for exactly 256 bytes")
elif occurrences == 0 and content.count(fixed) == 2:
    print("SNMP_Agent BER 256-byte length fix is already present")
else:
    raise RuntimeError(
        "Unexpected SNMP_Agent BEREncode.cpp layout; refusing to patch an unknown library version"
    )
