# INSTALL.md Restructure: Router + Per-Printer Guides

Status: design approved by Preston, 2026-09-13. Work ships on `release/1.0`,
then ports to `main`.

## Problem

`docs/user/INSTALL.md` is 1404 lines / 7,824 words and disjointed:

- Per-printer content lives in TWO places: "Prerequisites" blocks (K1, K2,
  AD5X, CC1, Sonic Pad, U1) and dedicated install chapters (Pi, AD5M, U1 only).
- The cartesian product of printers x lifecycle ops (update, uninstall, service
  management, log locations) repeats per-platform blocks four times over;
  roughly half the file is per-printer content.
- Straight duplication: Forge-X installer behavior told twice (Prerequisites
  and AD5M chapter), the Windows `scp -O` workaround three times, K1/AD5M
  two-step download blocks are near-twins.
- Heading-level oddities: two sibling `### Manual Installation` h3s; K1's
  install methods sit as siblings of the K1 section itself.

## Decisions (settled with Preston)

1. Router + per-printer guides. INSTALL.md shrinks to universal content plus a
   "which printer do you have" router table; each printer family gets an
   end-to-end guide owning install, update, upgrade, and uninstall for that
   platform.
2. UPGRADING.md is in scope: its per-printer blocks move into the spokes; it
   keeps universal upgrade content. TROUBLESHOOTING.md and
   guide/supported-printers.md are NOT restructured (link retargeting only).
3. The generic Linux/Pi path stays INLINE in INSTALL.md (Pi, BTT CB1/CB2/Manta,
   x86, any Debian-ish host - the majority path never leaves the page).

## File set

New spokes in `docs/user/guide/`, named `install-<family>.md` so they sort
together. `guide/creality-k1c-setup.md` already exists and KEEPS ITS NAME
(external links from Discord/forums point at it; it absorbs INSTALL.md's
duplicated K1 quick-install content instead).

| File | Absorbs from current INSTALL.md | Est. lines |
|---|---|---|
| `install-ad5m.md` | Prereqs, firmware-variants table, Forge-X prereqs, full AD5M chapter (merge the two "what the installer does" tellings into one), memory-constraints notes, Forge-X + Klipper-Mod update/uninstall/service blocks | ~260 |
| `install-k2.md` | K2 prereqs, python3-fetch install (no wget/curl on K2 firmware), differences-from-K1 list, K2 path rows | ~80 |
| `install-ad5x.md` | AD5X prereqs, ZMOD status, chroot manual-install section | ~90 |
| `install-cc1.md` | Centauri/COSMOS prereqs, 3-step install (COSMOS flash, HelixScreen, gui-switcher), notes | ~120 |
| `install-sonicpad.md` | Sonic Pad prereqs (SonicPad-Debian requirement), install, notes | ~60 |
| `install-u1.md` | U1 prereqs, full U1 chapter including firmware-upgrade-with-HelixScreen and blank-screen recovery sections | ~150 |
| `creality-k1c-setup.md` (exists, 219) | Absorbs K1 one-liner + two-step install currently duplicated in INSTALL.md | ~260 |

## Spoke skeleton (every guide follows this)

1. Title + one-line scope
2. Tested-state banner (firmware versions verified)
3. Prerequisites (hardware, software, firmware mod if required)
4. Install: easy path first; offline/manual paths in `<details>`
5. What the installer does on this platform (told ONCE)
6. Service control + log locations (the platform's rows from Getting Help's
   6-platform table)
