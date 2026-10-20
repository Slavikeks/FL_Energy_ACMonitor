Import("env")
import base64
import os
from pathlib import Path
import subprocess
import sys

project = Path(env.subst("$PROJECT_DIR"))
generated = Path(env.subst("$BUILD_DIR")) / "generated"
generated.mkdir(parents=True, exist_ok=True)
check_env = os.environ.copy()
check_env["PYTHONDONTWRITEBYTECODE"] = "1"
subprocess.run([sys.executable, str(project / "scripts" / "check_snmp_mib.py")], check=True, env=check_env)
page = (project / "web" / "index.html").read_text(encoding="utf-8")
logo = (project / "web" / "assets" / "fl_energy_logo.svg").read_bytes()
mib = (project / "web" / "assets" / "FL-ENERGY-ACMONITOR-MIB.mib").read_text(encoding="utf-8")
logo_uri = "data:image/svg+xml;base64," + base64.b64encode(logo).decode("ascii")
placeholder = 'src="assets/fl_energy_logo.svg'
if page.count(placeholder + '"') != 2:
    raise RuntimeError("Expected exactly two logo references in the embedded page")
page = page.replace(placeholder + '"', 'src="' + logo_uri + '"')
(generated / "EmbeddedWebAssets.h").write_text(
    "#pragma once\ninline constexpr char kEmbeddedIndexHtml[] = R\"FLDATA(" + page + ")FLDATA\";\n"
    "inline constexpr char kEmbeddedMib[] = R\"FLMIB(" + mib + ")FLMIB\";\n",
    encoding="utf-8",
)
env.Append(CPPPATH=[str(generated)])
