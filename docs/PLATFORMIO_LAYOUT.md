# PlatformIO workspace

The checkout contains source code, pinned project configuration, documentation, tests, and web assets. PlatformIO Core, Python virtual environments, downloaded toolchains/SDKs, dependency caches, and build outputs belong outside the checkout.

`platformio.ini` sets the PlatformIO workspace relative to `platformio.core_dir`. With a default Core installation, the project output is under:

```text
<PlatformIO Core directory>/workspaces/FL_Energy_ACMonitor/
  build/wt32-eth01/
  build/native/
  libdeps/native/Unity/
```

The common firmware output is `build/wt32-eth01/firmware.bin`. The exact Core directory is machine-specific; query it with `pio system info`. No user name or absolute workstation path is required in the project configuration.

`platformio.local.ini.example` is a safe template for local serial-port settings. Copy it to `platformio.local.ini` and keep the local file untracked. Never place credentials or private network settings in a committed example.

The repository's `.gitignore` excludes PlatformIO products, SDKs, toolchains, local IDE state, virtual environments, local configuration, secrets, and compiled binary files.
