# Installing HelixScreen on the Elegoo Centauri Carbon

How to get HelixScreen onto an Elegoo Centauri Carbon: flash the community [OpenCentauri COSMOS](https://docs.opencentauri.cc/klipper-conversion/cosmos/cosmos/) firmware first, then install HelixScreen on top of it.

## Tested With

> **Tested and working.** Prebuilt binaries ship in releases and the installer has auto-detection support. Requires the community [OpenCentauri COSMOS firmware](https://docs.opencentauri.cc/klipper-conversion/cosmos/cosmos/); stock Elegoo firmware is not supported (no SSH, no Klipper, no Moonraker).

## Prerequisites

- **Hardware:**
  - Elegoo Centauri Carbon (4.3" 480×272 touchscreen, Allwinner R528, armv7l)
  - Network connection (WiFi or Ethernet)
  - A USB stick formatted FAT32 (for the firmware flash in Step 1)
- **Software:**
  - [OpenCentauri COSMOS firmware](https://github.com/OpenCentauri/cosmos/releases) installed (replaces stock Elegoo firmware; ships Klipper + Moonraker + grumpyscreen/atomscreen/guppyscreen)
  - SSH access: `root` / default password `OpenCentauri` (change it after install)

## Installation

### Step 1: Install COSMOS firmware

OpenCentauri COSMOS is a full firmware replacement for the Centauri Carbon. It ships with Klipper, Moonraker, Mainsail, and a `gui-switcher` that lets you pick which touch UI to run.

1. Download the latest `update.swu` from https://github.com/OpenCentauri/cosmos/releases
2. Copy it to the root of a FAT32-formatted USB stick
3. Insert the USB stick into the printer, power on
4. From the stock Elegoo UI, navigate to the firmware-update menu and apply the update
5. **First boot takes 5-10 minutes** while it reflashes the toolhead and bed boards; be patient
6. After reboot, connect to WiFi from the COSMOS UI and note the printer's IP address

If the update fails or the device won't boot, consult the OpenCentauri [install guide](https://docs.opencentauri.cc/klipper-conversion/cosmos/install/) and [emergency USB recovery](https://docs.opencentauri.cc/software/updates/) docs.

### Step 2: Install HelixScreen

SSH into the printer (replace `<ip>` with your printer's IP):

```bash
ssh root@<ip>
# Default password: OpenCentauri
```

Then run the installer:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

### Step 3: Switch back to another UI (optional)

COSMOS's `config-manager` tool lets you switch between installed UIs without uninstalling HelixScreen:

```bash
config-manager ui screen_ui grumpyscreen   # or atomscreen, guppyscreen, helixscreen
/etc/init.d/gui-switcher restart
```

## What the Installer Does on This Printer

The installer auto-detects COSMOS, installs HelixScreen to `/user-resource/helixscreen/`, and registers it with `gui-switcher` as the selected touch UI. It stops the currently active UI (grumpyscreen, atomscreen, or guppyscreen) and starts HelixScreen in its place.

- Install directory: `/user-resource/helixscreen/` (`/` is read-only squashfs on COSMOS)
- Init script: `/etc/init.d/helixscreen` (LSB-style, PIDFILE=`/var/run/gui.pid` for gui-switcher compatibility)

## Service Control and Logs

```bash
/etc/init.d/helixscreen restart

# Structured app log (COSMOS uses BusyBox in-memory syslog)
logread | grep helix-screen | tail -100

# Launcher / crash capture (startup, crash output)
tail -100 /user-resource/helixscreen/logs/launcher.log
```

## Updating

Re-run the installer with `--update`; it preserves your settings:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --update
```

To pin a specific version add `--version vX.Y.Z`, or swap `--update` for `--clean` to reinstall with fresh settings. See [Updating HelixScreen](../INSTALL.md#updating-helixscreen) for the universal details.

## Uninstalling

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall
```

The uninstaller reverses the `gui-switcher` registration, including the allowlist wrapper described below, and restores the UI that was running before.

## Quirks and Notes

- **Moonraker listens on port `80`** on COSMOS directly (no nginx); HelixScreen's `cc1` preset is configured for this
- **Factory white-balance calibration**: the `cc1` preset ships with per-channel panel gain so colors look neutral out of the box on the Centauri Carbon's 4.3" panel. No manual tuning needed
- **The `config-manager` allowlist**: COSMOS's `config-manager` has a fixed allowlist for the `screen_ui` slot. The installer handles this automatically via an init-script wrapper so HelixScreen can be selected without patching COSMOS itself; the uninstaller fully reverses it

**Testing on this printer?** Please report your results via [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) or [Discord](https://discord.gg/RZCT2StKhr).

## See Also

- [Supported printers](supported-printers.md#other-dedicated-builds): where the Centauri Carbon sits in the support matrix
- [Troubleshooting](../TROUBLESHOOTING.md): log collection and common problems
- [UPGRADING.md](../UPGRADING.md): version pinning, resets, migrations