7. Updating (this platform's `--update` / two-step / bundled-installer path)
8. Uninstalling / reverting to stock
9. Quirks and notes (AD5M memory, AD5X chroot, CFS/IFS pointers)
10. Cross-links: TROUBLESHOOTING printer section, supported-printers family
    section, UPGRADING for migrations

## INSTALL.md final shape (~300 lines)

1. Quick Start: the one-liner + KIAUH note + run-on-the-host warning
2. Generic Linux / Pi / BTT / x86 (INLINE): prereqs, glibc 2.31/Bullseye note
   with cc1-package escape hatch, install walkthrough, what the installer does,
   wizard pointer; Pi 5 DRM devices, camera turbojpeg, and low-memory notes
   fold in here from the current "Platform-Specific Notes"
3. Remote Screen Setup (separate device)
4. Android App (Experimental)
5. Router table: one row per family (K1/K1C, K2, AD5M, AD5X, CC1, Sonic Pad,
   U1) with each platform's quick-start command + link to its guide
6. First Boot & Setup Wizard (universal)
7. Display Configuration (universal; takes main's "Display Backends: DRM vs
   Framebuffer" rewrite, see Fidelity)
8. Updating (universal): update_manager via Mainsail/Fluidd, `--update`,
   `--version`, `--clean`, manual update-manager setup with a path-hint table
9. Uninstalling (universal): `--uninstall` restores previous UI; platform
   specifics live in the spokes
10. Getting Help: journalctl + "log locations are in your printer's guide"

## UPGRADING.md final shape (~120 lines)

Moves out: Forge-X/Klipper-Mod quick-upgrade command blocks, AD5X chroot note,
per-platform binary-path listings (each spoke's Updating section carries its
own).
Stays: setup-wizard-keeps-appearing fix, factory reset from UI, what's
preserved vs affected by a reset, version pinning (`--version`, `--clean`),
on-screen version check.

## Link repair (no anchor stubs left behind)

Retarget inbound links rather than keeping relay headings. Files touched:

- `guide/supported-printers.md`: 6 anchor links retarget to the new guide
  files (`#flashforge-adventurer-5m--5m-pro`, `#flashforge-adventurer-5x`,
  `#creality-k1-series`, `#creality-k2-series`, `#snapmaker-u1`, plus the
  plain links). Content otherwise untouched.
- `docs/user/TROUBLESHOOTING.md`: 1 retarget
  (`#recovery-screen-is-blank-or-the-printer-is-off-the-network` ->
  `guide/install-u1.md#...`).
- `README.md` (root): anchors `#android-app-experimental` and
  `#remote-screen-setup-run-on-a-separate-device` survive in place; plain
  links unchanged.
- `docs/README.md`: tree diagram (L138) and index rows updated.
- `docs/user/CLAUDE.md`: index rows for the new files; the convention line
  "New install methods/platforms: Add to INSTALL.md" becomes: new platforms
  get their own `guide/install-*.md`; universal methods go in INSTALL.md.
- `docs/user/FAQ.md`, `USER_GUIDE.md`, `UPGRADING.md`, `guide/tips.md`,
  `TESTING_INSTALLATION.md`: anchors/links survive or are plain; verify.
- `scripts/kiauh/README.md`: absolute URL pinned to main - unaffected.

External deep links (Discord, forums) that hit a removed anchor land at the
top of INSTALL.md where the router table sits. Accepted.

## Gates

- `scripts/check_doc_refs.py` (refs + links + index) is the binding gate.
- `make check-doc-anchors` advisory.
- No installer gate parses these files (`check_installer_step_reachability.py`
  scans installer shell sources only).

## Content fidelity

- Commands move verbatim: they stay clean-install-proven.
- Main has 3 doc fixes 1.0 lacks; two fold in freely during the move (pure
  prose): the "GPU Rendering" -> "Display Backends: DRM vs Framebuffer"
  rewrite (no inbound links to that anchor), and the Snapmaker U1
  stock-firmware wording.
- The K2 binary-path fix (`/opt/helixscreen` -> `/mnt/UDISK/helixscreen`) must
  be VERIFIED against each branch's installer before folding: the installer
  code diverged ~2,400 lines (main reworked `scripts/lib/installer/`), and
  main's truth is not automatically 1.0's truth.
- 1.0's backtick formatting around paths is better than main's; carry it
  forward when folding main-side text.
- Anything genuinely NEW (not moved, not folded) gets flagged for hw-verify
  against the test fleet rather than asserted.

## Branch mechanics

- Branch `docs/install-restructure` off `release/1.0`, worktree at
  `.worktrees/install-restructure`. Docs-only: no test-suite changes, no
  build beyond what the commit hook does.
- Land on `release/1.0`, then port to `main` promptly (release fixes always
  port). The doc diff between branches is 13/13 lines pre-restructure, so the
  port should cherry-pick near-cleanly.
- Port pass on main: re-verify installer-behavior claims against main's
  reworked installer; absorb main-side install improvements the guides should
  reflect. Known candidate: the headless-install plan for Forge-X
  (`docs/devel/plans/2026-08-27-forgex-142-headless-install.md`, main only) -
  check whether it shipped and document it in `install-ad5m.md` on main.

## Out of scope

- TROUBLESHOOTING.md restructure (58 printer-name mentions stay as-is)
- supported-printers.md restructure (link retargets only)
- TESTING_INSTALLATION.md (internal matrix; plain links keep working)
- docs/devel/INSTALLER.md (dev-side how-it-works layer, already correct)
- Any website generator migration (plain GitHub-rendered markdown only)
