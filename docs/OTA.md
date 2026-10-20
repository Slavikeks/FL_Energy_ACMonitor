# OTA firmware updates

OTA uploads a single PlatformIO `firmware.bin` image over the authenticated web interface. The image includes the firmware and embedded web assets.

## Before updating

1. Build the `wt32-eth01` environment from this repository, or download the matching WT32-ETH01 image from the project’s GitHub Release page.
2. Confirm that the image belongs to the 4 MiB flash / 1,966,080-byte OTA-slot layout.
3. Keep Ethernet power stable and do not interrupt the upload or reboot.

## Update through the UI

1. Sign in to the controller.
2. Open OTA, choose `firmware.bin`, and start the upload.
3. Wait for the page to report that the update was accepted and the controller is restarting.
4. Reopen the controller after reboot and check the version chip, Status page, and event log.

The endpoint requires an authenticated session and CSRF token. A successful upload response means the image was accepted; it does not replace post-reboot verification.

The partition table contains two application slots. The project has no separate automatic rollback policy. For an uninitialized board or recovery, use a USB-to-UART programmer and the full PlatformIO upload process described in [Installation](INSTALLATION.md).
