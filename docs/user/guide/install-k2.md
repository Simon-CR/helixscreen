# Installing HelixScreen on the Creality K2 Series

End-to-end guide for the Creality K2, K2 Plus, and K2 Pro: install, update, and uninstall. HelixScreen runs on the stock firmware, in place of the stock screen UI.

> **Tested state:** works with stock firmware and stock Moonraker (no custom firmware required). Root access is enabled from the printer's own settings menu.

## Prerequisites

- **Hardware:**
  - Creality K2, K2 Plus, or K2 Pro
  - Stock 4.3" touchscreen display (480x800)
  - Network connection

- **Software:**
  - Stock firmware with root access enabled (Settings > "Root account information")
  - SSH access (`root@<printer-ip>`, password: `creality_2024`)
  - Moonraker is included in stock firmware; the wizard's default port (`7125`) needs no changes (Creality's own web front also reaches Moonraker through a proxy on 4408)

## Install

SSH into the printer and run:

```bash
python3 -c "import urllib.request as u;u.urlretrieve('https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh','/tmp/install.sh')" && sh /tmp/install.sh
```

> **Why not `wget`?** Recent K2 firmware (Tina/OpenWrt) ships neither `wget` nor `curl` on the `PATH`: even the BusyBox `wget` applet has been compiled out. Every K2 includes `python3` (Klipper and Moonraker need it) with working SSL, so the command above uses Python to fetch the installer over HTTPS; the installer then uses Python for the rest of the download and extraction. If your firmware still has `wget` (older builds did), `wget -O - http://dl.helixscreen.org/install.sh | sh` also works.

After installation, the setup wizard runs on the touchscreen. Moonraker is already on the printer, so the wizard's connection step needs no changes.

## What the Installer Does Here

- Installs to `/opt/helixscreen/`, with a boot service at `/etc/init.d/S99helixscreen`
- Stops and persistently disables the stock Creality UI service (`/etc/init.d/app`). Creality's `web-server` is intentionally left running, so the Creality Cloud app and its camera stream keep working after the install. Known issue: on the next reboot, disabling the stock UI service also stops `web-server`, and Creality Cloud stops working (prestonbrown/helixscreen#1617)
- Stages the download, caches, and logs on `/mnt/UDISK` (the large user partition): `/opt` sits on a small system overlay that a release archive would fill
- Waits up to two minutes for Moonraker during boot, then starts the UI anyway; it reconnects on its own

## Service Control and Logs

```bash
/etc/init.d/S99helixscreen start|stop|restart|status
```

Two log streams; collect both when reporting an issue:

```bash
# Structured app log (on the UDISK data partition, rotated):
tail -100 /mnt/UDISK/helixscreen/logs/helix.log
# Launcher / crash capture (lives beside the install; on builds whose
# /var/log is persistent it lands at /var/log/helixscreen/launcher.log):
tail -100 /opt/helixscreen/logs/launcher.log
# Anything that reached the OpenWrt syslog:
logread | grep helix-screen | tail -100
```

## Updating

Use the bundled installer (no need to download it again):

```bash
/opt/helixscreen/install.sh --update
```

To pin a specific version, add `--version vX.Y.Z` (find the latest on the [releases page](https://github.com/prestonbrown/helixscreen/releases/latest)). Your settings are preserved; see [UPGRADING.md](../UPGRADING.md) for what `--update` keeps and what `--clean` resets.

Check the current version over SSH:

```bash
/opt/helixscreen/bin/helix-screen --version
```

## Uninstalling

```bash
cp /opt/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
```

This removes HelixScreen, re-enables and starts the stock Creality UI service (`/etc/init.d/app`), and hands the chamber camera back to the stock camera app, which also restores Creality's stock AI failure detection (see the note below).

## Quirks and Notes

**What's different from the K1:**
- ARM processor (Allwinner, not MIPS): standard cross-compilation
- Stock Moonraker: no community firmware required
- OpenWrt-based init system (procd, not SysV)
- CFS (Creality Filament System) support for RS-485 filament management

**Stock AI failure detection is disabled while HelixScreen is installed.** The stock detector rides the same service as the stock UI and watches the chamber camera, and HelixScreen takes over both. HelixScreen does not yet ship a replacement detector; one is tracked in [issue #1033](https://github.com/prestonbrown/helixscreen/issues/1033). Uninstalling restores the stock detector.

**The portrait panel is software-rotated to landscape.** That is the supported configuration; running it un-rotated with the portrait layout is alpha and mostly untested. On this lower-power CPU, animations and 3D bed-mesh rendering may be throttled.

## See Also

- [Supported Printers: Creality K2 Plus / K2 Pro](supported-printers.md#creality-k2-plus--k2-pro): CFS integration detail
- [Troubleshooting](../TROUBLESHOOTING.md): including [CFS shows no slots](../TROUBLESHOOTING.md#cfs-shows-no-slots-creality-k2)
- [UPGRADING.md](../UPGRADING.md): version pinning, factory reset, what a reset clears
- [Installation overview](../INSTALL.md): all other platforms
