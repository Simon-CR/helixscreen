# Screensaver Gating and Fireworks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every board that builds screensavers runs one at the best quality it can sustain, decided by measuring the saver's own CPU cost on that board, and a new fireworks saver proves the design at 60 fps on the Pi 3B and on 16-bit boards.

**Architecture:** One always-compiled registry names every saver; a shared foundation (overlay, canvas, frame timer, pixel writer, saver base) carries all four savers, and the three existing savers move onto it with pixel-identical output pinned by fingerprint tests. `ScreensaverManager` samples process CPU time on the display manager's idle-check tick, subtracts an idle baseline, compares 5 s windows against a per-core budget with a pure decision function, steps the running saver down its ladder, and remembers the level per saver, app version and board fingerprint in config. Fireworks is a pure simulation that draws through `PixelWriter` into RGB565 or XRGB8888 frames and returns one dirty box per rocket or burst.

**Tech Stack:** C++17 (`-std=c++17`), LVGL 9.5 (public API plus the private headers the tree already includes), helix-xml, Catch2 v3 (`tests/catch_amalgamated.hpp`), spdlog, nlohmann json via `hv/json.hpp`, GNU make, bash and POSIX sh (shellcheck), Python 3 with pytest, a static armv7 C probe built in the `helixscreen/toolchain-cc1` Docker image.

**Spec:** docs/devel/plans/2026-09-14-screensaver-gating-fireworks-design.md

## Global Constraints

- The spec is authoritative: this plan implements exactly it, adds no feature and drops none.
- Compiler standard is `-std=c++17` (Makefile `CXXFLAGS`); no C++20 (`std::span`, designated initialisers) anywhere.
- LVGL private headers are included only as `lvgl/src/<dir>/<name>_private.h`, the way `src/ui/screensaver_manager.cpp` includes `lvgl/src/misc/lv_timer_private.h`; `LV_USE_PRIVATE_API` stays 0.
- Stable saver names: `toasters`, `starfield`, `pipes`, `fireworks`. Fireworks is type 4.
- Budget, verbatim: "By core count: 4 or more cores 50%, 3 cores 37%, 2 cores 25%, 1 core 10%. Halved while a print is running."
- Window, verbatim: "After a saver starts, the first second is ignored. Then each 5 s window, for as long as the saver runs, computes `(cpu delta - baseline * wall) / wall` as a share of one core."
- Baseline, verbatim: "While no saver runs, `ScreensaverManager` samples the clock from the display manager's idle check, which runs every main-loop iteration, at most every 250 ms. The baseline is the CPU rate over the last idle stretch of up to 10 s. Under 3 s of samples, the baseline is 0, which can only make the gate step down early, never late."
- Decision, verbatim: "Over budget: step one level down at the saver's next natural break. Over budget at the bottom level: mark the board too heavy. A session is one run of a saver from start to stop; within it the level never steps up, and the next run starts at the stored level."
- Level store, verbatim: config-only setting at `/display/screensaver_levels/<name>`: `{"level": n, "too_heavy": bool, "version": "...", "board": "..."}`; `version` is `helix_version_full()`; `board` is a pure function of the running display backend (drm, egl, fbdev, sdl), CPU core count, bogomips rounded to 100, resolution and colour depth; "A mismatch in either starts again from level 0. Malformed entries are ignored."
- Too heavy, verbatim: "The session falls back to a static black `SaverOverlay` with no frame timer, logs once why, and the store remembers it until the app version or board fingerprint changes."
- Ladders, verbatim: "Level 0 runs at 16 ms (a 60 Hz panel) by default, and `HELIX_SCREENSAVER_REFR_PERIOD_MS` overrides it for manual testing; the smoothness branch ships the pacing defaults (EGL vsync, main-loop floor) that keep 16 ms even. Starfield and pipes get two levels: 16 ms, then 33 ms. Toasters get three: 16 ms with every sprite, 33 ms with every sprite, 33 ms with 10 sprites, which replaces today's tier-based sprite cap. Motion is time-based, so a period change mid-run is seamless." `SAVER_FAST_PERIOD` is 16. When `HELIX_SCREENSAVER_REFR_PERIOD_MS` is set it replaces level 0's period, for the saver's frame timer and the held display refresh alike (`SaverBase::level_period_ms(0)` returns `RefreshPeriodHold::period()`); levels 1 and up keep their declared periods. While a saver runs, the display refresh follows the saver's current period, so the two stay equal at every level.
- Device runs set the pacing that keeps 16 ms even: every Pi 3B arm puts `HELIX_EGL_VSYNC=1 HELIX_LOOP_MIN_SLEEP_MS=1` in its drop-in and checks the running app carries them (Task 0 `arm_measure.sh`, `APP_ENV ok`), and the CC1 and AD5M runs set both in `helixscreen.env` (Task 11). They are set, not detected: the app logs neither when it equals the default, and setting a default changes nothing, so the 57 fps bar stays meaningful.
- Every device step needs Preston's approval for this session, holds a `helix-claim` on the device while it touches it, and runs no load unless Moonraker reports the printer is not printing or paused. Pi 3B steps that drive the device directly run between `perf_pi3b_take` and `perf_pi3b_release`.
- Dirty areas: "invalidation of a list of dirty areas merged to at most 32".
- Fireworks ladder, verbatim (applied when the next shell launches; all tunables in one table):

  | Level | Frame period | Sparks per burst | Trail | Bursts at once |
  |---|---|---|---|---|
  | 0 | 16 ms | 150 | 5 | 6 |
  | 1 | 33 ms | 150 | 5 | 6 |
  | 2 | 33 ms | 90 | 3 | 4 |
  | 3 | 50 ms | 50 | 0 | 3 |

- Fireworks pacing, verbatim: "One shell every 0.8 to 2.5 s; every 3 minutes or so a finale of 6 to 10 shells over about 4 s." Sparks come "from a pool allocated at start (about 30 KB at level 0); a full pool drops new sparks rather than allocating."
- Tiers: `STANDARD_RAM_THRESHOLD_MB` drops from 2048 to 768; 4 cores stays required; lands as its own commit after a Pi 3B UI check under the load gate.
- Default: fresh installs default `screensaver_type` to flying toasters wherever savers are built; existing configs are not migrated (no config migration, `CONFIG_VERSION` untouched).
- Environment switches `HELIX_SCREENSAVER_BUDGET_PCT` and `HELIX_SCREENSAVER_LEVEL`: "parsed strictly (malformed values warn and are ignored)", documented in `docs/devel/ENVIRONMENT_VARIABLES.md`.
- Load gate, verbatim: "`cyclictest --policy=other` plus `stress-ng --cpu 2`, every run max under 20 ms and p99 under 5 ms". BusyBox boards use the wake-up probe plus busy loops with the same pass rule.
- "No print runs during measurement, and Preston approves each device before it is stressed."
- "No new threads, so no TSAN run."
- Test access, verbatim: "One generic access class through the base (overlay, canvas, timer, seed, level). Per-saver access classes remain only for simulation state the base does not own (pipes grid, stars, sprites)."
- Fingerprints, verbatim: "They are recorded for the x86-64 Linux test build and skip elsewhere, since antialiased float math can move a pixel."
- Fireworks registration includes "a CJK font rebake for the new glyphs", and Task 10 writes "a durable `docs/devel/SCREENSAVERS.md` so this spec and its plan can be deleted when the work ships".
- Source files start with `// Copyright (C) 2025-2026 356C LLC` then `// SPDX-License-Identifier: GPL-3.0-or-later` in tests (as existing tests do) and `// SPDX-License-Identifier: GPL-3.0-or-later` alone in `src/` and `include/` (as the existing screensaver files do); shell and Python files carry `# SPDX-License-Identifier: GPL-3.0-or-later` in their first three lines.
- spdlog only; declarations live in `helix::ui` (existing global `Screensaver`, `ScreensaverManager`, `ScreensaverType` stay global); no RTTI (`static_cast` downcasts only); comments describe the code as it is now, never its history; doc citations are `path#symbol`, never line numbers.
- Every new `src/**/*.cpp` gets a line in `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`; every new screensaver source is covered by the Makefile `ENABLE_SCREENSAVER` filter.
- Every commit body carries exactly one `Mutation:` line naming the mutation made by hand and the test that went red (or, where a hunk cannot be killed on the test host, the surviving mutation and why).
- `git commit -m` takes simple double-quoted strings; paths are named explicitly, never `git add -A` or `git add .`; no em-dash characters in code, docs or commit text.
- Builds: check `pgrep -x -d' ' 'make|cc1plus'` first, use `-j"$(scripts/helix-claim jobs)"`, run in the foreground, and never pipe a build through `head`, `tail` or `grep` (redirect to a log file and filter the file). `make test` before `./build/bin/helix-tests`, run from the repo root.
- No device address, user name or password literal in the repo: scripts read `HELIX_PERF_HOST`, `HELIX_PERF_USER`, `HELIX_PERF_PASSWORD` from the environment; the values live in the device roster memory (`reference_ssh_access.md`).

---

## File Structure

| Path | Task | Responsibility |
|---|---|---|
| `scripts/screensaver-perf/README.md` | 0 | How to measure: settings, Pi flow, BusyBox flow, load-gate rule, probe build |
| `scripts/screensaver-perf/perf_env.sh` | 0 | Sourced settings and ssh helpers; saver name to dropdown index |
| `scripts/screensaver-perf/pi3b_service.sh` | 0 | On-device: stop, start, env drop-in, file collection, crash count |
| `scripts/screensaver-perf/pi3b_deploy.sh` | 0 | Deploy a build to the Pi and print what runs |
| `scripts/screensaver-perf/pi3b_measure.sh` | 0 | On-device: CPU window and strace of one workload |
| `scripts/screensaver-perf/pi3b_loadgate.sh` | 0 | On-device: cyclictest under stress-ng for one workload |
| `scripts/screensaver-perf/pi3b_pass.sh` | 0 | Host: one measurement pass over workloads |
| `scripts/screensaver-perf/pi3b_gate.sh` | 0 | Host: one load-gate pass over workloads |
| `scripts/screensaver-perf/arm_measure.sh` | 0 | Host: build, deploy, smoke, three passes, summary for one arm |
| `scripts/screensaver-perf/embedded_loadgate.sh` | 0 | On-device (BusyBox): probe under busy loops |
| `scripts/screensaver-perf/embedded_gate.sh` | 0 | Host: one load-gate run on a BusyBox board |
| `scripts/screensaver-perf/wakeup_probe.c` | 0 | 1 ms `clock_nanosleep` wake-up latency histogram |
| `scripts/screensaver-perf/flips.py` | 0 | Frame statistics from PAGE_FLIP and ATOMIC ioctls |
| `scripts/screensaver-perf/ctparse.py` | 0 | Histogram to RESULT line |
| `scripts/screensaver-perf/summarize.py` | 0 | Tables and load-gate verdict |
| `tests/python/test_screensaver_perf_parsers.py` | 0 | Parser and verdict tests |
| `scripts/CLAUDE.md` | 0 | Index row for the harness |
| `include/screensaver_registry.h` | 1, 7, 10 | `ScreensaverType`, registry rows, count, clamp, name lookup, default type |
| `tests/unit/test_screensaver_registry.cpp` | 1, 7, 10 | Registry, clamp, names, XML option list |
| `tests/unit/test_screensaver_fingerprint.cpp` | 2, 3, 4 | Pixel fingerprints of pipes and starfield |
| `tests/fixtures/screensaver/fingerprints.txt` | 2 | Recorded fingerprints |
| `include/screensaver_frame.h` | 3, 4, 9 | `DirtyRect`, merge, `SAVER_FAST_PERIOD`, `PixelFormat`, `Rgb`, `FrameTarget` (pure) |
| `src/ui/screensaver_frame.cpp` | 3 | `merge_dirty_areas` |
| `include/screensaver_overlay.h`, `src/ui/screensaver_overlay.cpp` | 3 | `SaverOverlay` |
| `include/screensaver_canvas.h`, `src/ui/screensaver_canvas.cpp` | 3, 4, 9 | `SaverCanvas` |
| `include/screensaver_frame_timer.h`, `src/ui/screensaver_frame_timer.cpp` | 3 | `SaverFrameTimer` |
| `include/screensaver_base.h`, `src/ui/screensaver_base.cpp` | 3 | `SaverBase`, two-level ladder helpers |
| `include/refresh_period_hold.h`, `src/application/display_manager.cpp#RefreshPeriodHold` | 3 | The display refresh during a saver: the saver's current period (`follow`); the configured period until a saver gives one |
| `tests/unit/application/test_refresh_period_hold.cpp` | 3 | The hold follows a saver's period, from the configured one or from nothing |
| `tests/unit/test_screensaver_parts.cpp` | 3 | Merge, overlay, canvas, frame timer, base |
| `include/screensaver_pixel_writer.h` | 4, 9 | `PixelWriter` (header only, pure) |
| `tests/unit/test_screensaver_pixel_writer.cpp` | 4, 9 | Exact bits, dither, blend, line |
| `include/env_whole_number.h` | 6 | Strict whole-number environment parsing shared by refresh timing and the gate |
| `include/screensaver_cpu_clock.h`, `src/ui/screensaver_cpu_clock.cpp` | 6 | `CpuSample`, `read_process_cpu_clock` |
| `include/screensaver_gate.h`, `src/ui/screensaver_gate.cpp` | 6 | Budget, decision, `IdleBaseline`, `SaverGateSession`, env overrides |
| `include/screensaver_level_store.h`, `src/ui/screensaver_level_store.cpp` | 6 | Board fingerprint, entry parse, start entry, load, save |
| `tests/unit/test_screensaver_gate.cpp` | 6 | Pure gate and store tests |
| `tests/unit/test_screensaver_gate_manager.cpp` | 6 | Manager integration, fallback, env switches |
| `tests/unit/application/test_display_screensaver_gate.cpp` | 6 | DisplayManager wiring: printing state and display backend reach the gate |
| `include/screensaver_fireworks_sim.h`, `src/ui/screensaver_fireworks_sim.cpp` | 10 | `FireworksSim`, `FIREWORKS_LEVELS`, `FireworksPacing` |
| `include/screensaver_fireworks.h`, `src/ui/screensaver_fireworks.cpp` | 10 | `FireworksScreensaver` |
| `tests/unit/test_screensaver_fireworks.cpp` | 10 | Sim and saver tests |
| `docs/devel/SCREENSAVERS.md` | 10 | Durable developer doc (registry, foundation, gate, store, adding a saver) |
| Modified: `include/screensaver.h`, `src/ui/screensaver_manager.cpp`, `src/ui/screensaver_pipes.cpp`, `include/screensaver_pipes.h`, `src/ui/screensaver_starfield.cpp`, `include/screensaver_starfield.h`, `src/ui/screensaver_starfield_sim.cpp`, `include/screensaver_starfield_sim.h`, `src/ui/ui_screensaver.cpp`, `include/ui_screensaver.h` | 1-6, 10 | Savers on the foundation, manager gate |
| Modified: `src/system/display_settings_manager.cpp`, `src/application/display_manager.cpp`, `include/display_manager.h`, `include/display_backend.h`, `include/refresh_timing_env.h` | 1, 6, 7 | Clamp, idle tick, host, backend key, shared env parsing |
| Modified: `include/platform_capabilities.h`, `tests/unit/test_platform_capabilities.cpp`, `docs/devel/INPUT_SHAPER.md` | 8 | STANDARD floor |
| Modified: `tests/test_helpers/screensaver_test_access.h`, `tests/test_helpers/screensaver_manager_test_access.h`, `tests/unit/test_screensaver*.cpp` | 1-6, 10 | Generic test access and updated call sites |
| Modified: `Makefile`, `mk/cross.mk`, `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` | 3, 6, 10, 11 | Source filter, 16 bpp filter, Docker forwarding, ESP32 exclusions |
| Modified: `ui_xml/settings_display_sound_overlay.xml`, `translations/*.yml`, `ui_xml/translations/*.xml` | 10 | Dropdown option and translations |
| Modified: `docs/devel/ENVIRONMENT_VARIABLES.md`, `docs/user/CONFIGURATION.md`, `docs/user/guide/settings/display-sound.md`, `config/settings.json.template`, `docs/devel/CLAUDE.md`, `docs/README.md`, `docs/CLAUDE.md` | 1, 6, 7, 10 | Docs |
| Deleted when the work ships: `docs/devel/plans/2026-09-14-screensaver-gating-fireworks-design.md`, this plan | 11 | Scaffolding removal |

## Conventions every task uses

Set these once per shell (the scratch directory is your session scratchpad, outside the repo):

```bash
cd <worktree root>                      # the worktree Step 0 creates; commands run from its root
export SS_SCRATCH=<your scratchpad>/screensaver-gating
mkdir -p "$SS_SCRATCH"
JOBS=$(scripts/helix-claim jobs)
```

- Compile check for touched sources: `scripts/syntax_check.py <files>`.
- Build the tests: `pgrep -x -d' ' 'make|cc1plus'; make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`. On a non-zero exit read the log with `grep -n -E 'error|Error' "$SS_SCRATCH/test-build.log"`.
- Run a tag: `./build/bin/helix-tests "[tag]"`.
- Build the app: `make -j"$JOBS" > "$SS_SCRATCH/app-build.log" 2>&1; echo "exit $?"`.
- Format before committing: `.venv/bin/clang-format -i <changed C++ files>` (the pre-commit hook checks it too).
- Commit: stage the named paths with `git add -- <paths>`, then `git commit -m "<subject>" -m "<body paragraph>" -m "Mutation: <mutation and the test that went red>"`. The pre-commit hook runs `scripts/quality-checks.sh`, which builds; wait for it in the foreground.
- A mutation step means: make the named edit by hand, rebuild the tests, run the tag, confirm the named test fails, then undo the edit exactly and rebuild.

### Step 0: A clean worktree on the landed smoothness base

Every task runs in one new worktree cut from `origin/main` once the smoothness work is on it, holding nothing HEAD does not describe: no uncommitted or untracked file, and no lvgl change beyond the patch set `mk/patches.mk` applies at HEAD. A stray experiment in either (an `LV_INV_BUF_SIZE` edit, a draft doc) would otherwise be committed with Task 0 or measured as if it were this branch.

From the main tree, with `SS_SCRATCH` and `JOBS` set as in the block above:

```bash
git fetch origin
git merge-base --is-ancestor feature/screensaver-smoothness origin/main; echo "smoothness on main: exit $?"
scripts/setup-worktree.sh --base origin/main --no-build feature/screensaver-gating
cd .worktrees/screensaver-gating
git log -1 --format='%h %s'
ls docs/devel/plans/2026-09-14-screensaver-gating-fireworks-design.md docs/devel/plans/2026-09-14-screensaver-gating-fireworks.md
```

Expected: `smoothness on main: exit 0`; HEAD is the tip of `origin/main`; both plan documents are listed (Task 11 deletes them in the shipping change, so they must be committed). If the smoothness branch landed as a squash, `--is-ancestor` exits `1` although the work is on main: ask Preston for the landing commit and run `git merge-base --is-ancestor <that commit> origin/main` instead. Any other failure: stop and tell Preston.

Then prove the tree is what HEAD describes:

```bash
git status --porcelain | wc -l
make reapply-patches > "$SS_SCRATCH/reapply-patches.log" 2>&1; echo "reapply exit $?"
python3 scripts/check_patch_drift.py; echo "drift exit $?"
git -C lib/lvgl status --porcelain --untracked-files=no | awk '{print $2}' | sort > "$SS_SCRATCH/lvgl-modified.txt"
make -pn 2>/dev/null | sed -n 's/^LVGL_PATCHED_FILES := //p' | tr ' ' '\n' | sed '/^$/d' | sort -u > "$SS_SCRATCH/lvgl-patched.txt"
wc -l < "$SS_SCRATCH/lvgl-patched.txt"
comm -23 "$SS_SCRATCH/lvgl-modified.txt" "$SS_SCRATCH/lvgl-patched.txt"
grep -n 'define LV_INV_BUF_SIZE' lib/lvgl/src/display/lv_display_private.h
git status --porcelain | wc -l
```

Expected:
- `0` from both `git status` counts. `.gitmodules` marks `lib/lvgl` `ignore = dirty`, so applied patches do not show; anything listed is a file HEAD does not have.
- `reapply exit 0`: `make reapply-patches` puts lvgl's patched files back to upstream and applies exactly the set `mk/patches.mk` names at HEAD, so no earlier experiment survives in them.
- `drift exit 0`: every applied patch matches its file in `patches/`.
- A non-zero patched-file count, and `comm` printing nothing: no lvgl file is changed outside that patch set.
- A line defining `LV_INV_BUF_SIZE` as `32`, the invalidation-area limit `SAVER_MAX_DIRTY_AREAS` (Task 3) matches.

Anything else: stop and show Preston the output before building. Then build once, `make -j"$JOBS" > "$SS_SCRATCH/app-build.log" 2>&1; echo "exit $?"` (expect `exit 0`), and use this directory as `<worktree root>` from here on.

---

### Task 0: Measurement harness in the repo

Ports the Pi 3B measurement scripts out of a session scratchpad into `scripts/screensaver-perf/`, with every device setting from the environment, adds the BusyBox wake-up probe and its gate, and pins the parsers with pytest.

**Files:**
- Create: `scripts/screensaver-perf/perf_env.sh`, `scripts/screensaver-perf/pi3b_service.sh`, `scripts/screensaver-perf/pi3b_deploy.sh`, `scripts/screensaver-perf/pi3b_measure.sh`, `scripts/screensaver-perf/pi3b_loadgate.sh`, `scripts/screensaver-perf/pi3b_pass.sh`, `scripts/screensaver-perf/pi3b_gate.sh`, `scripts/screensaver-perf/arm_measure.sh`, `scripts/screensaver-perf/embedded_loadgate.sh`, `scripts/screensaver-perf/embedded_gate.sh`, `scripts/screensaver-perf/wakeup_probe.c`, `scripts/screensaver-perf/flips.py`, `scripts/screensaver-perf/ctparse.py`, `scripts/screensaver-perf/summarize.py`, `scripts/screensaver-perf/README.md`
- Modify: `scripts/CLAUDE.md` (new subsection after the `### Release & Packaging` table)
- Test: `tests/python/test_screensaver_perf_parsers.py`

**Interfaces:**
- Consumes: nothing.
- Produces (used by Tasks 3, 4, 5, 6, 8, 10, 11):
  - Environment: `HELIX_PERF_HOST`, `HELIX_PERF_SCRATCH` (required), `HELIX_PERF_USER` (default `pi`), `HELIX_PERF_PASSWORD` (unset = key auth), `HELIX_PERF_INSTALL` (default `/home/pi/helixscreen`), `HELIX_PERF_CTL_SOCK` (default `/run/helixscreen/control.sock`), `HELIX_PERF_BUILD_TREE` (tree `arm_measure.sh` builds), `HELIX_PERF_PROBE` (probe binary for `embedded_gate.sh`).
  - `ARM=<label> BUILD=1|0 BIN_FROM=<arm> ENV_EXTRA="K=V ..." WORKLOADS="..." SAVERS="..." BASELINES="..." scripts/screensaver-perf/arm_measure.sh` writes `$HELIX_PERF_SCRATCH/results/<ARM>.txt`, `$HELIX_PERF_SCRATCH/<ARM>_summary.txt`, `$HELIX_PERF_SCRATCH/<ARM>.done`.
  - `SAVERS="off ui <saver>..." DUR=60 scripts/screensaver-perf/pi3b_gate.sh <arm> <run>`; workload `ui` cycles `home controls filament settings print-select`.
  - `DUR=60 HELIX_PERF_PROBE=<bin> scripts/screensaver-perf/embedded_gate.sh <arm> <run> <label>`.
  - `perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=stop|start|dropin|collect|crashes|journal|display_value|unset_display|app_env|print_state` with `ENV_EXTRA`, `FILE`, `SINCE`, `PATTERN`, `KEY` as the action needs.
  - In `perf_env.sh`: `PERF_PACING_ENV="HELIX_EGL_VSYNC=1 HELIX_LOOP_MIN_SLEEP_MS=1"`, set on every device run; `perf_thermal`; `perf_print_idle <state>`; `perf_pi3b_take <reason>` and `perf_pi3b_release` around direct Pi steps; `perf_app_log` (the log file the running app holds open). Every arm logs `PRINT_STATE`, `APP_ENV ok` and `THERMAL` lines, and removes its drop-in when it finishes.
  - `pi3b_measure.sh` takes `SETTLE_S=<seconds>`: that long with no ctl traffic before the Test Screensaver press.
  - `perf_saver_type <name>` in `perf_env.sh`: `idle|off|ui` 0, `toasters` 1, `starfield` 2, `pipes` 3 (Task 10 adds `fireworks` 4).
  - `python3 scripts/screensaver-perf/summarize.py <results files>` prints `PASS` or `FAIL` per arm and workload under the rule max < 20000 us, p99 < 5000 us, no overflow, samples > 0.

- [ ] **Step 1: Write the failing parser tests**

Create `tests/python/test_screensaver_perf_parsers.py`:

```python
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the screensaver measurement parsers in scripts/screensaver-perf/."""

import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "screensaver-perf"))

import ctparse  # noqa: E402
import flips  # noqa: E402
import summarize  # noqa: E402


def fields_of(line):
    return dict(part.split("=", 1) for part in line.split()[1:])


def test_flips_count_page_flip_and_atomic_ioctls_but_not_resumptions():
    log = [
        "812  1726300000.000000 ioctl(10, DRM_IOCTL_MODE_PAGE_FLIP, 0x7ffd) = 0",
        "812  1726300000.016000 ioctl(10, DRM_IOCTL_MODE_ATOMIC, 0x7ffd <unfinished ...>",
        "812  1726300000.017000 <... ioctl resumed>) = 0",
        "812  1726300000.020000 ioctl(10, DRM_IOCTL_MODE_GETRESOURCES, 0x7ffd) = 0",
        "812  1726300000.033000 ioctl(10, DRM_IOCTL_MODE_ATOMIC, 0x7ffd) = 0",
    ]
    assert flips.flip_times(log) == [1726300000.0, 1726300000.016, 1726300000.033]


def test_flips_line_reports_rate_and_largest_gap():
    line = flips.flips_line([0.0, 0.016, 0.033, 0.050])
    fields = fields_of(line)
    assert line.startswith("FLIPS ")
    assert abs(float(fields["fps"]) - 60.0) < 0.1
    assert fields["max"] == "17.0"


def test_flips_line_needs_three_flips():
    assert flips.flips_line([1.0, 2.0]) == "FLIPS n=2 insufficient"


HISTOGRAM = [
    "# Histogram",
    "000100 000090",
    "000200 000009",
    "003000 000001",
    "# Total: 000000100",
    "# Histogram Overflows: 00000",
    "# Max Latencies: 03000",
]


def test_ctparse_reports_percentiles_max_and_cpu():
    fields = fields_of(ctparse.result_line(HISTOGRAM, "armA", "off", "GATE_RAW ct_exit=0 helix_cpu=4.2"))
    assert fields["samples"] == "100"
    assert fields["p50_us"] == "100"
    assert fields["p99_us"] == "200"
    assert fields["p999_us"] == "3000"
    assert fields["max_us"] == "3000"
    assert fields["helix_cpu"] == "4.2%"
    assert fields["ct_exit"] == "0"


def test_ctparse_counts_overflows_in_the_total():
    histogram = ["000050 000010", "# Histogram Overflows: 00002", "# Max Latencies: 25000"]
    fields = fields_of(ctparse.result_line(histogram, "armA", "off", "GATE_RAW ct_exit=0"))
    assert fields["samples"] == "12"
    assert fields["overflow_gt20ms"] == "2"
    assert fields["p999_us"] == "20000"
    assert fields["max_us"] == "25000"


def test_ctparse_empty_histogram_reports_no_samples():
    fields = fields_of(ctparse.result_line([], "armA", "off", "GATE_RAW missing"))
    assert fields["samples"] == "0"
    assert fields["p99_us"] == "-1"
    assert fields["max_us"] == "-1"


def runs(**values):
    table = defaultdict(list)
    for key, series in values.items():
        table[key] = list(series)
    return table


def test_gate_passes_when_every_run_is_under_both_limits():
    assert summarize.gate_passes(
        runs(samples=[60000, 60000], p99_us=[4999, 3000], max_us=[19999, 12000], overflow_gt20ms=[0, 0]))


def test_gate_fails_on_one_wakeup_of_20_ms():
    assert not summarize.gate_passes(
        runs(samples=[60000, 60000], p99_us=[1000, 1000], max_us=[20000, 900], overflow_gt20ms=[0, 0]))


def test_gate_fails_on_one_p99_of_5_ms():
    assert not summarize.gate_passes(
        runs(samples=[60000, 60000], p99_us=[5000, 100], max_us=[9000, 900], overflow_gt20ms=[0, 0]))


def test_gate_fails_on_an_overflow_a_run_without_samples_or_no_runs():
    assert not summarize.gate_passes(
        runs(samples=[60000, 60000], p99_us=[100, 100], max_us=[900, 900], overflow_gt20ms=[0, 1]))
    assert not summarize.gate_passes(
        runs(samples=[60000, 0], p99_us=[100, 100], max_us=[900, 900], overflow_gt20ms=[0, 0]))
    assert not summarize.gate_passes(runs())
```

- [ ] **Step 2: Run it and watch it fail**

Run: `.venv/bin/pytest tests/python/test_screensaver_perf_parsers.py -q`
Expected: collection error, `ModuleNotFoundError: No module named 'ctparse'`.

- [ ] **Step 3: Write the three parsers**

Create `scripts/screensaver-perf/flips.py`:

```python
# SPDX-License-Identifier: GPL-3.0-or-later
"""Presented-frame statistics from an strace ioctl log.

usage: flips.py <strace log>

The dumb-DRM binary presents with DRM_IOCTL_MODE_PAGE_FLIP and the EGL binary with
DRM_IOCTL_MODE_ATOMIC, so both count as a presented frame. Prints one FLIPS line.
"""

import re
import statistics
import sys

FLIP_IOCTLS = ("DRM_IOCTL_MODE_PAGE_FLIP", "DRM_IOCTL_MODE_ATOMIC")
TIMESTAMP = re.compile(r"(\d{10}\.\d+) ioctl")


def flip_times(lines):
    """Timestamps, in seconds, of every presenting ioctl call. A resumed call is the same flip."""
    times = []
    for line in lines:
        if "resumed" in line or not any(name in line for name in FLIP_IOCTLS):
            continue
        match = TIMESTAMP.search(line)
        if match:
            times.append(float(match.group(1)))
    return times


def flips_line(times):
    if len(times) < 3:
        return f"FLIPS n={len(times)} insufficient"
    gaps = sorted((b - a) * 1000 for a, b in zip(times, times[1:]))
    count = len(gaps)

    def quantile(p):
        return gaps[min(count - 1, int(count * p))]

    span = times[-1] - times[0]
    return (f"FLIPS fps={(len(times) - 1) / span:.1f} p10={quantile(.1):.1f} p50={quantile(.5):.1f} "
            f"p90={quantile(.9):.1f} p99={quantile(.99):.1f} max={gaps[-1]:.1f} "
            f"stdev={statistics.pstdev(gaps):.1f}")


def main(argv):
    with open(argv[1]) as log:
        print(flips_line(flip_times(log)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

Create `scripts/screensaver-perf/ctparse.py`:

```python
# SPDX-License-Identifier: GPL-3.0-or-later
"""Latency percentiles from a cyclictest -h histogram (one thread).

usage: ctparse.py <histogram file> <arm> <label> <GATE_RAW line>

wakeup_probe writes the same histogram format, so its output parses here too. Prints one
RESULT line in the format summarize.py reads.
"""

import re
import sys

BIN = re.compile(r"^(\d+)\s+(\d+)\s*$")
TOTAL = re.compile(r"^#\s*Total:\s*(\d+)")
OVERFLOWS = re.compile(r"^#\s*Histogram Overflows:\s*(\d+)")
MAX_LATENCY = re.compile(r"^#\s*Max Latencies:\s*(\d+)")
HISTOGRAM_US = 20000


def parse_histogram(lines):
    counts = {}
    overflow = 0
    total_reported = None
    max_us = None
    for line in lines:
        match = BIN.match(line)
        if match:
            us = int(match.group(1))
            counts[us] = counts.get(us, 0) + int(match.group(2))
            continue
        match = TOTAL.match(line)
        if match:
            total_reported = int(match.group(1))
            continue
        match = OVERFLOWS.match(line)
        if match:
            overflow = int(match.group(1))
            continue
        match = MAX_LATENCY.match(line)
        if match:
            max_us = int(match.group(1))
    return counts, overflow, total_reported, max_us


def percentile(counts, overflow, fraction):
    """Smallest bin holding `fraction` of the samples; HISTOGRAM_US when it falls in the overflow."""
    total = sum(counts.values()) + overflow
    if total == 0:
        return -1
    need = total * fraction
    running = 0
    for us in sorted(counts):
        running += counts[us]
        if running >= need:
            return us
    return HISTOGRAM_US


def result_line(lines, arm, label, raw):
    fields = dict(re.findall(r"(\w+)=(\S+)", raw))
    counts, overflow, total_reported, max_us = parse_histogram(lines)
    total = sum(counts.values()) + overflow
    return (f"RESULT arm={arm} saver={label} samples={total} total_reported={total_reported} "
            f"p50_us={percentile(counts, overflow, .5)} p99_us={percentile(counts, overflow, .99)} "
            f"p999_us={percentile(counts, overflow, .999)} max_us={max_us if max_us is not None else -1} "
            f"overflow_gt20ms={overflow} helix_cpu={fields.get('helix_cpu', '-1')}% "
            f"saver_started={fields.get('saver_started', '-1')} "
            f"saver_stopped_midrun={fields.get('saver_stopped_midrun', '-1')} "
            f"ct_exit={fields.get('ct_exit', '-1')} ct_err={fields.get('ct_err', 'none')}")


def main(argv):
    histogram, arm, label, raw = argv[1:5]
    with open(histogram) as lines:
        print(result_line(lines, arm, label, raw))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

Create `scripts/screensaver-perf/summarize.py`:

```python
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-arm, per-workload mean [min..max] tables and the load-gate verdict.

usage: summarize.py <results/arm.txt>...

Reads the CPU, FLIPS, STOPPED_MIDRUN, STARTED and gate RESULT lines that pi3b_pass.sh,
pi3b_gate.sh and embedded_gate.sh append.
"""

import re
import statistics
import sys
from collections import defaultdict

# The load gate: in every run, no wake-up of 20 ms or more and a p99 under 5 ms.
MAX_WAKEUP_US = 20000
MAX_P99_US = 5000

LINE = re.compile(r"^(\S+) run=(\d+) (\S+) (CPU|FLIPS|STOPPED_MIDRUN|STARTED|RESULT)\s*(.*)$")
NUMBER = re.compile(r"(\w+)=([-\d.]+%?)")
GATE_KEYS = ("samples", "p50_us", "p99_us", "p999_us", "max_us", "overflow_gt20ms", "helix_cpu")


def numbers(text):
    return {key: float(value.rstrip("%")) for key, value in NUMBER.findall(text)}


def gate_passes(runs):
    """True when every run of one arm and workload passes the load gate."""
    return (len(runs["p99_us"]) > 0
            and all(samples > 0 for samples in runs["samples"])
            and all(wakeup < MAX_WAKEUP_US for wakeup in runs["max_us"])
            and all(overflow == 0 for overflow in runs["overflow_gt20ms"])
            and all(p99 < MAX_P99_US for p99 in runs["p99_us"]))


def load(paths):
    cpu = defaultdict(lambda: defaultdict(list))
    flips = defaultdict(lambda: defaultdict(list))
    gate = defaultdict(lambda: defaultdict(list))
    flags = []
    for path in paths:
        with open(path) as results:
            for raw in results:
                match = LINE.match(raw.strip())
                if not match:
                    continue
                arm, run, workload, kind, rest = match.groups()
                if kind == "CPU":
                    for key, value in numbers(rest).items():
                        cpu[(arm, workload)][key].append(value)
                elif kind == "FLIPS":
                    if "insufficient" in rest:
                        flags.append(f"{arm} run={run} {workload}: too few flips")
                        continue
                    for key, value in numbers(rest).items():
                        flips[(arm, workload)][key].append(value)
                elif kind == "STOPPED_MIDRUN" and rest.strip() != "0":
                    flags.append(f"{arm} run={run} {workload}: saver stopped mid-run ({rest.strip()})")
                elif kind == "STARTED" and rest.strip() == "0":
                    flags.append(f"{arm} run={run} {workload}: saver never started")
                elif kind == "RESULT":
                    label = re.search(r"saver=(\S+)", rest).group(1)
                    values = numbers(rest)
                    for key in GATE_KEYS:
                        if key in values:
                            gate[(arm, label)][key].append(values[key])
                    if values.get("saver_stopped_midrun", 0) > 0:
                        flags.append(f"{arm} run={run} gate {label}: saver stopped mid-run")
                    if values.get("samples", 0) <= 0:
                        flags.append(f"{arm} run={run} gate {label}: no latency samples "
                                     f"(ct_exit={values.get('ct_exit', '?')})")
                    elif values.get("ct_exit", 0) != 0:
                        flags.append(f"{arm} run={run} gate {label}: probe exit {values.get('ct_exit')}")
                    if label not in ("off", "ui") and values.get("saver_started", 1) == 0:
                        flags.append(f"{arm} run={run} gate {label}: saver never started")
    return cpu, flips, gate, flags


def fmt(values, digits=1):
    if not values:
        return "-"
    return f"{statistics.mean(values):.{digits}f} [{min(values):.{digits}f}..{max(values):.{digits}f}] n={len(values)}"


def main(argv):
    cpu, flips, gate, flags = load(argv[1:])
    print("## CPU (% of one core) and presented frames")
    print(f"{'arm':<18} {'workload':<10} {'cpu total':<26} {'cpu main':<26} {'fps':<26} "
          f"{'p50 ms':<8} {'p90 ms':<8} {'stdev':<6}")
    for key in sorted(set(cpu) | set(flips)):
        arm, workload = key
        c, f = cpu.get(key, {}), flips.get(key, {})
        p50 = f"{statistics.mean(f['p50']):.1f}" if f.get("p50") else "-"
        p90 = f"{statistics.mean(f['p90']):.1f}" if f.get("p90") else "-"
        stdev = f"{statistics.mean(f['stdev']):.1f}" if f.get("stdev") else "-"
        print(f"{arm:<18} {workload:<10} {fmt(c.get('total', [])):<26} {fmt(c.get('main', [])):<26} "
              f"{fmt(f.get('fps', [])):<26} {p50:<8} {p90:<8} {stdev:<6}")
    if gate:
        print("\n## Load gate, microseconds")
        print("Verdict: PASS when every run has no wake-up of 20 ms or more and p99 under 5 ms.")
        print(f"{'arm':<18} {'workload':<10} {'p99':<24} {'p99.9':<24} {'max':<26} {'>20ms':<6} "
              f"{'helix cpu':<22} verdict")
        for (arm, label), runs in sorted(gate.items()):
            print(f"{arm:<18} {label:<10} {fmt(runs['p99_us'], 0):<24} {fmt(runs['p999_us'], 0):<24} "
                  f"{fmt(runs['max_us'], 0):<26} {int(sum(runs['overflow_gt20ms'])):<6} "
                  f"{fmt(runs['helix_cpu']):<22} {'PASS' if gate_passes(runs) else 'FAIL'}")
    if flags:
        print("\n## Contaminated runs")
        for flag in flags:
            print(" -", flag)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

- [ ] **Step 4: Run the parser tests and watch them pass**

Run: `.venv/bin/pytest tests/python/test_screensaver_perf_parsers.py -q`
Expected: `10 passed`.

- [ ] **Step 5: Write the wake-up probe and prove it against the parser**

Create `scripts/screensaver-perf/wakeup_probe.c`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wake-up latency probe for boards without cyclictest. Sleeps to 1 ms deadlines on
// CLOCK_MONOTONIC with clock_nanosleep under the default scheduler, records how late each
// wake-up is, and prints the histogram in the format cyclictest -h writes, so ctparse.py
// reads it unchanged.
//
// usage: wakeup_probe <seconds>
// Build commands are in README.md beside this file.

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define INTERVAL_NS 1000000L
#define HISTOGRAM_US 20000

static int64_t to_ns(const struct timespec* ts) {
    return (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

static uint64_t histogram[HISTOGRAM_US];

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <seconds>\n", argv[0]);
        return 2;
    }
    char* end = NULL;
    long seconds = strtol(argv[1], &end, 10);
    if (argv[1][0] == '\0' || *end != '\0' || seconds <= 0 || seconds > 3600) {
        fprintf(stderr, "seconds must be a whole number from 1 to 3600\n");
        return 2;
    }

    uint64_t overflow = 0;
    uint64_t samples = 0;
    int64_t max_us = 0;
    struct timespec deadline;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    const int64_t stop_ns = to_ns(&deadline) + (int64_t)seconds * 1000000000LL;

    for (;;) {
        deadline.tv_nsec += INTERVAL_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }
        int rc;
        do {
            rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (rc == EINTR);
        if (rc != 0) {
            fprintf(stderr, "clock_nanosleep failed: %d\n", rc);
            return 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t late_ns = to_ns(&now) - to_ns(&deadline);
        int64_t late_us = late_ns / 1000;
        if (late_us < 0) {
            late_us = 0;
        }
        if (late_us > max_us) {
            max_us = late_us;
        }
        if (late_us >= HISTOGRAM_US) {
            overflow++;
        } else {
            histogram[late_us]++;
        }
        samples++;
        if (to_ns(&now) >= stop_ns) {
            break;
        }
        // A wake-up more than an interval late starts the schedule again from now instead of
        // owing the missed deadlines as a burst of zero-length sleeps.
        if (late_ns > INTERVAL_NS) {
            deadline = now;
        }
    }

    for (int us = 0; us < HISTOGRAM_US; us++) {
        if (histogram[us] != 0) {
            printf("%06d %06llu\n", us, (unsigned long long)histogram[us]);
        }
    }
    printf("# Total: %09llu\n", (unsigned long long)samples);
    printf("# Histogram Overflows: %05llu\n", (unsigned long long)overflow);
    printf("# Max Latencies: %05lld\n", (long long)max_us);
    return 0;
}
```

Run on the host:

```bash
cc -std=c99 -O2 -Wall -Wextra -o "$SS_SCRATCH/wakeup_probe" scripts/screensaver-perf/wakeup_probe.c -lrt
"$SS_SCRATCH/wakeup_probe" 2 > "$SS_SCRATCH/probe-host.txt"; echo "exit $?"
python3 scripts/screensaver-perf/ctparse.py "$SS_SCRATCH/probe-host.txt" host off "GATE_RAW ct_exit=0 helix_cpu=0"
"$SS_SCRATCH/wakeup_probe" 0; echo "exit $?"
```

Expected: the compile prints no warnings; `exit 0`; a `RESULT arm=host saver=off samples=` line with samples between 1900 and 2000 and a non-negative `p99_us`; the last command prints `seconds must be a whole number from 1 to 3600` and `exit 2`.

- [ ] **Step 6: Write the shared settings and the Pi device-side scripts**

Create `scripts/screensaver-perf/perf_env.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Settings shared by the screensaver measurement scripts. Source it from bash; it defines
# the ssh helpers and stops with a message when a required setting is missing.
#
#   HELIX_PERF_HOST      device address (required)
#   HELIX_PERF_SCRATCH   directory for results, logs and copied binaries (required, outside the repo)
#   HELIX_PERF_USER      ssh user (default pi)
#   HELIX_PERF_PASSWORD  ssh and sudo password; unset means key auth and passwordless sudo
#   HELIX_PERF_INSTALL   install root on the device (default /home/pi/helixscreen)
#   HELIX_PERF_CTL_SOCK  helix-screen control socket on the device (default /run/helixscreen/control.sock)

: "${HELIX_PERF_HOST:?set HELIX_PERF_HOST to the device address}"
: "${HELIX_PERF_SCRATCH:?set HELIX_PERF_SCRATCH to a directory outside the repo}"
HELIX_PERF_USER=${HELIX_PERF_USER:-pi}
HELIX_PERF_INSTALL=${HELIX_PERF_INSTALL:-/home/pi/helixscreen}
HELIX_PERF_CTL_SOCK=${HELIX_PERF_CTL_SOCK:-/run/helixscreen/control.sock}

PERF_TARGET="$HELIX_PERF_USER@$HELIX_PERF_HOST"
# shellcheck disable=SC2034 # read by every script that sources this file
PERF_HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC2034 # read by arm_measure.sh
PERF_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
mkdir -p "$HELIX_PERF_SCRATCH/results"

# Runs an ssh-based tool against the device. A password reaches sshpass through its
# environment, so it never appears in a process list. DISPLAY and SSH_ASKPASS are cleared
# so no desktop password dialog opens.
perf_tool() {
    local tool=$1
    shift
    if [ -n "${HELIX_PERF_PASSWORD:-}" ]; then
        DISPLAY='' SSH_ASKPASS='' SSHPASS="$HELIX_PERF_PASSWORD" sshpass -e "$tool" \
            -o PubkeyAuthentication=no -o PreferredAuthentications=password \
            -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$@"
    else
        DISPLAY='' SSH_ASKPASS='' "$tool" -o BatchMode=yes -o StrictHostKeyChecking=no \
            -o ConnectTimeout=10 "$@"
    fi
}

perf_ssh() {
    perf_tool ssh "$PERF_TARGET" "$@"
}

perf_scp() {
    perf_tool scp -q "$@"
}

# Copies the contents of a local directory into a directory on the device.
perf_rsync_dir() {
    local ssh_cmd="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10"
    if [ -n "${HELIX_PERF_PASSWORD:-}" ]; then
        DISPLAY='' SSH_ASKPASS='' SSHPASS="$HELIX_PERF_PASSWORD" sshpass -e rsync -az --checksum \
            -e "$ssh_cmd -o PubkeyAuthentication=no -o PreferredAuthentications=password" \
            "$1/" "$PERF_TARGET:$2/"
    else
        rsync -az --checksum -e "$ssh_cmd -o BatchMode=yes" "$1/" "$PERF_TARGET:$2/"
    fi
}

# Runs a device-side script under bash. The password, install root, control socket and
# every KEY=VALUE argument become the script's first lines, so none travels on a command line.
perf_run_remote() {
    local script=$1
    shift
    {
        printf 'PW=%q\nINSTALL=%q\nCTL_SOCK=%q\n' "${HELIX_PERF_PASSWORD:-}" "$HELIX_PERF_INSTALL" \
            "$HELIX_PERF_CTL_SOCK"
        local kv
        for kv in "$@"; do
            printf '%s=%q\n' "${kv%%=*}" "${kv#*=}"
        done
        cat "$script"
    } | perf_ssh "bash -s"
}

# Settings dropdown index of a workload name. idle, off and ui run no saver.
perf_saver_type() {
    case $1 in
    idle | off | ui) echo 0 ;;
    toasters) echo 1 ;;
    starfield) echo 2 ;;
    pipes) echo 3 ;;
    *)
        echo "perf_saver_type: unknown workload '$1'" >&2
        return 1
        ;;
    esac
}

# EGL vsync and a 1 ms main-loop floor keep 16 ms frames even. Every device run sets both:
# the app logs neither when it equals the default, so no log shows which pacing is in force,
# and setting a default changes nothing.
# shellcheck disable=SC2034 # read by arm_measure.sh and by the device steps
PERF_PACING_ENV="HELIX_EGL_VSYNC=1 HELIX_LOOP_MIN_SLEEP_MS=1"

# One line with the Pi's temperature, throttle flags and ARM clock. The Pi 3B throttles under
# a long load gate, and a throttled run is not comparable with one that was not.
perf_thermal() {
    perf_ssh 'echo "$(vcgencmd measure_temp) $(vcgencmd get_throttled) arm_clock=$(vcgencmd measure_clock arm | cut -d= -f2)"' \
        < /dev/null 2> /dev/null
}

# True when a Moonraker print state leaves the board free for a load test. "none" means
# nothing answers at the Moonraker address the app is configured with.
perf_print_idle() {
    case $1 in
    standby | ready | complete | cancelled | error | none) return 0 ;;
    *) return 1 ;;
    esac
}

# For a step that drives the Pi 3B directly: claims device:pi3b for the calling session, then
# checks no print is running. Non-zero, holding nothing, when another session has the Pi or a
# print may be running. perf_pi3b_release gives the claim back.
perf_pi3b_take() {
    local claim=$PERF_REPO/scripts/helix-claim state
    if ! "$claim" check device:pi3b > /dev/null 2>&1; then
        echo "device:pi3b is held by another session; not touching the Pi" >&2
        "$claim" list 2> /dev/null | grep -A2 "device:pi3b" >&2
        return 1
    fi
    "$claim" take device:pi3b "${1:?give a reason}" > /dev/null || return 1
    state=$(perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=print_state)
    echo "PRINT_STATE $state"
    if ! perf_print_idle "$state"; then
        echo "not touching the Pi: print state '$state'" >&2
        perf_pi3b_release
        return 1
    fi
}

perf_pi3b_release() {
    "$PERF_REPO/scripts/helix-claim" release device:pi3b > /dev/null
}

# Path of the log file the running app holds open, read from its file descriptors so no step
# assumes where a platform's log hook writes; /var/log/messages when the app logs to syslog
# there instead. Prints an empty line when neither is found.
perf_app_log() {
    perf_ssh 'log=$(for p in $(pidof helix-screen); do for f in /proc/$p/fd/*; do readlink "$f"; done; done 2>/dev/null | grep "\.log$" | head -n 1)
if [ -z "$log" ] && grep -q helix-screen /var/log/messages 2>/dev/null; then log=/var/log/messages; fi
echo "$log"' < /dev/null
}
```

Create `scripts/screensaver-perf/pi3b_service.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs ON the Pi through perf_run_remote.
#   ACTION=stop     stops the service and waits for the app to exit
#   ACTION=start    starts it, waits for the control socket, prints what runs
#   ACTION=dropin   writes ENV_EXTRA ("KEY=VALUE ...") as a systemd drop-in; empty removes it
#   ACTION=collect  prints FILE, then deletes it
#   ACTION=crashes  prints how many app crashes the journal holds since SINCE (UTC)
#   ACTION=journal  prints the app's journal lines since SINCE (UTC) that match PATTERN (ERE)
#   ACTION=display_value  prints /display/KEY from the app's settings.json as JSON (null when absent)
#   ACTION=unset_display  stops the app, removes /display/KEY from settings.json, starts the app
#   ACTION=app_env  prints the running app's HELIX_* environment, one KEY=VALUE per line
#   ACTION=print_state  prints print_stats.state from the Moonraker in the app's settings:
#                   "none" when nothing listens there, "unknown" on any other failure
set -u
PW=${PW:-}
INSTALL=${INSTALL:?}
CTL_SOCK=${CTL_SOCK:?}
ACTION=${ACTION:?}
ENV_EXTRA=${ENV_EXTRA:-}
FILE=${FILE:-}
SINCE=${SINCE:-}
PATTERN=${PATTERN:-}
KEY=${KEY:-}
DROPIN_DIR=/etc/systemd/system/helixscreen.service.d
DROPIN=$DROPIN_DIR/helix-perf.conf

sudo_() {
    if [ -n "$PW" ]; then
        printf '%s\n' "$PW" | sudo -S -p '' "$@"
    else
        sudo -n "$@"
    fi
}

app_running() {
    pidof helix-screen helix-screen-egl >/dev/null
}

ctl_answers() {
    "$INSTALL/bin/helix-screen-egl" ctl -s "$CTL_SOCK" ping < /dev/null 2>/dev/null | grep -q pong
}

case $ACTION in
stop)
    sudo_ systemctl stop helixscreen
    for _ in $(seq 75); do
        app_running || break
        sleep 0.2
    done
    if app_running; then
        echo "DEPLOY_FAIL still running"
        exit 1
    fi
    ;;
start)
    sudo_ systemctl start helixscreen
    for _ in $(seq 90); do
        ctl_answers && break
        sleep 1
    done
    echo "DEVICE_VERSION $("$INSTALL/bin/helix-screen" --version 2>&1 | head -n 1)"
    echo "DEVICE_RUNG $(ps -eo args | grep -o "[/]${INSTALL#/}/bin/helix-screen[-a-z]*" | grep -v watchdog | sort -u | tr '\n' ' ')"
    echo "DEVICE_EGL_SHA $(sha256sum "$INSTALL/bin/helix-screen-egl" | cut -c1-16)"
    if ctl_answers; then
        echo DEPLOY_OK
    else
        echo "DEPLOY_FAIL ctl not answering"
    fi
    ;;
dropin)
    if [ -n "$ENV_EXTRA" ]; then
        {
            echo "[Service]"
            for kv in $ENV_EXTRA; do
                echo "Environment=$kv"
            done
        } > /tmp/helix-perf.conf
        sudo_ mkdir -p "$DROPIN_DIR"
        sudo_ cp /tmp/helix-perf.conf "$DROPIN"
        rm -f /tmp/helix-perf.conf
        sudo_ cat "$DROPIN"
    else
        sudo_ rm -f "$DROPIN"
    fi
    sudo_ systemctl daemon-reload
    ;;
collect)
    cat "${FILE:?}"
    sudo_ rm -f "$FILE"
    ;;
crashes)
    sudo_ journalctl -u helixscreen --since "${SINCE:?} UTC" --no-pager -o cat 2>/dev/null |
        grep -c 'Child exited with code 139'
    ;;
journal)
    sudo_ journalctl -u helixscreen --since "${SINCE:?} UTC" --no-pager -o cat 2>/dev/null |
        grep -E "${PATTERN:?}"
    ;;
display_value)
    python3 -c 'import json, sys; print(json.dumps(json.load(open(sys.argv[1])).get("display", {}).get(sys.argv[2]), sort_keys=True))' \
        "$INSTALL/config/settings.json" "${KEY:?}"
    ;;
unset_display)
    sudo_ systemctl stop helixscreen
    sudo_ python3 -c 'import json, sys; p = sys.argv[1]; c = json.load(open(p)); c.get("display", {}).pop(sys.argv[2], None); json.dump(c, open(p, "w"), indent=2)' \
        "$INSTALL/config/settings.json" "${KEY:?}"
    sudo_ systemctl start helixscreen
    ;;
app_env)
    pid=$(ps -eo pid,comm | awk '$2 ~ /^helix-screen/ {print $1; exit}')
    sudo_ cat "/proc/$pid/environ" 2>/dev/null | tr '\0' '\n' | grep -E '^HELIX_'
    ;;
print_state)
    python3 -c 'import json, sys, urllib.error, urllib.request
settings = json.load(open(sys.argv[1]))
printer = settings.get("printers", {}).get(settings.get("active_printer_id") or "default", {})
url = "http://%s:%s/printer/objects/query?print_stats=state" % (
    printer.get("moonraker_host") or "127.0.0.1", printer.get("moonraker_port") or 7125)
try:
    reply = json.load(urllib.request.urlopen(url, timeout=5))
    print(reply["result"]["status"]["print_stats"]["state"])
except urllib.error.URLError as error:
    print("none" if isinstance(error.reason, ConnectionRefusedError) else "unknown")
except Exception:
    print("unknown")' "$INSTALL/config/settings.json"
    ;;
*)
    echo "pi3b_service: unknown ACTION '$ACTION'" >&2
    exit 2
    ;;
esac
```

Create `scripts/screensaver-perf/pi3b_measure.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs ON the Pi through perf_run_remote with TYPE=<settings dropdown index> and optional
# SETTLE_S=<seconds> with no ctl traffic before a saver starts, for a gate idle baseline
# (the last 10 s before a saver runs) that sees the app quiet.
# TYPE=0 measures CPU on the idle home panel. Any other type starts that saver with the Test
# Screensaver button, measures CPU for 20 s and traces ioctls for 10 s; the trace is left at
# /tmp/hm_strace.txt for the host to collect. Prints CPU, STOPPED_MIDRUN and STARTED lines.
set -u
PW=${PW:-}
INSTALL=${INSTALL:?}
CTL_SOCK=${CTL_SOCK:?}
TYPE=${TYPE:-1}
SETTLE_S=${SETTLE_S:-0}
CTL_BIN="$INSTALL/bin/helix-screen-egl"

ctl() {
    "$CTL_BIN" ctl -s "$CTL_SOCK" "$@" < /dev/null > /dev/null 2>&1
}

sudo_() {
    if [ -n "$PW" ]; then
        printf '%s\n' "$PW" | sudo -S -p '' "$@"
    else
        sudo -n "$@"
    fi
}

PID=$(ps -eo pid,comm | awk '$2 ~ /^helix-screen/ {print $1; exit}')
HZ=$(getconf CLK_TCK)

# "<tid> <utime+stime>" for every thread of the app.
thread_ticks() {
    local task tid
    for task in /proc/"$PID"/task/*; do
        tid=${task##*/}
        # shellcheck disable=SC2046 # the stat fields are meant to split into arguments
        set -- $(sed 's/^.*) //' "$task/stat" 2>/dev/null)
        [ $# -ge 13 ] && echo "$tid $((${12} + ${13}))"
    done
}

cpu_window() {
    local seconds=$1
    thread_ticks | sort > /tmp/hm_s0
    sleep "$seconds"
    thread_ticks | sort > /tmp/hm_s1
    join /tmp/hm_s0 /tmp/hm_s1 | awk -v hz="$HZ" -v w="$seconds" -v pid="$PID" '
        { d = $3 - $2; tot += d; if ($1 == pid) mainv = d; else if (d > other) other = d }
        END { printf "CPU total=%.1f main=%.1f busiest_other=%.1f\n", (tot/hz)/w*100, (mainv/hz)/w*100, (other/hz)/w*100 }'
    rm -f /tmp/hm_s0 /tmp/hm_s1
}

ORIG_TYPE=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["display"]["screensaver_type"])' "$INSTALL/config/settings.json")
ctl navigate home
sleep 2

if [ "$TYPE" -eq 0 ]; then
    cpu_window 20
    exit 0
fi

ctl navigate settings
sleep 1
ctl click row_display_sound
sleep 1
ctl set_value row_screensaver "$TYPE"
sleep 1
sleep "$SETTLE_S"
T_START=$(date '+%Y-%m-%d %H:%M:%S')
ctl click btn_test_screensaver
sleep 5

cpu_window 20
sudo_ rm -f /tmp/hm_strace.txt
sudo_ timeout 10 strace -f -ttt -e trace=ioctl -o /tmp/hm_strace.txt -p "$PID" 2>/dev/null
echo "STOPPED_MIDRUN $(sudo_ journalctl -u helixscreen --since "$T_START" --no-pager -o cat 2>/dev/null | grep -c 'Stopping')"
echo "STARTED $(sudo_ journalctl -u helixscreen --since "$T_START" --no-pager -o cat 2>/dev/null | grep -c 'Started screensaver type')"

ctl wake
sleep 1
ctl navigate settings
sleep 1
ctl click row_display_sound
sleep 1
ctl set_value row_screensaver "$ORIG_TYPE"
sleep 1
ctl navigate home
```

Create `scripts/screensaver-perf/pi3b_loadgate.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs ON the Pi through perf_run_remote with ARM=<label> LABEL=<workload> TYPE=<dropdown
# index, 0 for none> UI_LOOP=0|1 DUR=<seconds>.
# Starts the saver (TYPE > 0) or cycles the base panels (UI_LOOP=1), loads two cores with
# stress-ng, runs cyclictest for DUR seconds and prints one GATE_RAW line. The histogram is
# left at /tmp/hm_cyclic_<arm>_<label>.txt for the host to collect.
# cyclictest needs root even for SCHED_OTHER; --laptop keeps it from pinning
# /dev/cpu_dma_latency to 0, which would hold the CPU out of idle states and distort the
# latency being measured.
set -u
PW=${PW:-}
INSTALL=${INSTALL:?}
CTL_SOCK=${CTL_SOCK:?}
ARM=${ARM:-unnamed}
LABEL=${LABEL:-off}
TYPE=${TYPE:-0}
UI_LOOP=${UI_LOOP:-0}
DUR=${DUR:-60}
CTL_BIN="$INSTALL/bin/helix-screen-egl"

ctl() {
    "$CTL_BIN" ctl -s "$CTL_SOCK" "$@" < /dev/null > /dev/null 2>&1
}

sudo_() {
    if [ -n "$PW" ]; then
        printf '%s\n' "$PW" | sudo -S -p '' "$@"
    else
        sudo -n "$@"
    fi
}

PID=$(ps -eo pid,comm | awk '$2 ~ /^helix-screen/ {print $1; exit}')
HZ=$(getconf CLK_TCK)

cpu_ticks() {
    # shellcheck disable=SC2046 # the stat fields are meant to split into arguments
    set -- $(sed 's/^.*) //' /proc/"$PID"/stat)
    echo $((${12} + ${13}))
}

ORIG_TYPE=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["display"]["screensaver_type"])' "$INSTALL/config/settings.json")
ctl navigate home
sleep 1
T_START=$(date '+%Y-%m-%d %H:%M:%S')
if [ "$TYPE" -ne 0 ]; then
    ctl navigate settings
    sleep 1
    ctl click row_display_sound
    sleep 1
    ctl set_value row_screensaver "$TYPE"
    sleep 1
    ctl click btn_test_screensaver
    sleep 4
fi

NAV=""
if [ "$UI_LOOP" = "1" ]; then
    (
        while :; do
            for panel in home controls filament settings print-select; do
                ctl navigate "$panel"
                sleep 2
            done
        done
    ) < /dev/null &
    NAV=$!
fi

stress-ng --cpu 2 --cpu-method matrixprod --timeout $((DUR + 15))s --quiet < /dev/null &
STRESS=$!
sleep 5

HIST=/tmp/hm_cyclic_${ARM}_${LABEL}.txt
T0=$(cpu_ticks)
sudo_ nice -n 0 cyclictest --laptop --policy=other -i 1000 -D "${DUR}s" -q -h 20000 > "$HIST" 2> "$HIST.err"
CT_EXIT=$?
T1=$(cpu_ticks)

kill "$STRESS" 2>/dev/null
wait "$STRESS" 2>/dev/null
if [ -n "$NAV" ]; then
    kill "$NAV" 2>/dev/null
    wait "$NAV" 2>/dev/null
fi
STARTED=$(sudo_ journalctl -u helixscreen --since "$T_START" --no-pager -o cat 2>/dev/null | grep -c "Started screensaver type")
STOPPED=$(sudo_ journalctl -u helixscreen --since "$T_START" --no-pager -o cat 2>/dev/null | grep -c "Stopped screensaver type")
CT_ERR=$(grep -v -i "password" "$HIST.err" | grep -v "cpu_dma_latency" | head -n 1 | tr ' ' '_')
rm -f "$HIST.err"

if [ "$TYPE" -ne 0 ]; then
    ctl wake
    sleep 1
    ctl navigate settings
    sleep 1
    ctl click row_display_sound
    sleep 1
    ctl set_value row_screensaver "$ORIG_TYPE"
    sleep 1
fi
ctl navigate home

CPU=$(awk -v d=$((T1 - T0)) -v hz="$HZ" -v w="$DUR" 'BEGIN { printf "%.1f", (d/hz)/w*100 }')
echo "GATE_RAW hist=$HIST ct_exit=$CT_EXIT ct_err=${CT_ERR:-none} helix_cpu=$CPU saver_started=$STARTED saver_stopped_midrun=$STOPPED"
```

- [ ] **Step 7: Write the host-side Pi scripts**

Create `scripts/screensaver-perf/pi3b_deploy.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Deploys helix-screen and helix-screen-egl from a directory to the Pi and proves which build runs.
# usage: pi3b_deploy.sh <bin-dir> [<tree whose ui_xml and assets are synced>]
# Prints DEVICE_VERSION, DEVICE_RUNG, DEVICE_EGL_SHA, DEPLOY_OK or DEPLOY_FAIL, and LOCAL_EGL_SHA.
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=perf_env.sh
. "$(dirname "$0")/perf_env.sh"

BIN_DIR=$1
TREE=${2:-}
for f in helix-screen helix-screen-egl; do
    [ -f "$BIN_DIR/$f" ] || {
        echo "DEPLOY_FAIL missing $BIN_DIR/$f"
        exit 1
    }
done

perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=stop
perf_scp "$BIN_DIR/helix-screen" "$BIN_DIR/helix-screen-egl" "$PERF_TARGET:$HELIX_PERF_INSTALL/bin/"
if [ -n "$TREE" ]; then
    for sub in ui_xml assets; do
        perf_rsync_dir "$TREE/$sub" "$HELIX_PERF_INSTALL/$sub"
    done
fi
perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=start
echo "LOCAL_EGL_SHA $(sha256sum "$BIN_DIR/helix-screen-egl" | cut -c1-16)"
```

Create `scripts/screensaver-perf/pi3b_pass.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One measurement pass on whatever is deployed to the Pi.
# usage: WORKLOADS="idle toasters starfield pipes" pi3b_pass.sh <arm> <run#>
# Appends "<arm> run=<n> <workload> CPU|FLIPS|STOPPED_MIDRUN|STARTED|THERMAL ..." lines to
# $HELIX_PERF_SCRATCH/results/<arm>.txt and keeps each trace under strace/.
set -uo pipefail
# shellcheck source-path=SCRIPTDIR source=perf_env.sh
. "$(dirname "$0")/perf_env.sh"

ARM=$1
RUN=$2
WORKLOADS=${WORKLOADS:-"idle toasters starfield pipes"}
OUT=$HELIX_PERF_SCRATCH/results/$ARM.txt
mkdir -p "$HELIX_PERF_SCRATCH/strace"

echo "$ARM run=$RUN start $(date '+%F %T') $(perf_ssh "$HELIX_PERF_INSTALL/bin/helix-screen --version 2>&1 | head -n 1")" >> "$OUT"

for workload in $WORKLOADS; do
    type=$(perf_saver_type "$workload") || exit 2
    result=$(perf_run_remote "$PERF_HERE/pi3b_measure.sh" "TYPE=$type" 2>&1)
    echo "$result" | grep -E '^(CPU|STOPPED_MIDRUN|STARTED)' | sed "s/^/$ARM run=$RUN $workload /" >> "$OUT"
    if [ "$type" -ne 0 ]; then
        trace=$HELIX_PERF_SCRATCH/strace/$ARM-r$RUN-$workload.txt
        perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=collect FILE=/tmp/hm_strace.txt > "$trace"
        python3 "$PERF_HERE/flips.py" "$trace" | sed "s/^/$ARM run=$RUN $workload /" >> "$OUT"
    fi
    echo "$ARM run=$RUN $workload THERMAL $(perf_thermal)" >> "$OUT"
done

echo "$ARM run=$RUN end $(date '+%F %T')" >> "$OUT"
grep " run=$RUN " "$OUT" | tail -n 40
```

Create `scripts/screensaver-perf/pi3b_gate.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Load-gate pass on whatever is deployed to the Pi.
# usage: SAVERS="off ui toasters" DUR=60 pi3b_gate.sh <arm> <run#>
# off is the idle home panel, ui cycles the base panels, a saver name runs that saver.
# Appends "<arm> run=<n> gate RESULT ..." and "<arm> run=<n> gate <label> THERMAL mid-run ..."
# lines to $HELIX_PERF_SCRATCH/results/<arm>.txt.
set -uo pipefail
# shellcheck source-path=SCRIPTDIR source=perf_env.sh
. "$(dirname "$0")/perf_env.sh"

ARM=$1
RUN=$2
SAVERS=${SAVERS:-"off toasters"}
DUR=${DUR:-60}
OUT=$HELIX_PERF_SCRATCH/results/$ARM.txt
mkdir -p "$HELIX_PERF_SCRATCH/cyclic"

for label in $SAVERS; do
    type=$(perf_saver_type "$label") || exit 2
    ui_loop=0
    [ "$label" = "ui" ] && ui_loop=1
    mid_thermal=$HELIX_PERF_SCRATCH/cyclic/$ARM-r$RUN-$label.thermal
    rm -f "$mid_thermal"
    # Sampled while the load runs: the load, not the pause after it, is what throttles the board.
    (sleep $((DUR / 2 + 5)); perf_thermal > "$mid_thermal") < /dev/null &
    sampler=$!
    raw=$(perf_run_remote "$PERF_HERE/pi3b_loadgate.sh" "ARM=$ARM" "LABEL=$label" "TYPE=$type" \
        "UI_LOOP=$ui_loop" "DUR=$DUR" 2>&1 | grep '^GATE_RAW' | tail -n 1)
    wait "$sampler"
    remote_hist=$(sed -n 's/.*hist=\([^ ]*\).*/\1/p' <<<"$raw")
    local_hist=$HELIX_PERF_SCRATCH/cyclic/$ARM-r$RUN-$label.txt
    if [ -n "$remote_hist" ]; then
        perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=collect "FILE=$remote_hist" > "$local_hist"
    else
        : > "$local_hist"
    fi
    python3 "$PERF_HERE/ctparse.py" "$local_hist" "$ARM" "$label" "${raw:-GATE_RAW missing}" |
        sed "s/^/$ARM run=$RUN gate /" >> "$OUT"
    echo "$ARM run=$RUN gate $label THERMAL mid-run $(cat "$mid_thermal" 2>/dev/null)" >> "$OUT"
done
grep " run=$RUN gate " "$OUT" | tail -n 16
```

Create `scripts/screensaver-perf/arm_measure.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One measured arm on the Pi: optionally build, deploy, smoke-test each saver for crashes,
# then three measurement passes and three load-gate passes, and a summary.
#   ARM=<label>                 names results/<ARM>.txt and <ARM>_summary.txt under $HELIX_PERF_SCRATCH
#   BUILD=1|0                   1 runs `make pi-docker` in $HELIX_PERF_BUILD_TREE; 0 deploys arms/<BIN_FROM>
#   BIN_FROM=<arm>              arm whose binaries BUILD=0 deploys (default ARM)
#   ENV_EXTRA="KEY=VALUE ..."   more drop-in variables; the pacing switches are always set
#   WORKLOADS="..."             measurement workloads (default "idle toasters starfield pipes")
#   SAVERS="..."                load-gate workloads (default "off toasters starfield pipes")
#   BASELINES="<arm> ..."       arms summarized beside this one
# Run detached: setsid nohup scripts/screensaver-perf/arm_measure.sh > <log> 2>&1 < /dev/null &
# It touches the Pi only while it holds device:pi3b and no print is running (PRINT_STATE), runs
# with PERF_PACING_ENV and ENV_EXTRA in a systemd drop-in, checks the app's environment carries
# them (APP_ENV), and on finishing removes the drop-in and restarts the app.
# Writes $HELIX_PERF_SCRATCH/<ARM>.done when it finishes, whether or not it succeeded.
set -uo pipefail
# shellcheck source-path=SCRIPTDIR source=perf_env.sh
. "$(dirname "$0")/perf_env.sh"

ARM=${ARM:?set ARM to a label for this arm}
BUILD=${BUILD:-1}
BIN_FROM=${BIN_FROM:-$ARM}
ENV_EXTRA=${ENV_EXTRA:-}
WORKLOADS=${WORKLOADS:-"idle toasters starfield pipes"}
SAVERS=${SAVERS:-"off toasters starfield pipes"}
BASELINES=${BASELINES:-}
CLAIM=$PERF_REPO/scripts/helix-claim
S=$HELIX_PERF_SCRATCH

DEVICE_TAKEN=0

finish() {
    if [ "$DEVICE_TAKEN" = "1" ]; then
        echo "=== $(date +%T) removing the environment drop-in"
        perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=dropin ENV_EXTRA= > /dev/null 2>&1
        perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=stop > /dev/null 2>&1
        perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=start > /dev/null 2>&1
    fi
    echo "=== $(date +%T) finish rc=${1:-0}"
    touch "$S/$ARM.done"
    exit "${1:-0}"
}
rm -f "$S/$ARM.done"

if [ "$BUILD" = "1" ]; then
    : "${HELIX_PERF_BUILD_TREE:?set HELIX_PERF_BUILD_TREE to the tree to build}"
    cd "$HELIX_PERF_BUILD_TREE" || finish 1
    git log -1 --format='HEAD %h %s'
    unapplied=0
    for patch in "$HELIX_PERF_BUILD_TREE"/patches/lvgl*.patch; do
        if git -C lib/lvgl apply --check "$patch" >/dev/null 2>&1; then
            echo "UNAPPLIED ON HOST: $(basename "$patch")"
            unapplied=1
        fi
    done
    [ "$unapplied" -eq 0 ] || finish 1
    echo "=== $(date +%T) build"
    build_claim="build:$(basename "$HELIX_PERF_BUILD_TREE")"
    "$CLAIM" take "$build_claim" "pi-docker arm $ARM" >/dev/null
    build_jobs=$("$CLAIM" jobs)
    make pi-docker NPROC_DOCKER_RUN="$build_jobs" > "$S/pi-$ARM.log" 2>&1
    build_exit=$?
    "$CLAIM" release "$build_claim" >/dev/null
    echo "BUILD_EXIT=$build_exit"
    grep -F "EGL binary carries" "$S/pi-$ARM.log"
    if [ "$build_exit" -ne 0 ]; then
        grep -n -E "error:|✗" "$S/pi-$ARM.log" | head -n 5
        finish 1
    fi
    mkdir -p "$S/arms/$ARM"
    cp build/pi/bin/helix-screen build/pi/bin/helix-screen-egl "$S/arms/$ARM/"
    git rev-parse --short HEAD > "$S/arms/$ARM/.sha"
    BIN_FROM=$ARM
fi

echo "=== $(date +%T) claiming device:pi3b and checking no print is running"
perf_pi3b_take "screensaver measurement arm $ARM" || finish 1
trap perf_pi3b_release EXIT
DEVICE_TAKEN=1

echo "=== $(date +%T) environment drop-in: '$PERF_PACING_ENV $ENV_EXTRA'"
perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=dropin "ENV_EXTRA=$PERF_PACING_ENV $ENV_EXTRA"

echo "=== $(date +%T) deploy $BIN_FROM"
"$PERF_HERE/pi3b_deploy.sh" "$S/arms/$BIN_FROM" "${HELIX_PERF_BUILD_TREE:-}" > "$S/deploy-$ARM.log" 2>&1
grep -E "DEVICE_|DEPLOY_|LOCAL_" "$S/deploy-$ARM.log"
grep -q DEPLOY_OK "$S/deploy-$ARM.log" || finish 1

echo "=== $(date +%T) app environment"
perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=app_env > "$S/env-$ARM.txt"
for kv in $PERF_PACING_ENV $ENV_EXTRA; do
    if ! grep -q -x -F "$kv" "$S/env-$ARM.txt"; then
        echo "APP_ENV missing $kv: the drop-in did not reach the app"
        finish 1
    fi
done
echo "APP_ENV ok: $PERF_PACING_ENV $ENV_EXTRA"

echo "=== $(date +%T) crash smoke"
since=$(date -u '+%Y-%m-%d %H:%M:%S')
for workload in $WORKLOADS; do
    [ "$workload" = "idle" ] && continue
    WORKLOADS="$workload" "$PERF_HERE/pi3b_pass.sh" "smoke-$ARM" 1 > /dev/null 2>&1
done
crashes=$(perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=crashes "SINCE=$since")
echo "CRASHES_DURING_SMOKE=$crashes"
[ "$crashes" = "0" ] || finish 1

since=$(date -u '+%Y-%m-%d %H:%M:%S')
for run in 1 2 3; do
    echo "=== $(date +%T) pass $run"
    WORKLOADS="$WORKLOADS" "$PERF_HERE/pi3b_pass.sh" "$ARM" "$run" > /dev/null 2>&1
    echo "=== $(date +%T) gate $run"
    SAVERS="$SAVERS" DUR=60 "$PERF_HERE/pi3b_gate.sh" "$ARM" "$run" > /dev/null 2>&1
done
crashes=$(perf_run_remote "$PERF_HERE/pi3b_service.sh" ACTION=crashes "SINCE=$since")
echo "CRASHES_DURING_MEASUREMENT=$crashes"
files=()
for arm in $BASELINES $ARM; do
    files+=("$S/results/$arm.txt")
done
python3 "$PERF_HERE/summarize.py" "${files[@]}" > "$S/${ARM}_summary.txt" 2>&1
finish 0
```

- [ ] **Step 8: Write the BusyBox gate scripts and the README**

Create `scripts/screensaver-perf/embedded_loadgate.sh`:

```sh
#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs ON a BusyBox board (CC1, AD5M) through embedded_gate.sh with PROBE=<probe path>
# HIST=<histogram path> DUR=<seconds> as its first lines.
# Loads half the cores with busy loops, runs the wake-up probe for DUR seconds and prints one
# GATE_RAW line in the fields ctparse.py reads. USER_HZ is 100 on these kernels, and BusyBox
# has no getconf.
PROBE=${PROBE:?}
HIST=${HIST:?}
DUR=${DUR:-60}
HZ=100

PID=$(pidof helix-screen | awk '{print $1}')
if [ -z "$PID" ]; then
    echo "GATE_RAW hist=$HIST ct_exit=3 ct_err=no_helix_screen helix_cpu=-1 saver_started=-1 saver_stopped_midrun=-1"
    exit 0
fi

cpu_ticks() {
    awk '{ sub(/^.*\) /, ""); print $12 + $13 }' "/proc/$PID/stat"
}

CORES=$(grep -c '^processor' /proc/cpuinfo)
LOADERS=$(((CORES + 1) / 2))
BUSY=""
i=0
while [ "$i" -lt "$LOADERS" ]; do
    sh -c 'while :; do :; done' < /dev/null &
    BUSY="$BUSY $!"
    i=$((i + 1))
done
sleep 5

T0=$(cpu_ticks)
"$PROBE" "$DUR" > "$HIST" 2> "$HIST.err"
CT_EXIT=$?
T1=$(cpu_ticks)

for p in $BUSY; do
    kill "$p" 2>/dev/null
done
CT_ERR=$(head -n 1 "$HIST.err" | tr ' ' '_')
rm -f "$HIST.err"
CPU=$(awk -v d=$((T1 - T0)) -v hz="$HZ" -v w="$DUR" 'BEGIN { printf "%.1f", (d / hz) / w * 100 }')
echo "GATE_RAW hist=$HIST ct_exit=$CT_EXIT ct_err=${CT_ERR:-none} helix_cpu=$CPU saver_started=-1 saver_stopped_midrun=-1"
```

Create `scripts/screensaver-perf/embedded_gate.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Load gate on a BusyBox board for whatever the app is running now.
# usage: DUR=60 HELIX_PERF_PROBE=<wakeup_probe built for the board> embedded_gate.sh <arm> <run#> <label>
# Appends "<arm> run=<n> gate RESULT ..." to $HELIX_PERF_SCRATCH/results/<arm>.txt.
set -uo pipefail
# shellcheck source-path=SCRIPTDIR source=perf_env.sh
. "$(dirname "$0")/perf_env.sh"

ARM=$1
RUN=$2
LABEL=$3
DUR=${DUR:-60}
: "${HELIX_PERF_PROBE:?set HELIX_PERF_PROBE to the wakeup_probe binary built for this board}"
OUT=$HELIX_PERF_SCRATCH/results/$ARM.txt
mkdir -p "$HELIX_PERF_SCRATCH/cyclic"
REMOTE_PROBE=/tmp/wakeup_probe
REMOTE_HIST=/tmp/hm_probe_${ARM}_${LABEL}.txt

perf_ssh "cat > $REMOTE_PROBE && chmod +x $REMOTE_PROBE" < "$HELIX_PERF_PROBE"
raw=$({
    printf 'PROBE=%s\nHIST=%s\nDUR=%s\n' "$REMOTE_PROBE" "$REMOTE_HIST" "$DUR"
    cat "$PERF_HERE/embedded_loadgate.sh"
} | perf_ssh "sh -s" | grep '^GATE_RAW' | tail -n 1)
local_hist=$HELIX_PERF_SCRATCH/cyclic/$ARM-r$RUN-$LABEL.txt
perf_ssh "cat $REMOTE_HIST; rm -f $REMOTE_HIST" > "$local_hist"
python3 "$PERF_HERE/ctparse.py" "$local_hist" "$ARM" "$LABEL" "${raw:-GATE_RAW missing}" |
    sed "s/^/$ARM run=$RUN gate /" >> "$OUT"
tail -n 1 "$OUT"
```

Create `scripts/screensaver-perf/README.md`:

````markdown
# Screensaver performance measurement

Scripts that measure what a screensaver costs on a real board: CPU per thread, presented
frames, and whether the printer's timing survives it (the load gate). They drive a deployed
app over ssh and `helix-screen ctl`, so they need a build with the control server.

## Settings

Every script sources `perf_env.sh`. Nothing about a device is written in the repo; export these:

| Variable | Meaning |
|---|---|
| `HELIX_PERF_HOST` | Device address (required) |
| `HELIX_PERF_SCRATCH` | Directory for results, logs, traces and copied binaries, outside the repo (required) |
| `HELIX_PERF_USER` | ssh user, default `pi` |
| `HELIX_PERF_PASSWORD` | ssh and sudo password; unset means key auth and passwordless sudo. Passed to `sshpass -e`, never on a command line |
| `HELIX_PERF_INSTALL` | Install root on the device, default `/home/pi/helixscreen` |
| `HELIX_PERF_CTL_SOCK` | Control socket on the device, default `/run/helixscreen/control.sock` |
| `HELIX_PERF_BUILD_TREE` | Tree `arm_measure.sh` builds with `make pi-docker` |
| `HELIX_PERF_PROBE` | Wake-up probe binary for `embedded_gate.sh` |

## The load gate

A run passes when the latency probe records **no wake-up of 20 ms or more and a p99 under
5 ms**, under a CPU load that stands in for klippy and moonraker. An arm passes only when
every run passes. On the Pi the probe is `cyclictest --laptop --policy=other -i 1000` beside
`stress-ng --cpu 2`; on BusyBox boards it is `wakeup_probe` beside busy loops on half the cores.
`summarize.py` prints the verdict.

## Raspberry Pi 3B

```bash
ARM=my-arm BUILD=1 HELIX_PERF_BUILD_TREE=$PWD \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$HELIX_PERF_SCRATCH/my-arm.log" 2>&1 < /dev/null &
```

The arm claims `device:pi3b` with `scripts/helix-claim`, deploys, smoke-tests every saver for
crashes, runs three measurement passes and three load-gate passes, and writes
`$HELIX_PERF_SCRATCH/my-arm_summary.txt`. `ENV_EXTRA="HELIX_SCREENSAVER_LEVEL=1"` runs the arm
with those variables in a systemd drop-in, which the arm removes (restarting the app) when it
finishes. `SAVERS="off ui"` measures the idle home panel and a loop through the base panels.

Before touching the Pi the arm asks the Moonraker in the app's settings for the print state and
stops unless the printer is idle (`PRINT_STATE` in the log). Every arm runs with
`HELIX_EGL_VSYNC=1 HELIX_LOOP_MIN_SLEEP_MS=1`, which keep 16 ms frames even. They are set rather
than detected, because the app logs neither when it equals the default; the `APP_ENV ok` line
confirms the running app carries them and `ENV_EXTRA` (`env-<ARM>.txt` holds its environment).
A step that drives the Pi by hand runs its commands between `perf_pi3b_take "<reason>"` and
`perf_pi3b_release` from `perf_env.sh`, which claim `device:pi3b` and make the same check.

Every pass appends a `THERMAL` line per workload (`vcgencmd measure_temp`, `get_throttled` and
the ARM clock) and every gate run one sampled mid-run. The Pi 3B throttles under the load gate
(80 to 84 C, `throttled=0x20002`, the ARM clock down to 818 MHz), and a throttled run is not
comparable with an unthrottled one. `summarize.py` ignores these lines; report the hottest
reading and the count of readings with a `throttled` other than `0x0` beside an arm's numbers:

```bash
grep -o "temp=[0-9.]*" "$HELIX_PERF_SCRATCH/results/my-arm.txt" | sort -t= -k2 -n | tail -n 1
grep THERMAL "$HELIX_PERF_SCRATCH/results/my-arm.txt" | grep -v -c "throttled=0x0"
```

Frames come from `strace` of the presenting ioctls: `DRM_IOCTL_MODE_PAGE_FLIP` on the dumb-DRM
binary and `DRM_IOCTL_MODE_ATOMIC` on the EGL binary.

## BusyBox boards (CC1, AD5M)

These boards have no cyclictest, stress-ng or python. Build the probe as a static armv7
binary in the CC1 toolchain image:

```bash
mkdir -p build/screensaver-perf
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src -w /src helixscreen/toolchain-cc1 \
  arm-none-linux-gnueabihf-gcc -std=c99 -O2 -static \
  -o build/screensaver-perf/wakeup_probe-armv7 scripts/screensaver-perf/wakeup_probe.c -lrt
```

Then, with the app already running the workload to measure:

```bash
DUR=60 HELIX_PERF_PROBE=build/screensaver-perf/wakeup_probe-armv7 \
  scripts/screensaver-perf/embedded_gate.sh cc1-fireworks 1 level0
python3 scripts/screensaver-perf/summarize.py "$HELIX_PERF_SCRATCH/results/cc1-fireworks.txt"
```

Never measure while a print runs, and get the board owner's approval before stressing a printer.
````

- [ ] **Step 9: Lint the scripts**

Run:

```bash
shellcheck -S warning -e SC3043,SC1091 scripts/screensaver-perf/*.sh; echo "exit $?"
.venv/bin/pytest tests/python/test_screensaver_perf_parsers.py -q
```

Expected: `exit 0` with no findings; `10 passed`.

- [ ] **Step 10: Index the harness in scripts/CLAUDE.md**

In `scripts/CLAUDE.md`, directly after the table under `### Release & Packaging` (before the next `### ` heading), insert:

```markdown
### Performance Measurement
| Script | Purpose |
|--------|---------|
| `screensaver-perf/` | Screensaver cost on real boards: CPU per thread, presented frames from DRM ioctls, and the load gate (cyclictest or `wakeup_probe` under load). Device settings come from `HELIX_PERF_*` variables; `screensaver-perf/README.md` has the flows |
```

- [ ] **Step 11: Prove a parser test can fail, then commit**

Mutation: in `scripts/screensaver-perf/flips.py`, change `FLIP_IOCTLS = ("DRM_IOCTL_MODE_PAGE_FLIP", "DRM_IOCTL_MODE_ATOMIC")` to `FLIP_IOCTLS = ("DRM_IOCTL_MODE_PAGE_FLIP",)`, run `.venv/bin/pytest tests/python/test_screensaver_perf_parsers.py -q`, confirm `test_flips_count_page_flip_and_atomic_ioctls_but_not_resumptions` fails, and restore the line.

```bash
git add -- scripts/screensaver-perf tests/python/test_screensaver_perf_parsers.py scripts/CLAUDE.md
git commit -m "tools(screensaver): measurement harness for the Pi 3B and BusyBox boards" -m "Measures a deployed saver's CPU, presented frames and load-gate latency. Device address and credentials come from HELIX_PERF_* variables, the gate verdict and the frame count (page flips and atomic commits) are pinned by pytest, and a static wakeup_probe stands in for cyclictest on boards without it." -m "Mutation: dropped DRM_IOCTL_MODE_ATOMIC from FLIP_IOCTLS; test_flips_count_page_flip_and_atomic_ioctls_but_not_resumptions went red"
```

---
### Task 1: Registry and type count

One always-compiled header names every saver and derives the count, the clamp, the `HELIX_SCREENSAVER_NOW` names and the dropdown order check from its rows. The hardcoded bound 3 disappears from the manager, the settings manager and the tests.

**Files:**
- Create: `include/screensaver_registry.h`
- Modify: `include/screensaver.h` (remove the `ScreensaverType` enum, include the registry), `src/ui/screensaver_manager.cpp#ScreensaverManager::configured_type`, `src/system/display_settings_manager.cpp#DisplaySettingsManager::init_subjects` and `#DisplaySettingsManager::set_screensaver_type`, `src/application/display_manager.cpp#DisplayManager::check_display_sleep`, `docs/devel/ENVIRONMENT_VARIABLES.md` (section `HELIX_SCREENSAVER_NOW`)
- Test: `tests/unit/test_screensaver_registry.cpp` (new), `tests/unit/test_screensaver.cpp` (section `out of range clamped`, one new test case)

**Interfaces:**
- Consumes: nothing.
- Produces (every later task):
  - `enum class ScreensaverType : int { OFF = 0, FLYING_TOASTERS = 1, STARFIELD = 2, PIPES_3D = 3 };` (global, moved into `include/screensaver_registry.h`; Task 10 adds `FIREWORKS = 4`)
  - `namespace helix::ui`: `enum SaverDepth : uint8_t { SAVER_DEPTH_16 = 1u << 0, SAVER_DEPTH_32 = 1u << 1 };`
  - `struct ScreensaverInfo { ScreensaverType type; const char* name; const char* label_key; uint8_t depths; };`
  - `inline constexpr ScreensaverInfo SCREENSAVERS[]`, `inline constexpr size_t SCREENSAVER_COUNT`, `inline constexpr const char* SCREENSAVER_OFF_LABEL_KEY = "Off";`
  - `constexpr int screensaver_last_type();`, `constexpr int clamp_screensaver_type(int value);`
  - `constexpr const ScreensaverInfo* find_screensaver(ScreensaverType type);`, `constexpr const ScreensaverInfo* find_screensaver_by_name(std::string_view name);`
  - `constexpr ScreensaverType resolve_screensaver_now(std::string_view value, ScreensaverType configured);`

- [ ] **Step 1: Write the failing registry tests**

Create `tests/unit/test_screensaver_registry.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_registry.h"

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::SCREENSAVER_COUNT;
using helix::ui::SCREENSAVERS;

namespace {

/// Value of `attr` on the self-closing element whose name is `name`, with &#10; decoded to a
/// newline, or "" when there is no such element or attribute.
std::string attribute_of_named(const std::string& xml, const std::string& name,
                               const std::string& attr) {
    const size_t at = xml.find("name=\"" + name + "\"");
    if (at == std::string::npos) {
        return "";
    }
    const size_t open = xml.rfind('<', at);
    const size_t close = xml.find("/>", at);
    if (open == std::string::npos || close == std::string::npos) {
        return "";
    }
    const std::string element = xml.substr(open, close - open);
    const std::regex pattern("\\s" + attr + "=\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(element, match, pattern)) {
        return "";
    }
    std::string value = match[1].str();
    for (size_t pos = value.find("&#10;"); pos != std::string::npos;
         pos = value.find("&#10;", pos)) {
        value.replace(pos, 5, "\n");
        pos += 1;
    }
    return value;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST_CASE("screensaver registry rows run in type order from 1 with unique names",
          "[screensaver][screensaver_registry]") {
    REQUIRE(SCREENSAVER_COUNT > 0);
    std::set<std::string> names;
    for (size_t i = 0; i < SCREENSAVER_COUNT; i++) {
        CAPTURE(i);
        CHECK(static_cast<int>(SCREENSAVERS[i].type) == static_cast<int>(i) + 1);
        CHECK_FALSE(std::string(SCREENSAVERS[i].name).empty());
        CHECK_FALSE(std::string(SCREENSAVERS[i].label_key).empty());
        CHECK(SCREENSAVERS[i].depths != 0);
        names.insert(SCREENSAVERS[i].name);
    }
    CHECK(names.size() == SCREENSAVER_COUNT);
    CHECK(helix::ui::screensaver_last_type() == static_cast<int>(SCREENSAVER_COUNT));
}

TEST_CASE("an out-of-range screensaver type clamps to the last type",
          "[screensaver][screensaver_registry]") {
    const int last = static_cast<int>(SCREENSAVER_COUNT);
    CHECK(helix::ui::clamp_screensaver_type(-1) == 0);
    CHECK(helix::ui::clamp_screensaver_type(0) == 0);
    CHECK(helix::ui::clamp_screensaver_type(1) == 1);
    CHECK(helix::ui::clamp_screensaver_type(last) == last);
    CHECK(helix::ui::clamp_screensaver_type(last + 1) == last);
    CHECK(helix::ui::clamp_screensaver_type(99) == last);
}

TEST_CASE("every registered screensaver name resolves to its row",
          "[screensaver][screensaver_registry]") {
    for (const helix::ui::ScreensaverInfo& info : SCREENSAVERS) {
        CAPTURE(info.name);
        const helix::ui::ScreensaverInfo* by_name = helix::ui::find_screensaver_by_name(info.name);
        REQUIRE(by_name != nullptr);
        CHECK(by_name->type == info.type);
        CHECK(helix::ui::find_screensaver(info.type) == by_name);
        CHECK(helix::ui::resolve_screensaver_now(info.name, ScreensaverType::OFF) == info.type);
    }
    CHECK(helix::ui::find_screensaver_by_name("") == nullptr);
    CHECK(helix::ui::find_screensaver_by_name("Starfield") == nullptr);
    CHECK(helix::ui::find_screensaver(ScreensaverType::OFF) == nullptr);
}

TEST_CASE("HELIX_SCREENSAVER_NOW falls back to the configured saver, then flying toasters",
          "[screensaver][screensaver_registry]") {
    using helix::ui::resolve_screensaver_now;
    CHECK(resolve_screensaver_now("1", ScreensaverType::STARFIELD) == ScreensaverType::STARFIELD);
    CHECK(resolve_screensaver_now("bogus", ScreensaverType::PIPES_3D) == ScreensaverType::PIPES_3D);
    CHECK(resolve_screensaver_now("1", ScreensaverType::OFF) == ScreensaverType::FLYING_TOASTERS);
}

TEST_CASE("the settings dropdown lists Off then every registered screensaver in type order",
          "[screensaver][screensaver_registry]") {
    std::ifstream file("ui_xml/settings_display_sound_overlay.xml");
    REQUIRE(file.is_open()); // helix-tests runs from the repo root
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string xml = buffer.str();

    std::vector<std::string> expected{helix::ui::SCREENSAVER_OFF_LABEL_KEY};
    for (const helix::ui::ScreensaverInfo& info : SCREENSAVERS) {
        expected.emplace_back(info.label_key);
    }
    CHECK(split_lines(attribute_of_named(xml, "row_screensaver", "options")) == expected);
    CHECK(split_lines(attribute_of_named(xml, "row_screensaver", "options_tag")) == expected);
}
```

In `tests/unit/test_screensaver.cpp`, add `#include "screensaver_registry.h"` below `#include "platform_capabilities.h"`, replace the body of `SECTION("out of range clamped")` with:

```cpp
    SECTION("out of range clamped") {
        DisplaySettingsManager::instance().set_screensaver_type(99);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() ==
                helix::ui::screensaver_last_type());

        DisplaySettingsManager::instance().set_screensaver_type(-1);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 0);
    }
```

and add this test case directly after `TEST_CASE_METHOD(LVGLTestFixture, "Screensaver type subject reflects setter", ...)`:

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "a stored or subject screensaver type past the last type clamps to the last type",
                 "[screensaver][display_settings]") {
    Config* config = Config::get_instance();
    const int stored_before = config->get<int>("/display/screensaver_type", 1);
    config->set<int>("/display/screensaver_type", 99);

    DisplaySettingsManager::instance().init_subjects();
    CHECK(DisplaySettingsManager::instance().get_screensaver_type() ==
          helix::ui::screensaver_last_type());

    // The manager clamps what the subject holds, whoever wrote it.
    lv_subject_set_int(DisplaySettingsManager::instance().subject_screensaver_type(), 99);
    CHECK(ScreensaverManager::configured_type() ==
          static_cast<ScreensaverType>(helix::ui::screensaver_last_type()));

    DisplaySettingsManager::instance().deinit_subjects();
    config->set<int>("/display/screensaver_type", stored_before);
}
```

`ScreensaverManager` is declared in `screensaver.h`; add `#include "screensaver.h"` next to the registry include (the file already includes it further down, which is harmless).

- [ ] **Step 2: Run the tests and watch them fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; `grep -n 'error' "$SS_SCRATCH/test-build.log"` shows `screensaver_registry.h: No such file or directory`.

- [ ] **Step 3: Write the registry header**

Create `include/screensaver_registry.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

/**
 * @file screensaver_registry.h
 * @brief The screensavers the app knows, as data
 *
 * Always compiled, including in builds without screensavers, so settings code can clamp a
 * stored type without the saver sources. The saver implementations are created in
 * src/ui/screensaver_manager.cpp.
 */

/**
 * @brief Available screensaver types
 *
 * Values map directly to the settings dropdown index and persisted config value.
 */
enum class ScreensaverType : int {
    OFF = 0,
    FLYING_TOASTERS = 1,
    STARFIELD = 2,
    PIPES_3D = 3,
};

namespace helix::ui {

/// Color depths a saver draws at, as bits of ScreensaverInfo::depths.
enum SaverDepth : uint8_t {
    SAVER_DEPTH_16 = 1u << 0,
    SAVER_DEPTH_32 = 1u << 1,
};

/// One registered screensaver.
struct ScreensaverInfo {
    ScreensaverType type;
    /// Stable name: the level store key and a HELIX_SCREENSAVER_NOW value.
    const char* name;
    /// Translation key, which is also the English label in the settings dropdown.
    const char* label_key;
    /// SaverDepth bits the saver draws at.
    uint8_t depths;
};

/// Every screensaver, in type order starting at 1: row i has type i + 1. The settings
/// dropdown in ui_xml/settings_display_sound_overlay.xml lists "Off" and then these labels in
/// this order, which a test checks.
inline constexpr ScreensaverInfo SCREENSAVERS[] = {
    {ScreensaverType::FLYING_TOASTERS, "toasters", "Flying Toasters", SAVER_DEPTH_32},
    {ScreensaverType::STARFIELD, "starfield", "Starfield", SAVER_DEPTH_32},
    {ScreensaverType::PIPES_3D, "pipes", "3D Pipes", SAVER_DEPTH_32},
};

inline constexpr size_t SCREENSAVER_COUNT = std::size(SCREENSAVERS);

/// Translation key and English label of the dropdown's first option.
inline constexpr const char* SCREENSAVER_OFF_LABEL_KEY = "Off";

/// Highest valid type value.
constexpr int screensaver_last_type() {
    return static_cast<int>(SCREENSAVERS[SCREENSAVER_COUNT - 1].type);
}

/// A stored type made valid: below OFF reads as OFF, past the last type as the last type.
constexpr int clamp_screensaver_type(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > screensaver_last_type()) {
        return screensaver_last_type();
    }
    return value;
}

/// The row for `type`, or nullptr for OFF and unregistered values.
constexpr const ScreensaverInfo* find_screensaver(ScreensaverType type) {
    for (const ScreensaverInfo& info : SCREENSAVERS) {
        if (info.type == type) {
            return &info;
        }
    }
    return nullptr;
}

/// The row whose stable name is `name`, or nullptr.
constexpr const ScreensaverInfo* find_screensaver_by_name(std::string_view name) {
    for (const ScreensaverInfo& info : SCREENSAVERS) {
        if (name == info.name) {
            return &info;
        }
    }
    return nullptr;
}

/**
 * @brief Saver a HELIX_SCREENSAVER_NOW value asks for
 *
 * A registered name starts that saver. Any other value starts the configured saver, or
 * flying toasters when the configured type is OFF.
 */
constexpr ScreensaverType resolve_screensaver_now(std::string_view value,
                                                  ScreensaverType configured) {
    if (const ScreensaverInfo* info = find_screensaver_by_name(value)) {
        return info->type;
    }
    return configured != ScreensaverType::OFF ? configured : ScreensaverType::FLYING_TOASTERS;
}

} // namespace helix::ui
```

- [ ] **Step 4: Point the enum's users at the registry**

In `include/screensaver.h`, delete the block from `/**` above `enum class ScreensaverType : int {` through its closing `};`, and add `#include "screensaver_registry.h"` above `#ifdef HELIX_ENABLE_SCREENSAVER` (after `#pragma once`).

In `src/ui/screensaver_manager.cpp`, replace the body of `ScreensaverManager::configured_type()` with:

```cpp
ScreensaverType ScreensaverManager::configured_type() {
    const int type_int = helix::DisplaySettingsManager::instance().get_screensaver_type();
    return static_cast<ScreensaverType>(helix::ui::clamp_screensaver_type(type_int));
}
```

In `src/system/display_settings_manager.cpp`, add `#include "screensaver_registry.h"` to the include block, replace `screensaver_type = std::clamp(screensaver_type, 0, 3);` in `init_subjects()` with `screensaver_type = helix::ui::clamp_screensaver_type(screensaver_type);`, and replace `type = std::clamp(type, 0, 3);` in `set_screensaver_type()` with `type = helix::ui::clamp_screensaver_type(type);`.

In `src/application/display_manager.cpp#DisplayManager::check_display_sleep`, replace the `HELIX_SCREENSAVER_NOW` block (from the comment `// HELIX_SCREENSAVER_NOW — force-start screensaver immediately (for testing)` through the closing brace of `if (!screensaver_force_checked)`) with:

```cpp
    // HELIX_SCREENSAVER_NOW: start a screensaver on the first tick. A registered saver name
    // picks that saver; any other value the configured one, or flying toasters.
    static bool screensaver_force_checked = false;
    if (!screensaver_force_checked) {
        screensaver_force_checked = true;
        const char* env = std::getenv("HELIX_SCREENSAVER_NOW");
        if (env) {
            const ScreensaverType force_type =
                helix::ui::resolve_screensaver_now(env, ScreensaverManager::configured_type());
            spdlog::info("[DisplayManager] HELIX_SCREENSAVER_NOW={}, forcing screensaver type {}",
                         env, static_cast<int>(force_type));
            m_display_dimmed = true;
            ScreensaverManager::instance().start(force_type);
            m_screensaver_active = true;
            return;
        }
    }
```

In `docs/devel/ENVIRONMENT_VARIABLES.md`, in the `HELIX_SCREENSAVER_NOW` section, replace the **Values** row and the **File** row of its table with:

```markdown
| **Values** | A saver name from `include/screensaver_registry.h#SCREENSAVERS` (`toasters`, `starfield`, `pipes`), or `1` / any other value (uses the configured type, falling back to flying toasters) |
| **File** | `src/application/display_manager.cpp`, names in `include/screensaver_registry.h` |
```

- [ ] **Step 5: Build and run the registry and settings tests**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver_registry],[display_settings]"` then `./build/bin/helix-tests "[screensaver]"`.
Expected: `exit 0`; all test cases pass, including `the settings dropdown lists Off then every registered screensaver in type order`.

- [ ] **Step 6: Mutate, then commit**

Mutation: in `clamp_screensaver_type`, change `return screensaver_last_type();` inside `if (value > screensaver_last_type())` to `return 0;`, rebuild, and confirm `an out-of-range screensaver type clamps to the last type` and `a stored or subject screensaver type past the last type clamps to the last type` fail. Restore.

```bash
.venv/bin/clang-format -i include/screensaver_registry.h include/screensaver.h src/ui/screensaver_manager.cpp src/system/display_settings_manager.cpp src/application/display_manager.cpp tests/unit/test_screensaver_registry.cpp tests/unit/test_screensaver.cpp
git add -- include/screensaver_registry.h include/screensaver.h src/ui/screensaver_manager.cpp src/system/display_settings_manager.cpp src/application/display_manager.cpp docs/devel/ENVIRONMENT_VARIABLES.md tests/unit/test_screensaver_registry.cpp tests/unit/test_screensaver.cpp
git commit -m "refactor(screensaver): one registry names the savers and bounds the type" -m "The type count, the stored-type clamp, the HELIX_SCREENSAVER_NOW names and the settings dropdown order all derive from one always-compiled row table, so adding a saver touches one row. An out-of-range type now clamps to the last type everywhere, including in ScreensaverManager::configured_type, which used to read it as Off." -m "Mutation: clamp_screensaver_type returned 0 past the last type; the registry clamp test and the stored-type clamp test went red"
```

---

### Task 2: Pixel fingerprints for pipes and starfield

Records a hash of the canvas pixels for a fixed seed and fixed frame times on the current code, before any refactor, so Tasks 3 and 4 can prove pixel-identical output.

**Files:**
- Create: `tests/unit/test_screensaver_fingerprint.cpp`, `tests/fixtures/screensaver/fingerprints.txt` (written by the test in record mode)
- Test: the same file

**Interfaces:**
- Consumes: `PipesScreensaverTestAccess::{set_fixed_seed, timer, canvas, set_total_segments, max_segments}`, `StarfieldScreensaverTestAccess::{set_fixed_seed, timer, canvas}` (existing), `ScreensaverStopOnExit` (existing).
- Produces (Tasks 3, 4): tag `[screensaver_fingerprint]`; the six seam functions `seed_pipes`, `pipes_timer`, `pipes_canvas`, `seed_starfield`, `starfield_timer`, `starfield_canvas` at the top of the test file are the only lines a refactor edits; the golden file `tests/fixtures/screensaver/fingerprints.txt` holds lines `pipes <16 hex digits>` and `starfield <16 hex digits>`.

- [ ] **Step 1: Write the fingerprint test**

Create `tests/unit/test_screensaver_fingerprint.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_pipes.h"
#include "screensaver_starfield.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

// Pixel fingerprints of the canvas savers. A fixed seed, a fixed resolution and fixed frame
// times make a run draw the same pixels every time, so any change to a single drawn pixel
// changes the hash. The recorded values live in FINGERPRINT_FILE; with
// HELIX_SCREENSAVER_FINGERPRINT_WRITE set, a test records the value it computes and fails, so
// a recording run can never pass as a comparison.

namespace {

constexpr const char* FINGERPRINT_FILE = "tests/fixtures/screensaver/fingerprints.txt";
constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

// The saver seams: the only lines that change when a saver moves to another test access class.
void seed_pipes(PipesScreensaver& ss, uint32_t seed) {
    PipesScreensaverTestAccess::set_fixed_seed(ss, seed);
}
lv_timer_t* pipes_timer(const PipesScreensaver& ss) {
    return PipesScreensaverTestAccess::timer(ss);
}
lv_obj_t* pipes_canvas(const PipesScreensaver& ss) {
    return PipesScreensaverTestAccess::canvas(ss);
}
void seed_starfield(StarfieldScreensaver& ss, uint32_t seed) {
    StarfieldScreensaverTestAccess::set_fixed_seed(ss, seed);
}
lv_timer_t* starfield_timer(const StarfieldScreensaver& ss) {
    return StarfieldScreensaverTestAccess::timer(ss);
}
lv_obj_t* starfield_canvas(const StarfieldScreensaver& ss) {
    return StarfieldScreensaverTestAccess::canvas(ss);
}

uint64_t fnv1a(uint64_t hash, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

/// Hash of every pixel byte of the canvas, row by row, leaving out any stride padding.
uint64_t canvas_hash(lv_obj_t* canvas) {
    REQUIRE(canvas != nullptr);
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    const uint32_t row_bytes =
        buf->header.w * lv_color_format_get_size(static_cast<lv_color_format_t>(buf->header.cf));
    uint64_t hash = FNV_OFFSET;
    for (uint32_t y = 0; y < buf->header.h; y++) {
        hash = fnv1a(hash, buf->data + static_cast<size_t>(y) * buf->header.stride, row_bytes);
    }
    return hash;
}

/// Folds one frame's hash into the run's.
uint64_t fold(uint64_t run, uint64_t frame) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) {
        bytes[i] = static_cast<uint8_t>(frame >> (8 * i));
    }
    return fnv1a(run, bytes, sizeof(bytes));
}

void fire(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

uint64_t pipes_run(uint32_t seed) {
    ScopedResolution resolution(lv_display_get_default(), 800, 480);
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    seed_pipes(ss, seed);
    ss.start();
    REQUIRE(ss.is_active());
    uint64_t run = fold(FNV_OFFSET, canvas_hash(pipes_canvas(ss)));
    for (int frame = 1; frame <= 120; frame++) {
        if (frame == 61) {
            // A full grid makes this frame reset the scene under a new camera.
            PipesScreensaverTestAccess::set_total_segments(
                ss, PipesScreensaverTestAccess::max_segments() + 1);
        }
        lv_tick_inc(100);
        fire(pipes_timer(ss));
        if (frame % 20 == 0) {
            run = fold(run, canvas_hash(pipes_canvas(ss)));
        }
    }
    return run;
}

uint64_t starfield_run(uint32_t seed) {
    ScopedResolution resolution(lv_display_get_default(), 800, 480);
    StarfieldScreensaver ss;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
    seed_starfield(ss, seed);
    ss.start();
    REQUIRE(ss.is_active());
    uint64_t run = fold(FNV_OFFSET, canvas_hash(starfield_canvas(ss)));
    constexpr uint32_t FRAME_MS[] = {33, 16, 50, 7, 70};
    for (int frame = 1; frame <= 150; frame++) {
        lv_tick_inc(FRAME_MS[frame % 5]);
        fire(starfield_timer(ss));
        if (frame % 25 == 0) {
            run = fold(run, canvas_hash(starfield_canvas(ss)));
        }
    }
    return run;
}

std::string hex(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016" PRIx64, value);
    return text;
}

std::map<std::string, std::string> read_fingerprints() {
    std::map<std::string, std::string> values;
    std::ifstream file(FINGERPRINT_FILE);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        std::string name;
        std::string value;
        if (fields >> name >> value) {
            values[name] = value;
        }
    }
    return values;
}

void write_fingerprints(const std::map<std::string, std::string>& values) {
    std::ofstream file(FINGERPRINT_FILE, std::ios::trunc);
    file << "# Canvas pixel fingerprints; tests/unit/test_screensaver_fingerprint.cpp\n";
    for (const auto& [name, value] : values) {
        file << name << ' ' << value << '\n';
    }
}

/// Fingerprints are recorded with the Linux x86-64 test build; another compiler or target may
/// contract floating point differently and draw a pixel one step apart.
void skip_off_the_recording_platform() {
#if !(defined(__linux__) && defined(__x86_64__))
    SKIP("screensaver fingerprints are recorded on x86-64 Linux");
#endif
}

void check_fingerprint(const std::string& name, uint64_t value) {
    if (std::getenv("HELIX_SCREENSAVER_FINGERPRINT_WRITE") != nullptr) {
        std::map<std::string, std::string> values = read_fingerprints();
        values[name] = hex(value);
        write_fingerprints(values);
        FAIL("recorded " << name << " " << hex(value) << " in " << FINGERPRINT_FILE
                         << "; unset HELIX_SCREENSAVER_FINGERPRINT_WRITE to compare");
    }
    const std::map<std::string, std::string> recorded = read_fingerprints();
    INFO("fingerprint file " << FINGERPRINT_FILE << " (run helix-tests from the repo root)");
    REQUIRE(recorded.count(name) == 1);
    CHECK(hex(value) == recorded.at(name));
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the pipes canvas matches its recorded pixel fingerprint",
                 "[screensaver][screensaver_fingerprint]") {
    skip_off_the_recording_platform();
    check_fingerprint("pipes", pipes_run(42));
}

TEST_CASE_METHOD(LVGLTestFixture, "the starfield canvas matches its recorded pixel fingerprint",
                 "[screensaver][screensaver_fingerprint]") {
    skip_off_the_recording_platform();
    check_fingerprint("starfield", starfield_run(5));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a fingerprint run replays exactly and a different seed draws different pixels",
                 "[screensaver][screensaver_fingerprint]") {
    CHECK(pipes_run(42) == pipes_run(42));
    CHECK(pipes_run(42) != pipes_run(43));
    CHECK(starfield_run(5) == starfield_run(5));
    CHECK(starfield_run(5) != starfield_run(6));
}

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 2: Run it and watch it fail on the missing recording**

Run: `mkdir -p tests/fixtures/screensaver; make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver_fingerprint]"`.
Expected: `exit 0`; both "matches its recorded pixel fingerprint" cases fail at `REQUIRE( recorded.count(name) == 1 )`; the replay case passes.

- [ ] **Step 3: Record the fingerprints from the current code**

Run: `HELIX_SCREENSAVER_FINGERPRINT_WRITE=1 ./build/bin/helix-tests "[screensaver_fingerprint]"`
Expected: both recording cases fail with `recorded pipes <hex>` and `recorded starfield <hex>`; `cat tests/fixtures/screensaver/fingerprints.txt` shows the comment line, a `pipes` line and a `starfield` line.

- [ ] **Step 4: Compare against the recording**

Run: `./build/bin/helix-tests "[screensaver_fingerprint]"` twice.
Expected: all three test cases pass both times.

- [ ] **Step 5: Mutate, then commit**

Mutation: in `src/ui/screensaver_pipes.cpp` change `static constexpr float PIPE_RADIUS = 0.22f;` to `0.23f`, rebuild, confirm `the pipes canvas matches its recorded pixel fingerprint` fails; restore. Then in `src/ui/screensaver_starfield_sim.cpp` change `constexpr float COLOR_THRESHOLD = 0.35f;` to `0.36f`, rebuild, confirm `the starfield canvas matches its recorded pixel fingerprint` fails; restore and rebuild.

```bash
.venv/bin/clang-format -i tests/unit/test_screensaver_fingerprint.cpp
git add -- tests/unit/test_screensaver_fingerprint.cpp tests/fixtures/screensaver/fingerprints.txt
git commit -m "test(screensaver): pixel fingerprints of the pipes and starfield canvases" -m "A seeded run at 800x480 with fixed frame times hashes the canvas bytes every few frames, including a pipes grid reset, and compares with the recorded values. The refactors onto the shared saver parts must leave both hashes unchanged." -m "Mutation: PIPE_RADIUS 0.22f to 0.23f and, separately, COLOR_THRESHOLD 0.35f to 0.36f; the pipes and starfield fingerprint tests each went red"
```

---
### Task 3: Shared saver parts, the saver base, and pipes on them

Builds the foundation every saver runs on (overlay, canvas with dirty-area merge and the dirty-box layer session, frame timer, base class, generic test access) and moves pipes onto it. Level 0 of every ladder runs at 16 ms (`SAVER_FAST_PERIOD`), or at `HELIX_SCREENSAVER_REFR_PERIOD_MS` when that is set for manual testing; levels 1 and up keep their declared periods, and while a saver runs the display refresh follows its current period, so the two stay equal. The pipes fingerprint from Task 2 must not change, and a Pi 3B arm must show no regression.

**Files:**
- Create: `include/screensaver_frame.h`, `src/ui/screensaver_frame.cpp`, `include/screensaver_overlay.h`, `src/ui/screensaver_overlay.cpp`, `include/screensaver_canvas.h`, `src/ui/screensaver_canvas.cpp`, `include/screensaver_frame_timer.h`, `src/ui/screensaver_frame_timer.cpp`, `include/screensaver_base.h`, `src/ui/screensaver_base.cpp`
- Modify: `include/refresh_period_hold.h` (`follow`), `src/application/display_manager.cpp#RefreshPeriodHold`, `tests/test_helpers/refresh_period_hold_test_access.h#RefreshPeriodHoldTestAccess::reset`, `include/screensaver.h#ScreensaverManager::start` (doc), `docs/devel/ENVIRONMENT_VARIABLES.md` (`HELIX_SCREENSAVER_REFR_PERIOD_MS`), `include/screensaver_starfield_sim.h` (`DirtyRect` moves to the frame header), `include/screensaver_pipes.h` (rewritten), `src/ui/screensaver_pipes.cpp` (lifecycle, frame and layer-session sections), `tests/test_helpers/screensaver_test_access.h` (`helix::ui::SaverTestAccess`, `ScopedGlobalRefreshHold`, `level_zero_periods`; `PipesScreensaverTestAccess` keeps only simulation state), `tests/unit/test_screensaver.cpp`, `tests/unit/test_screensaver_canvas_stride.cpp`, `tests/unit/test_screensaver_pipes_canvas.cpp`, `tests/unit/test_screensaver_motion.cpp`, `tests/unit/test_screensaver_fingerprint.cpp` (pipes access call sites), `Makefile` (screensaver source filter), `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`
- Test: `tests/unit/test_screensaver_parts.cpp` (new), `tests/unit/application/test_refresh_period_hold.cpp` (two new cases), `tests/unit/test_screensaver.cpp` (one new case), `tests/unit/test_screensaver_motion.cpp` (the pipes period case), `[screensaver_fingerprint]`, `[screensaver]`, `[refresh_period]`

**Interfaces:**
- Consumes: `helix::ui::screensaver::MotionClock` (`include/screensaver_motion.h`), `helix::ui::screensaver_canvas_stride_bytes(int32_t, lv_color_format_t)` (`include/screensaver.h`), `helix::RefreshPeriodHold` and `helix::active_refresh_period_hold()` (`include/refresh_period_hold.h`), `helix::ui::safe_delete_deferred(lv_obj_t*&)` (`include/ui_utils.h`), `helix::ui::lv_timer_cancel_safe(lv_timer_t*)` (`include/ui_timer_guard.h`), the Task 2 fingerprint seams.
- Produces (Tasks 4, 5, 6, 9, 10):
  - `include/screensaver_frame.h` (pure, `helix::ui`): `inline constexpr uint32_t SAVER_FAST_PERIOD = 16;`, `inline constexpr size_t SAVER_MAX_DIRTY_AREAS = 32;`, `struct DirtyRect { int32_t x1, y1, x2, y2; bool empty() const; int64_t area() const; void add(int32_t, int32_t, int32_t, int32_t); void add(const DirtyRect&); bool operator==(const DirtyRect&) const; };`, `void merge_dirty_areas(std::vector<DirtyRect>& areas, size_t max_areas);`
  - `class SaverOverlay { void create(); void make_transparent(); void destroy(); lv_obj_t* obj() const; };`
  - `class SaverCanvas { bool create(lv_obj_t* parent, int32_t w, int32_t h, lv_color_format_t cf); void release(); lv_obj_t* obj() const; uint8_t* data() const; uint32_t stride() const; size_t buffer_size() const; lv_color_format_t format() const; int32_t width() const; int32_t height() const; void fill_black(); void invalidate(std::vector<DirtyRect>& areas); void begin_layer(lv_layer_t*); void mark_dirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2); void finish_layer(lv_layer_t*); };`
  - `class SaverFrameTimer { using FrameFn = std::function<void(uint32_t dt_ms)>; void start(uint32_t period_ms, FrameFn on_frame); void set_period(uint32_t period_ms); void cancel(); lv_timer_t* timer() const; };`
  - `class SaverBase : public Screensaver` with public `void start() final; void stop() final; bool is_active() const final; size_t level() const; size_t level_count() const; uint32_t level_period_ms(size_t level) const; void set_start_level(size_t level); void request_level(size_t level);`, protected pure virtuals `std::optional<lv_color_format_t> canvas_format() const`, `bool on_start()`, `void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty)`, `void on_stop()`, `size_t ladder_size() const`, `uint32_t ladder_period_ms(size_t level) const`, protected virtual `void on_level_request(size_t level)` (default applies at once), protected `void apply_level(size_t level)` (it and `start()` call `active_refresh_period_hold().follow(period)`), `SaverOverlay& overlay()`, `SaverCanvas& canvas()`, `std::minstd_rand& rng()`, `int32_t screen_w() const`, `int32_t screen_h() const`.
  - `inline constexpr size_t TWO_LEVEL_COUNT = 2;`, `constexpr uint32_t two_level_period_ms(size_t level);` (level 0 `SAVER_FAST_PERIOD`, else 33)
  - `void helix::RefreshPeriodHold::follow(uint32_t saver_period_ms);`: while held, the refresh and animation timers run at the saver's period from then on; `acquire()` is unchanged and still starts them at the configured `period()`. `SaverBase::level_period_ms(0)` returns `RefreshPeriodHold::period()` when one is configured (`HELIX_SCREENSAVER_REFR_PERIOD_MS`), else the ladder's level 0 period; other levels return their ladder period. `ScreensaverManager` is unchanged.
  - Test seam `helix::ui::SaverTestAccess` (`tests/test_helpers/screensaver_test_access.h`, re-exported globally with `using`): `overlay`, `canvas`, `timer`, `draw_buf_size`, `draw_buf_stride`, `set_fixed_seed`; also `struct ScopedGlobalRefreshHold` (the global hold released with no configured period, on entry and exit) and `LevelZeroPeriods level_zero_periods<Saver>(uint32_t configured_ms)` (level 0's timer period and the held display refresh of a fresh saver).

- [ ] **Step 0: Record a Pi 3B control arm on the Task 2 commit**

Preston approved device use this session? Ask him once, before the first Pi 3B command. Then, with `HELIX_PERF_HOST`, `HELIX_PERF_PASSWORD` and `HELIX_PERF_USER` taken from the device roster memory (`reference_ssh_access.md`) and `HELIX_PERF_SCRATCH="$SS_SCRATCH/perf"`:

```bash
git log -1 --format='%h %s'   # the Task 2 fingerprint commit
ARM=task2-control BUILD=1 HELIX_PERF_BUILD_TREE="$PWD" ENV_EXTRA="HELIX_SCREENSAVER_REFR_PERIOD_MS=16" \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$SS_SCRATCH/task2-control.log" 2>&1 < /dev/null &
```

`HELIX_SCREENSAVER_REFR_PERIOD_MS=16` runs the Task 2 savers, which tick at the display refresh period, at 16 ms, the period every refactored saver's level 0 runs at, so the arms compare like for like. The refactor arms in Tasks 3, 4 and 5 use the same `ENV_EXTRA`.

The arm runs with `HELIX_EGL_VSYNC=1 HELIX_LOOP_MIN_SLEEP_MS=1` in its drop-in, as every device run in this plan does, since the 57 fps bar in Task 11 assumes that pacing. Once it has deployed, `grep -E '^(PRINT_STATE|APP_ENV)' "$SS_SCRATCH/task2-control.log"` must show a print state that is not `printing` or `paused` and `APP_ENV ok`; either failing stops the arm, and then tell Preston what it printed.

Wait until `$HELIX_PERF_SCRATCH/arms/task2-control/.sha` exists (the binaries are copied out of the tree) before editing or building anything below; the arm keeps measuring in the background and writes `$HELIX_PERF_SCRATCH/task2-control.done` at the end. Compare its summary with the 2026-09-14 reference at 16 ms with EGL vsync and a 1 ms loop floor (toasters 58.6 fps and 23.3% CPU, starfield 58.7 fps and 45.9%, pipes 10.0 fps and 8.2%). If any saver differs from the reference by more than 20% of its CPU, stop and tell Preston the board or its firmware changed before any refactor is measured against it.

- [ ] **Step 1: Write the failing tests for the shared parts**

Create `tests/unit/test_screensaver_parts.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/refresh_period_hold_test_access.h"
#include "../test_helpers/screensaver_test_access.h"
#include "refresh_period_hold.h"
#include "screensaver_base.h"
#include "screensaver_canvas.h"
#include "screensaver_frame.h"
#include "screensaver_frame_timer.h"
#include "screensaver_overlay.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::SAVER_MAX_DIRTY_AREAS;
using helix::ui::SaverCanvas;
using helix::ui::SaverFrameTimer;
using helix::ui::SaverOverlay;

namespace {

bool box_covers(const std::vector<DirtyRect>& boxes, const DirtyRect& r) {
    return std::any_of(boxes.begin(), boxes.end(), [&](const DirtyRect& b) {
        return b.x1 <= r.x1 && b.y1 <= r.y1 && b.x2 >= r.x2 && b.y2 >= r.y2;
    });
}

bool areas_cover_point(const std::vector<lv_area_t>& areas, int32_t x, int32_t y) {
    return std::any_of(areas.begin(), areas.end(), [&](const lv_area_t& a) {
        return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
    });
}

void fire(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

lv_area_t coords_of(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area;
}

constexpr uint32_t PROBE_PERIODS_MS[] = {helix::ui::SAVER_FAST_PERIOD, 45, 50};

/// A saver that records what the base asks of it.
class ProbeSaver : public helix::ui::SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::STARFIELD;
    }
    using SaverBase::apply_level;

    std::optional<lv_color_format_t> format = LV_COLOR_FORMAT_XRGB8888;
    bool refuse = false;
    bool defer_levels = false;
    bool canvas_at_start = false;
    size_t level_at_start = 99;
    uint32_t first_random = 0;
    std::vector<std::string> calls;
    std::vector<uint32_t> frame_dts;
    std::vector<DirtyRect> frame_dirty;
    std::vector<size_t> level_requests;

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return format;
    }
    bool on_start() override {
        calls.emplace_back("start");
        canvas_at_start = canvas().obj() != nullptr;
        level_at_start = level();
        first_random = rng()();
        return !refuse;
    }
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override {
        calls.emplace_back("frame");
        frame_dts.push_back(dt_ms);
        dirty = frame_dirty;
    }
    void on_stop() override {
        calls.emplace_back("stop");
    }
    size_t ladder_size() const override {
        return std::size(PROBE_PERIODS_MS);
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return PROBE_PERIODS_MS[level];
    }
    void on_level_request(size_t level) override {
        level_requests.push_back(level);
        if (!defer_levels) {
            apply_level(level);
        }
    }
};

} // namespace

// ============================================================================
// merge_dirty_areas
// ============================================================================

TEST_CASE("merged dirty areas number at most the limit and cover every input",
          "[screensaver][screensaver_parts]") {
    std::minstd_rand rng(17);
    std::vector<DirtyRect> input;
    for (int i = 0; i < 45; i++) {
        const auto x = static_cast<int32_t>(rng() % 780);
        const auto y = static_cast<int32_t>(rng() % 460);
        input.push_back({x, y, x + static_cast<int32_t>(rng() % 20), y + static_cast<int32_t>(rng() % 20)});
    }
    REQUIRE(input.size() > SAVER_MAX_DIRTY_AREAS);

    std::vector<DirtyRect> merged = input;
    helix::ui::merge_dirty_areas(merged, SAVER_MAX_DIRTY_AREAS);

    CHECK(merged.size() == SAVER_MAX_DIRTY_AREAS);
    for (const DirtyRect& r : input) {
        CHECK(box_covers(merged, r));
    }
}

TEST_CASE("merging within the limit keeps the areas in order and drops empty ones",
          "[screensaver][screensaver_parts]") {
    std::vector<DirtyRect> areas = {{0, 0, 9, 9}, {}, {100, 100, 120, 110}, {50, 5, 60, 6}};
    helix::ui::merge_dirty_areas(areas, SAVER_MAX_DIRTY_AREAS);
    REQUIRE(areas.size() == 3);
    CHECK(areas[0] == DirtyRect{0, 0, 9, 9});
    CHECK(areas[1] == DirtyRect{100, 100, 120, 110});
    CHECK(areas[2] == DirtyRect{50, 5, 60, 6});
}

TEST_CASE("merging joins the pair that adds the fewest pixels",
          "[screensaver][screensaver_parts]") {
    std::vector<DirtyRect> areas = {{0, 0, 9, 9}, {10, 0, 19, 9}, {500, 400, 509, 409}};
    helix::ui::merge_dirty_areas(areas, 2);
    REQUIRE(areas.size() == 2);
    CHECK(areas[0] == DirtyRect{0, 0, 19, 9});
    CHECK(areas[1] == DirtyRect{500, 400, 509, 409});
}

// ============================================================================
// SaverOverlay
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver overlay is an opaque black touch-absorbing child of the top layer",
                 "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    lv_obj_t* obj = overlay.obj();
    REQUIRE(obj != nullptr);
    CHECK(lv_obj_get_parent(obj) == lv_layer_top());
    CHECK(lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), lv_color_black()));
    CHECK(lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE));
    CHECK_FALSE(lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE));

    overlay.make_transparent();
    CHECK(lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == LV_OPA_TRANSP);

    overlay.destroy();
    CHECK(overlay.obj() == nullptr);
    // Deleted on a later timer pass, hidden until then.
    CHECK(lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN));
}

// ============================================================================
// SaverCanvas
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver canvas allocates at LVGL's stride for its format and starts opaque black",
                 "[screensaver][screensaver_parts]") {
    const lv_color_format_t cf = GENERATE(as<lv_color_format_t>{}, LV_COLOR_FORMAT_XRGB8888,
                                          LV_COLOR_FORMAT_ARGB8888, LV_COLOR_FORMAT_RGB565);
    const int32_t w = GENERATE(630, 800);
    CAPTURE(static_cast<int>(cf), w);
    constexpr int32_t H = 120;

    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), w, H, cf));
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas.obj());
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == cf);
    CHECK(canvas.format() == cf);
    CHECK(canvas.width() == w);
    CHECK(canvas.height() == H);
    CHECK(canvas.stride() == lv_draw_buf_width_to_stride(static_cast<uint32_t>(w), cf));
    CHECK(canvas.stride() == buf->header.stride);
    CHECK(canvas.buffer_size() == static_cast<size_t>(canvas.stride()) * H);
    CHECK(canvas.buffer_size() >= buf->data_size);

    const uint32_t bytes = lv_color_format_get_size(cf);
    size_t not_black = 0;
    for (int32_t y = 0; y < H; y++) {
        const uint8_t* row = canvas.data() + static_cast<size_t>(y) * canvas.stride();
        for (int32_t x = 0; x < w; x++) {
            const uint8_t* px = row + static_cast<size_t>(x) * bytes;
            if (bytes == 2) {
                not_black += (px[0] | px[1]) != 0 ? 1 : 0;
            } else {
                not_black += ((px[0] | px[1] | px[2]) != 0 || px[3] != 0xFF) ? 1 : 0;
            }
        }
    }
    CHECK(not_black == 0);

    canvas.release();
    overlay.destroy();
}

TEST_CASE_METHOD(LVGLTestFixture, "releasing a saver canvas hides it and frees its buffer",
                 "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), 64, 48, LV_COLOR_FORMAT_XRGB8888));
    lv_obj_t* obj = canvas.obj();

    canvas.release();

    CHECK(lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN));
    CHECK(canvas.obj() == nullptr);
    CHECK(canvas.data() == nullptr);
    CHECK(canvas.buffer_size() == 0);
    overlay.destroy();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver canvas invalidates at most SAVER_MAX_DIRTY_AREAS areas covering every dirty box",
                 "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), 800, 480, LV_COLOR_FORMAT_XRGB8888));
    const lv_area_t coords = coords_of(canvas.obj());

    std::vector<DirtyRect> dirty;
    for (int32_t i = 0; i < 40; i++) {
        dirty.push_back({i * 19, i * 11, i * 19 + 2, i * 11 + 2});
    }
    const std::vector<DirtyRect> input = dirty;
    helix::test::InvalidatedAreas invalidated(lv_obj_get_display(canvas.obj()));

    canvas.invalidate(dirty);

    REQUIRE_FALSE(invalidated.areas.empty());
    CHECK(invalidated.areas.size() <= SAVER_MAX_DIRTY_AREAS);
    for (const DirtyRect& r : input) {
        CHECK(areas_cover_point(invalidated.areas, coords.x1 + r.x1, coords.y1 + r.y1));
        CHECK(areas_cover_point(invalidated.areas, coords.x1 + r.x2, coords.y1 + r.y2));
    }
    canvas.release();
    overlay.destroy();
}

TEST_CASE_METHOD(LVGLTestFixture, "a saver canvas layer session invalidates only the areas it marked",
                 "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), 200, 100, LV_COLOR_FORMAT_ARGB8888));
    const lv_area_t coords = coords_of(canvas.obj());
    helix::test::InvalidatedAreas invalidated(lv_obj_get_display(canvas.obj()));

    lv_layer_t layer;
    canvas.begin_layer(&layer);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_white();
    dsc.bg_opa = LV_OPA_COVER;
    const lv_area_t rect = {10, 20, 29, 39};
    lv_draw_rect(&layer, &dsc, &rect);
    canvas.mark_dirty(10, 20, 29, 39);
    canvas.finish_layer(&layer);

    REQUIRE(invalidated.areas.size() == 1);
    CHECK(invalidated.areas[0].x1 == coords.x1 + 10);
    CHECK(invalidated.areas[0].y1 == coords.y1 + 20);
    CHECK(invalidated.areas[0].x2 == coords.x1 + 29);
    CHECK(invalidated.areas[0].y2 == coords.y1 + 39);
    // The session drew the rect into the buffer.
    const uint8_t* px = canvas.data() + 25 * static_cast<size_t>(canvas.stride()) + 15 * 4;
    CHECK(px[0] == 0xFF);
    CHECK(px[1] == 0xFF);
    CHECK(px[2] == 0xFF);
    canvas.release();
    overlay.destroy();
}

// ============================================================================
// SaverFrameTimer
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a saver frame timer calls back with the time since its previous call",
                 "[screensaver][screensaver_parts]") {
    SaverFrameTimer timer;
    std::vector<uint32_t> dts;
    timer.start(20, [&](uint32_t dt_ms) { dts.push_back(dt_ms); });
    REQUIRE(timer.timer() != nullptr);
    CHECK(timer.timer()->period == 20);

    lv_tick_inc(25);
    fire(timer.timer());
    lv_tick_inc(7);
    fire(timer.timer());

    CHECK(dts == std::vector<uint32_t>{25, 7});
    timer.cancel();
}

TEST_CASE_METHOD(LVGLTestFixture, "changing a saver frame timer's period keeps motion continuous",
                 "[screensaver][screensaver_parts]") {
    SaverFrameTimer timer;
    uint32_t last_dt = 0;
    timer.start(16, [&](uint32_t dt_ms) { last_dt = dt_ms; });
    lv_tick_inc(10);
    timer.set_period(33);
    CHECK(timer.timer()->period == 33);
    lv_tick_inc(15);
    fire(timer.timer());
    CHECK(last_dt == 25);
    timer.cancel();
}

TEST_CASE_METHOD(LVGLTestFixture, "cancelling a saver frame timer neuters it",
                 "[screensaver][screensaver_parts]") {
    SaverFrameTimer timer;
    int calls = 0;
    timer.start(16, [&](uint32_t) { calls++; });
    lv_timer_t* raw = timer.timer();
    REQUIRE(raw != nullptr);

    timer.cancel();

    CHECK(timer.timer() == nullptr);
    // lv_timer_cancel_safe() clears the callback; lv_timer_handler deletes the timer later.
    CHECK(raw->timer_cb == nullptr);
    CHECK(calls == 0);
}

// ============================================================================
// SaverBase
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver starts with its overlay, canvas, seeded random sequence and a 16 ms timer",
                 "[screensaver][screensaver_parts]") {
    // With no period configured, level 0 runs at SAVER_FAST_PERIOD whatever the display refresh.
    ScopedRefreshPeriod refresh(20);
    ScopedGlobalRefreshHold clean_hold;
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    SaverTestAccess::set_fixed_seed(saver, 7);

    saver.start();

    REQUIRE(saver.is_active());
    CHECK(saver.canvas_at_start);
    lv_obj_t* overlay = SaverTestAccess::overlay(saver);
    REQUIRE(overlay != nullptr);
    CHECK(lv_obj_get_parent(overlay) == lv_layer_top());
    CHECK(lv_obj_get_style_bg_opa(overlay, LV_PART_MAIN) == LV_OPA_TRANSP);
    lv_obj_t* canvas = SaverTestAccess::canvas(saver);
    REQUIRE(canvas != nullptr);
    CHECK(lv_obj_get_parent(canvas) == overlay);
    REQUIRE(SaverTestAccess::timer(saver) != nullptr);
    CHECK(SaverTestAccess::timer(saver)->period == helix::ui::SAVER_FAST_PERIOD);
    std::minstd_rand expected(7);
    CHECK(saver.first_random == expected());
    CHECK(saver.calls == std::vector<std::string>{"start"});
}

TEST_CASE_METHOD(LVGLTestFixture, "a saver without a canvas keeps its overlay opaque and invalidates nothing",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    saver.format = std::nullopt;
    saver.frame_dirty = {{1, 1, 2, 2}};

    saver.start();

    REQUIRE(saver.is_active());
    CHECK_FALSE(saver.canvas_at_start);
    CHECK(SaverTestAccess::canvas(saver) == nullptr);
    CHECK(lv_obj_get_style_bg_opa(SaverTestAccess::overlay(saver), LV_PART_MAIN) == LV_OPA_COVER);
    helix::test::InvalidatedAreas invalidated(lv_display_get_default());
    lv_tick_inc(16);
    fire(SaverTestAccess::timer(saver));
    CHECK(saver.frame_dts == std::vector<uint32_t>{16});
    CHECK(invalidated.areas.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a saver that refuses to start leaves nothing running",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    saver.refuse = true;

    saver.start();

    CHECK_FALSE(saver.is_active());
    CHECK(SaverTestAccess::timer(saver) == nullptr);
    CHECK(SaverTestAccess::overlay(saver) == nullptr);
    CHECK(SaverTestAccess::canvas(saver) == nullptr);
    CHECK(SaverTestAccess::draw_buf_size(saver) == 0);
    CHECK(saver.calls == std::vector<std::string>{"start"});
}

TEST_CASE_METHOD(LVGLTestFixture, "a saver frame gets the elapsed time and its dirty areas are invalidated",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    saver.frame_dirty = {{5, 6, 7, 8}};
    saver.start();
    REQUIRE(saver.is_active());
    const lv_area_t coords = coords_of(SaverTestAccess::canvas(saver));
    helix::test::InvalidatedAreas invalidated(lv_display_get_default());

    lv_tick_inc(30);
    fire(SaverTestAccess::timer(saver));

    CHECK(saver.frame_dts == std::vector<uint32_t>{30});
    REQUIRE(invalidated.areas.size() == 1);
    CHECK(invalidated.areas[0].x1 == coords.x1 + 5);
    CHECK(invalidated.areas[0].y1 == coords.y1 + 6);
    CHECK(invalidated.areas[0].x2 == coords.x1 + 7);
    CHECK(invalidated.areas[0].y2 == coords.y1 + 8);
}

TEST_CASE_METHOD(LVGLTestFixture, "a saver starts at its start level, clamped to its ladder",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};

    saver.set_start_level(2);
    saver.start();
    REQUIRE(saver.is_active());
    CHECK(saver.level() == 2);
    CHECK(saver.level_at_start == 2);
    CHECK(SaverTestAccess::timer(saver)->period == 50);
    CHECK(saver.level_count() == 3);
    saver.stop();

    saver.set_start_level(9);
    saver.start();
    REQUIRE(saver.is_active());
    CHECK(saver.level() == 2);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a level request goes through the saver, which moves its timer and the held refresh",
                 "[screensaver][screensaver_parts]") {
    helix::ScopedTimerPeriods restore;
    helix::ScopedTimerPeriods::set(40, 40);
    ScopedGlobalRefreshHold clean_hold;
    helix::RefreshPeriodHold& hold = helix::active_refresh_period_hold();
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};

    saver.request_level(1);
    CHECK(saver.level_requests.empty()); // not running

    hold.acquire();
    saver.start();
    REQUIRE(saver.is_active());
    CHECK(SaverTestAccess::timer(saver)->period == helix::ui::SAVER_FAST_PERIOD);
    CHECK(helix::default_refr_timer_period() == helix::ui::SAVER_FAST_PERIOD);

    saver.request_level(1);
    CHECK(saver.level_requests == std::vector<size_t>{1});
    CHECK(saver.level() == 1);
    CHECK(SaverTestAccess::timer(saver)->period == 45);
    CHECK(helix::default_refr_timer_period() == 45);

    saver.defer_levels = true;
    saver.request_level(2);
    CHECK(saver.level_requests == std::vector<size_t>{1, 2});
    CHECK(saver.level() == 1);
    CHECK(SaverTestAccess::timer(saver)->period == 45);
    saver.apply_level(2);
    CHECK(saver.level() == 2);
    CHECK(SaverTestAccess::timer(saver)->period == 50);
    CHECK(helix::default_refr_timer_period() == 50);
    CHECK(saver.level_period_ms(9) == 50);

    saver.request_level(9);
    CHECK(saver.level_requests.back() == 2);

    saver.stop();
    hold.release();
    CHECK(helix::default_refr_timer_period() == 40);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a configured refresh period replaces level 0's period and no other level's",
                 "[screensaver][screensaver_parts]") {
    helix::ScopedTimerPeriods restore;
    helix::ScopedTimerPeriods::set(40, 40);
    ScopedGlobalRefreshHold clean_hold;
    helix::RefreshPeriodHold& hold = helix::active_refresh_period_hold();
    hold.set_period(20);
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    CHECK(saver.level_period_ms(0) == 20);
    CHECK(saver.level_period_ms(1) == 45);
    CHECK(saver.level_period_ms(2) == 50);

    hold.acquire();
    saver.start();
    REQUIRE(saver.is_active());
    CHECK(SaverTestAccess::timer(saver)->period == 20);
    CHECK(helix::default_refr_timer_period() == 20);

    saver.request_level(1);
    CHECK(SaverTestAccess::timer(saver)->period == 45);
    CHECK(helix::default_refr_timer_period() == 45);

    saver.stop();
    hold.release();
    CHECK(helix::default_refr_timer_period() == 40);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "stopping a saver cancels its timer, hides and frees its canvas, and queues its overlay",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    saver.start();
    REQUIRE(saver.is_active());
    lv_obj_t* canvas = SaverTestAccess::canvas(saver);
    lv_obj_t* overlay = SaverTestAccess::overlay(saver);
    lv_timer_t* timer = SaverTestAccess::timer(saver);

    saver.stop();

    CHECK_FALSE(saver.is_active());
    CHECK(timer->timer_cb == nullptr);
    CHECK(SaverTestAccess::timer(saver) == nullptr);
    CHECK(lv_obj_has_flag(canvas, LV_OBJ_FLAG_HIDDEN));
    CHECK(SaverTestAccess::draw_buf_size(saver) == 0);
    CHECK(SaverTestAccess::overlay(saver) == nullptr);
    CHECK(lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN));
    CHECK(saver.calls == std::vector<std::string>{"start", "stop"});
}

TEST_CASE_METHOD(LVGLTestFixture, "a restarted saver replays its fixed seed",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    SaverTestAccess::set_fixed_seed(saver, 1234);
    saver.start();
    const uint32_t first = saver.first_random;
    saver.stop();
    saver.start();
    CHECK(saver.first_random == first);
}

#endif // HELIX_ENABLE_SCREENSAVER
```

Append to `tests/unit/application/test_refresh_period_hold.cpp`:

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "RefreshPeriodHold without a configured period waits for the saver's and follows it",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    RefreshPeriodHold hold;

    hold.follow(33); // not held: changes nothing
    CHECK(default_refr_timer_period() == 40);

    hold.acquire();
    CHECK(default_refr_timer_period() == 40);
    hold.follow(16);
    CHECK(default_refr_timer_period() == 16);
    CHECK(anim_timer_period() == 16);
    hold.follow(33);
    CHECK(default_refr_timer_period() == 33);
    CHECK(anim_timer_period() == 33);

    hold.release();
    CHECK_FALSE(hold.is_held());
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a held RefreshPeriodHold moves from its configured period to the saver's",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    RefreshPeriodHold hold;
    hold.set_period(20);

    hold.acquire();
    CHECK(default_refr_timer_period() == 20);
    hold.follow(20); // a saver at level 0, which takes the configured period
    CHECK(default_refr_timer_period() == 20);
    hold.follow(33); // the same saver at level 1
    CHECK(default_refr_timer_period() == 33);
    CHECK(anim_timer_period() == 33);

    hold.release();
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);

    // The saver's period ends with the hold: the next acquire starts at the configured one.
    hold.acquire();
    CHECK(default_refr_timer_period() == 20);
    hold.release();
    CHECK(default_refr_timer_period() == 40);
}
```

In `tests/unit/test_screensaver.cpp`, add `#include "screensaver_base.h"` after `#include "refresh_timing_env.h"`, then append before the final `#endif // HELIX_ENABLE_SCREENSAVER`:

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "without HELIX_SCREENSAVER_REFR_PERIOD_MS the display refreshes at the saver's level period",
                 "[screensaver][refresh_period]") {
    auto& mgr = ScreensaverManager::instance();
    helix::ScopedTimerPeriods timers;
    helix::ScopedEnv global{"HELIX_REFR_PERIOD_MS"};
    helix::ScopedEnv scope{"HELIX_REFR_PERIOD_SCOPE"};
    helix::ScopedEnv saver_period{"HELIX_SCREENSAVER_REFR_PERIOD_MS"};
    setenv("HELIX_REFR_PERIOD_MS", "40", 1);
    unsetenv("HELIX_REFR_PERIOD_SCOPE");
    unsetenv("HELIX_SCREENSAVER_REFR_PERIOD_MS");
    helix::apply_refresh_timing(helix::refresh_timing_from_env());
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_refresh_period_hold().is_held());

    mgr.start(ScreensaverType::PIPES_3D);
    REQUIRE(mgr.is_active());
    CHECK(default_refr_timer_period() == helix::ui::SAVER_FAST_PERIOD);
    CHECK(anim_timer_period() == helix::ui::SAVER_FAST_PERIOD);

    auto* pipes =
        static_cast<helix::ui::SaverBase*>(helix::ScreensaverManagerTestAccess::active(mgr));
    pipes->request_level(1);
    CHECK(running_saver_timer_period(ScreensaverType::PIPES_3D) == 33);
    CHECK(default_refr_timer_period() == 33);
    CHECK(anim_timer_period() == 33);

    mgr.stop();
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(default_refr_timer_period() == GLOBAL_PERIOD_MS);
    CHECK(anim_timer_period() == GLOBAL_PERIOD_MS);
}
```

In `tests/unit/test_screensaver_motion.cpp`, replace the test case `"PipesScreensaver ticks at the display refresh period"` with:

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "PipesScreensaver level 0: configured period, else 16 ms, with the refresh equal",
                 "[screensaver][screensaver_motion]") {
    const uint32_t configured_ms = GENERATE(as<uint32_t>{}, 0, 20);
    INFO("configured period " << configured_ms << " ms");
    const LevelZeroPeriods periods = level_zero_periods<PipesScreensaver>(configured_ms);
    CHECK(periods.timer_ms == (configured_ms != 0 ? configured_ms : helix::ui::SAVER_FAST_PERIOD));
    CHECK(periods.refresh_ms == periods.timer_ms);
}
```

- [ ] **Step 2: Run the build and watch it fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; the log names `screensaver_base.h: No such file or directory`.

- [ ] **Step 3: Write the frame header and the merge**

Create `include/screensaver_frame.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file screensaver_frame.h
 * @brief Frame geometry the screensavers share. No LVGL, so simulations use it too.
 */

namespace helix::ui {

/// Frame period, in ms, of every saver's most expensive level: one refresh of a 60 Hz panel.
inline constexpr uint32_t SAVER_FAST_PERIOD = 16;

/**
 * @brief Most areas a saver invalidates in one frame
 *
 * LVGL keeps pending invalid areas in a fixed buffer (LV_INV_BUF_SIZE in
 * lvgl/src/display/lv_display_private.h) and invalidates the whole screen once it overflows,
 * so a frame with more areas than that costs a full redraw.
 */
inline constexpr size_t SAVER_MAX_DIRTY_AREAS = 32;

/// Inclusive pixel bounds of what changed in a frame. Empty until something is added.
struct DirtyRect {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = -1;
    int32_t y2 = -1;

    bool empty() const {
        return x2 < x1 || y2 < y1;
    }

    /// Pixels covered, 0 when empty.
    int64_t area() const {
        return empty() ? 0 : static_cast<int64_t>(x2 - x1 + 1) * (y2 - y1 + 1);
    }

    /// Grows to cover the inclusive box (ax1, ay1)-(ax2, ay2). An empty box adds nothing.
    void add(int32_t ax1, int32_t ay1, int32_t ax2, int32_t ay2) {
        if (ax2 < ax1 || ay2 < ay1) {
            return;
        }
        if (empty()) {
            *this = {ax1, ay1, ax2, ay2};
            return;
        }
        x1 = std::min(x1, ax1);
        y1 = std::min(y1, ay1);
        x2 = std::max(x2, ax2);
        y2 = std::max(y2, ay2);
    }

    void add(const DirtyRect& other) {
        add(other.x1, other.y1, other.x2, other.y2);
    }

    bool operator==(const DirtyRect& o) const {
        return x1 == o.x1 && y1 == o.y1 && x2 == o.x2 && y2 == o.y2;
    }
};

/**
 * @brief Reduces `areas` to at most `max_areas` boxes that together cover every input box
 *
 * Empty boxes are dropped, and within the limit the rest keep their order. Past it, the pair
 * whose bounding box adds the fewest pixels over the two boxes is joined, until the limit
 * holds. A `max_areas` of 0 is read as 1.
 */
void merge_dirty_areas(std::vector<DirtyRect>& areas, size_t max_areas);

} // namespace helix::ui
```

Create `src/ui/screensaver_frame.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_frame.h"

#include <cstddef>
#include <limits>

namespace helix::ui {

void merge_dirty_areas(std::vector<DirtyRect>& areas, size_t max_areas) {
    areas.erase(std::remove_if(areas.begin(), areas.end(),
                               [](const DirtyRect& r) { return r.empty(); }),
                areas.end());
    const size_t limit = std::max<size_t>(max_areas, 1);
    while (areas.size() > limit) {
        size_t best_a = 0;
        size_t best_b = 1;
        int64_t best_cost = std::numeric_limits<int64_t>::max();
        for (size_t a = 0; a < areas.size(); a++) {
            for (size_t b = a + 1; b < areas.size(); b++) {
                DirtyRect joined = areas[a];
                joined.add(areas[b]);
                const int64_t cost = joined.area() - areas[a].area() - areas[b].area();
                if (cost < best_cost) {
                    best_cost = cost;
                    best_a = a;
                    best_b = b;
                }
            }
        }
        areas[best_a].add(areas[best_b]);
        areas.erase(areas.begin() + static_cast<std::ptrdiff_t>(best_b));
    }
}

} // namespace helix::ui
```

In `include/screensaver_starfield_sim.h`, delete the whole `struct DirtyRect { ... };` definition together with its `/// Inclusive pixel bounds ...` comment, and add `#include "screensaver_frame.h"` above `#include <algorithm>`.

- [ ] **Step 4: Write SaverOverlay**

Create `include/screensaver_overlay.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include <lvgl.h>

namespace helix::ui {

/**
 * @brief The black, touch-absorbing full-screen overlay a screensaver shows on lv_layer_top()
 *
 * Opaque until an opaque canvas covers it: top-layer children are never cover-culled, so an
 * opaque background under a full-screen canvas would be filled every frame.
 */
class SaverOverlay {
  public:
    SaverOverlay() = default;
    SaverOverlay(const SaverOverlay&) = delete;
    SaverOverlay& operator=(const SaverOverlay&) = delete;

    /// Creates the opaque black overlay. Does nothing while one exists.
    void create();

    /// Stops filling the background, for when an opaque canvas covers the whole overlay.
    void make_transparent();

    /// Hides the overlay and deletes it with its children on a later timer pass.
    void destroy();

    lv_obj_t* obj() const {
        return obj_;
    }

  private:
    lv_obj_t* obj_ = nullptr;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

Create `src/ui/screensaver_overlay.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_overlay.h"

#include "ui_utils.h"

namespace helix::ui {

void SaverOverlay::create() {
    if (obj_) {
        return;
    }
    obj_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(obj_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(obj_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj_, 0, 0);
    lv_obj_set_style_pad_all(obj_, 0, 0);
    lv_obj_set_style_radius(obj_, 0, 0);
    // Clickable, so the waking touch lands here rather than on the panel beneath; LVGL still
    // counts it as activity.
    lv_obj_add_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
}

void SaverOverlay::make_transparent() {
    if (obj_) {
        lv_obj_set_style_bg_opa(obj_, LV_OPA_TRANSP, 0);
    }
}

void SaverOverlay::destroy() {
    // A saver stops from the main loop's wake path, next to lv_timer_handler ticks, where a
    // synchronous delete of an overlay with children corrupts LVGL's event list (#316).
    safe_delete_deferred(obj_);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 5: Write SaverCanvas**

Create `include/screensaver_canvas.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_frame.h"

#include <cstddef>
#include <cstdint>
#include <lvgl.h>
#include <vector>

namespace helix::ui {

/**
 * @brief A full-screen canvas a screensaver draws on
 *
 * Owns the draw buffer, allocated at the row stride lv_canvas_set_buffer() uses, so direct
 * pixel writes land where the canvas reads them. A scene drawn with lv_draw_* opens a layer
 * session and marks what it draws; a scene written pixel by pixel passes its dirty areas to
 * invalidate(). Either way only the changed areas are redrawn.
 */
class SaverCanvas {
  public:
    SaverCanvas() = default;
    SaverCanvas(const SaverCanvas&) = delete;
    SaverCanvas& operator=(const SaverCanvas&) = delete;

    /**
     * @brief Creates a w x h canvas in format `cf` as a child of `parent`, painted opaque black
     * @return false when the buffer cannot be allocated; nothing is created then
     */
    bool create(lv_obj_t* parent, int32_t w, int32_t h, lv_color_format_t cf);

    /// Hides the canvas, then frees its buffer. The canvas object is deleted with its parent.
    void release();

    lv_obj_t* obj() const {
        return canvas_;
    }
    uint8_t* data() const {
        return buf_;
    }
    uint32_t stride() const {
        return stride_;
    }
    size_t buffer_size() const {
        return buf_size_;
    }
    lv_color_format_t format() const {
        return cf_;
    }
    int32_t width() const {
        return w_;
    }
    int32_t height() const {
        return h_;
    }

    /// Paints the whole canvas opaque black and invalidates all of it.
    void fill_black();

    /// Invalidates `areas`, in canvas pixels, after merging them to at most SAVER_MAX_DIRTY_AREAS.
    void invalidate(std::vector<DirtyRect>& areas);

    /// Opens a layer session for lv_draw_* calls on the canvas.
    void begin_layer(lv_layer_t* layer);

    /// Records a canvas area (inclusive) the open layer session draws into.
    void mark_dirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

    /// Finishes the layer session and invalidates only the areas marked since begin_layer().
    void finish_layer(lv_layer_t* layer);

  private:
    lv_obj_t* canvas_ = nullptr;
    uint8_t* buf_ = nullptr;
    size_t buf_size_ = 0;
    uint32_t stride_ = 0;
    int32_t w_ = 0;
    int32_t h_ = 0;
    lv_color_format_t cf_ = LV_COLOR_FORMAT_UNKNOWN;
    std::vector<DirtyRect> layer_dirty_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

Create `src/ui/screensaver_canvas.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_canvas.h"

#include "lvgl/src/display/lv_display_private.h" // LV_INV_BUF_SIZE
#include "screensaver.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

static_assert(SAVER_MAX_DIRTY_AREAS <= LV_INV_BUF_SIZE,
              "a saver frame's areas must fit LVGL's invalid-area buffer");

bool SaverCanvas::create(lv_obj_t* parent, int32_t w, int32_t h, lv_color_format_t cf) {
    // lv_canvas_set_buffer() steps rows by the aligned stride and lv_canvas_fill_bg() writes the
    // whole extent at once, so a tightly packed w * h * bytes-per-pixel buffer would under-run it.
    const uint32_t stride = screensaver_canvas_stride_bytes(w, cf);
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(h);
    auto* buf = static_cast<uint8_t*>(lv_malloc(size));
    if (!buf) {
        spdlog::error("[Screensaver] Failed to allocate a {}KB canvas buffer", size / 1024);
        return false;
    }
    buf_ = buf;
    buf_size_ = size;
    stride_ = stride;
    w_ = w;
    h_ = h;
    cf_ = cf;

    canvas_ = lv_canvas_create(parent);
    lv_obj_set_size(canvas_, w, h);
    lv_obj_set_pos(canvas_, 0, 0);
    lv_canvas_set_buffer(canvas_, buf_, w, h, cf);
    fill_black();
    return true;
}

void SaverCanvas::release() {
    // The canvas object lives until its parent's deferred deletion. Hidden first, no refresh or
    // snapshot before then draws from the buffer freed here.
    if (canvas_) {
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
        canvas_ = nullptr;
    }
    if (buf_) {
        lv_free(buf_);
        buf_ = nullptr;
    }
    buf_size_ = 0;
    stride_ = 0;
    w_ = 0;
    h_ = 0;
    cf_ = LV_COLOR_FORMAT_UNKNOWN;
    layer_dirty_.clear();
}

void SaverCanvas::fill_black() {
    if (canvas_) {
        lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);
    }
}

void SaverCanvas::invalidate(std::vector<DirtyRect>& areas) {
    if (!canvas_) {
        return;
    }
    merge_dirty_areas(areas, SAVER_MAX_DIRTY_AREAS);
    lv_area_t coords;
    lv_obj_get_coords(canvas_, &coords);
    for (const DirtyRect& r : areas) {
        lv_area_t area = {r.x1, r.y1, r.x2, r.y2};
        lv_area_move(&area, coords.x1, coords.y1);
        lv_obj_invalidate_area(canvas_, &area);
    }
}

void SaverCanvas::begin_layer(lv_layer_t* layer) {
    lv_canvas_init_layer(canvas_, layer);
}

void SaverCanvas::mark_dirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    layer_dirty_.push_back({x1, y1, x2, y2});
}

void SaverCanvas::finish_layer(lv_layer_t* layer) {
    // The body of lv_canvas_finish_layer(), which ends by invalidating the whole canvas where a
    // scene step changes a few patches of it. Suppressing invalidation around
    // lv_canvas_finish_layer() is no substitute: the display's enable count is shared, and while
    // it is above 1 a single disable leaves invalidation on.
    if (layer->draw_task_head) {
        layer->all_tasks_added = true;
        lv_display_t* disp = lv_obj_get_display(canvas_);
        while (layer->draw_task_head) {
            lv_draw_dispatch_wait_for_request();
            if (!lv_draw_dispatch_layer(disp, layer)) {
                lv_draw_wait_for_finish();
                lv_draw_dispatch_request();
            }
        }
        lv_draw_unit_send_event(nullptr, LV_EVENT_SCREEN_LOAD_START, layer);
    }
    lv_draw_unit_send_event(nullptr, LV_EVENT_CHILD_DELETED, layer);

    invalidate(layer_dirty_);
    layer_dirty_.clear();
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 6: Write SaverFrameTimer**

Create `include/screensaver_frame_timer.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_motion.h"

#include <cstdint>
#include <functional>
#include <lvgl.h>

namespace helix::ui {

/**
 * @brief A screensaver's frame timer
 *
 * Calls back with the time since the previous call, read from lv_tick_get() through a
 * MotionClock, so motion stays a function of elapsed time whatever the period.
 */
class SaverFrameTimer {
  public:
    using FrameFn = std::function<void(uint32_t dt_ms)>;

    SaverFrameTimer() = default;
    SaverFrameTimer(const SaverFrameTimer&) = delete;
    SaverFrameTimer& operator=(const SaverFrameTimer&) = delete;

    /// Cancels the timer, so an owner destroyed while running never leaves it armed on freed memory.
    ~SaverFrameTimer();

    /// Starts calling `on_frame` every `period_ms`. A running timer is cancelled first.
    void start(uint32_t period_ms, FrameFn on_frame);

    /// Changes the period of a running timer without resetting the clock, so the next call
    /// still gets the whole time since the previous one.
    void set_period(uint32_t period_ms);

    /// Stops the callbacks. Safe from inside lv_timer_handler and after lv_deinit().
    void cancel();

    lv_timer_t* timer() const {
        return timer_;
    }

  private:
    static void timer_cb(lv_timer_t* timer);

    lv_timer_t* timer_ = nullptr;
    screensaver::MotionClock clock_;
    FrameFn on_frame_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

Create `src/ui/screensaver_frame_timer.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_frame_timer.h"

#include "ui_timer_guard.h" // lv_timer_cancel_safe

#include <utility>

namespace helix::ui {

SaverFrameTimer::~SaverFrameTimer() {
    cancel();
}

void SaverFrameTimer::start(uint32_t period_ms, FrameFn on_frame) {
    cancel();
    on_frame_ = std::move(on_frame);
    clock_.reset(lv_tick_get());
    timer_ = lv_timer_create(timer_cb, period_ms, this);
}

void SaverFrameTimer::set_period(uint32_t period_ms) {
    if (timer_) {
        lv_timer_set_period(timer_, period_ms);
    }
}

void SaverFrameTimer::cancel() {
    if (timer_) {
        // Neuters instead of unlinking and guards on lv_is_initialized(), which is what makes it
        // safe from the destructor, from inside lv_timer_handler and after lv_deinit (#750, #1173).
        lv_timer_cancel_safe(timer_);
        timer_ = nullptr;
    }
}

void SaverFrameTimer::timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<SaverFrameTimer*>(lv_timer_get_user_data(timer));
    if (!self || !self->on_frame_) {
        return;
    }
    self->on_frame_(self->clock_.advance(lv_tick_get()));
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 7: Write SaverBase and the generic test access**

Create `include/screensaver_base.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_canvas.h"
#include "screensaver_frame.h"
#include "screensaver_frame_timer.h"
#include "screensaver_overlay.h"

#include <cstddef>
#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <random>
#include <vector>

namespace helix::ui {

class SaverTestAccess;

/// Levels on the starfield and pipes ladder.
inline constexpr size_t TWO_LEVEL_COUNT = 2;

/// Frame period of `level` on that ladder: 16 ms, then 33 ms.
constexpr uint32_t two_level_period_ms(size_t level) {
    return level == 0 ? SAVER_FAST_PERIOD : 33;
}

/**
 * @brief Sequences the parts every screensaver runs on
 *
 * start() creates the overlay, the canvas when the saver has one, seeds the random sequence,
 * sets the start level, calls on_start() and starts the frame timer at the level's period.
 * Each frame calls on_frame() and invalidates the dirty areas it returns. stop() cancels the
 * timer, hides and frees the canvas, queues the overlay for deletion and calls on_stop().
 * A saver implements the on_* hooks and its ladder, most expensive level first. A level change
 * also moves the display refresh while ScreensaverManager holds it (RefreshPeriodHold::follow).
 */
class SaverBase : public Screensaver {
  public:
    void start() final;
    void stop() final;
    bool is_active() const final {
        return active_;
    }

    /// Level the saver runs at.
    size_t level() const {
        return level_;
    }
    size_t level_count() const {
        return ladder_size();
    }

    /// Frame period, in ms, of `level`, clamped to the ladder. Level 0 runs at
    /// HELIX_SCREENSAVER_REFR_PERIOD_MS (RefreshPeriodHold::period()) when one is configured.
    uint32_t level_period_ms(size_t level) const;

    /// Level the next start() runs at, clamped to the ladder.
    void set_start_level(size_t level);

    /// Asks a running saver to run at `level`, clamped to the ladder. Ignored while stopped.
    void request_level(size_t level);

  protected:
    SaverBase() = default;

    /// Canvas format the saver draws in, or nullopt for a saver without a canvas.
    virtual std::optional<lv_color_format_t> canvas_format() const = 0;
    /// Sets up the scene. The overlay, the canvas, the seeded random sequence and level() are
    /// ready. Returning false refuses to start and releases everything.
    virtual bool on_start() = 0;
    /// Advances the scene by `dt_ms` and adds the canvas areas it changed to `dirty` (empty on entry).
    virtual void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) = 0;
    /// Releases scene state once the timer, canvas and overlay are gone.
    virtual void on_stop() = 0;
    /// Number of quality levels, at least 1.
    virtual size_t ladder_size() const = 0;
    /// Frame period, in ms, of `level` (below ladder_size()).
    virtual uint32_t ladder_period_ms(size_t level) const = 0;
    /// A level request for the running saver. The default applies it at once, which suits
    /// motion that is a function of time; a saver with a natural break calls apply_level() there.
    virtual void on_level_request(size_t level);

    /// Runs at `level` (clamped) from now on: sets level(), the frame timer's period and, while
    /// the display refresh is held, the refresh period.
    void apply_level(size_t level);

    SaverOverlay& overlay() {
        return overlay_;
    }
    SaverCanvas& canvas() {
        return canvas_;
    }
    std::minstd_rand& rng() {
        return rng_;
    }
    int32_t screen_w() const {
        return screen_w_;
    }
    int32_t screen_h() const {
        return screen_h_;
    }

  private:
    friend class SaverTestAccess;

    void run_frame(uint32_t dt_ms);

    bool active_ = false;
    size_t level_ = 0;
    size_t start_level_ = 0;
    int32_t screen_w_ = 0;
    int32_t screen_h_ = 0;
    SaverOverlay overlay_;
    SaverCanvas canvas_;
    SaverFrameTimer timer_;
    std::vector<DirtyRect> dirty_;
    // Owned random sequence, seeded in start(); a fixed seed makes a run replay exactly.
    std::minstd_rand rng_;
    std::optional<uint32_t> fixed_seed_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

Create `src/ui/screensaver_base.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"

#include "refresh_period_hold.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>

namespace helix::ui {

void SaverBase::start() {
    if (active_) {
        spdlog::debug("[Screensaver] Type {} already active, ignoring start()",
                      static_cast<int>(type()));
        return;
    }
    lv_display_t* disp = lv_display_get_default();
    if (!disp) {
        spdlog::warn("[Screensaver] No display available, cannot start type {}",
                     static_cast<int>(type()));
        return;
    }
    screen_w_ = lv_display_get_horizontal_resolution(disp);
    screen_h_ = lv_display_get_vertical_resolution(disp);

    overlay_.create();
    if (const std::optional<lv_color_format_t> cf = canvas_format()) {
        if (!canvas_.create(overlay_.obj(), screen_w_, screen_h_, *cf)) {
            overlay_.destroy();
            return;
        }
        // The opaque canvas covers the whole overlay.
        overlay_.make_transparent();
    }

    rng_.seed(fixed_seed_.value_or(static_cast<uint32_t>(time(nullptr))));
    level_ = start_level_;
    if (!on_start()) {
        spdlog::warn("[Screensaver] Type {} refused to start", static_cast<int>(type()));
        canvas_.release();
        overlay_.destroy();
        return;
    }

    const uint32_t period_ms = level_period_ms(level_);
    timer_.start(period_ms, [this](uint32_t dt_ms) { run_frame(dt_ms); });
    // The display refreshes as often as the saver draws.
    helix::active_refresh_period_hold().follow(period_ms);
    active_ = true;
    spdlog::info("[Screensaver] Type {} running at level {} ({} ms frames, {}x{})",
                 static_cast<int>(type()), level_, period_ms, screen_w_, screen_h_);
}

void SaverBase::stop() {
    if (!active_) {
        return;
    }
    spdlog::info("[Screensaver] Stopping type {}", static_cast<int>(type()));
    timer_.cancel();
    canvas_.release();
    overlay_.destroy();
    on_stop();
    active_ = false;
}

void SaverBase::set_start_level(size_t level) {
    start_level_ = std::min(level, ladder_size() - 1);
}

void SaverBase::request_level(size_t level) {
    if (!active_) {
        return;
    }
    on_level_request(std::min(level, ladder_size() - 1));
}

void SaverBase::on_level_request(size_t level) {
    apply_level(level);
}

void SaverBase::apply_level(size_t level) {
    level_ = std::min(level, ladder_size() - 1);
    const uint32_t period_ms = level_period_ms(level_);
    timer_.set_period(period_ms);
    helix::active_refresh_period_hold().follow(period_ms);
}

uint32_t SaverBase::level_period_ms(size_t level) const {
    const size_t clamped = std::min(level, ladder_size() - 1);
    // HELIX_SCREENSAVER_REFR_PERIOD_MS replaces level 0's period for manual testing.
    const uint32_t configured_ms = helix::active_refresh_period_hold().period();
    return clamped == 0 && configured_ms != 0 ? configured_ms : ladder_period_ms(clamped);
}

void SaverBase::run_frame(uint32_t dt_ms) {
    if (!active_) {
        return;
    }
    dirty_.clear();
    on_frame(dt_ms, dirty_);
    if (!dirty_.empty()) {
        canvas_.invalidate(dirty_);
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

In `tests/test_helpers/screensaver_test_access.h`, add `#include "refresh_period_hold_test_access.h"` and `#include "screensaver_base.h"` next to `#include "screensaver_pipes.h"`, and insert directly after the closing `} // namespace helix::test`:

```cpp
namespace helix::ui {

/// Reaches the parts every saver keeps in SaverBase.
class SaverTestAccess {
  public:
    static lv_obj_t* overlay(const SaverBase& saver) {
        return saver.overlay_.obj();
    }
    static lv_obj_t* canvas(const SaverBase& saver) {
        return saver.canvas_.obj();
    }
    static lv_timer_t* timer(const SaverBase& saver) {
        return saver.timer_.timer();
    }
    static size_t draw_buf_size(const SaverBase& saver) {
        return saver.canvas_.buffer_size();
    }
    static uint32_t draw_buf_stride(const SaverBase& saver) {
        return saver.canvas_.stride();
    }
    static void set_fixed_seed(SaverBase& saver, uint32_t seed) {
        saver.fixed_seed_ = seed;
    }
};

/// Leaves the global refresh hold released with no configured period, on entry and however
/// the test exits: the configured period is level 0's frame period for every saver started.
struct ScopedGlobalRefreshHold {
    ScopedGlobalRefreshHold() {
        helix::RefreshPeriodHoldTestAccess::reset(helix::active_refresh_period_hold());
    }
    ~ScopedGlobalRefreshHold() {
        helix::RefreshPeriodHoldTestAccess::reset(helix::active_refresh_period_hold());
    }
    ScopedGlobalRefreshHold(const ScopedGlobalRefreshHold&) = delete;
    ScopedGlobalRefreshHold& operator=(const ScopedGlobalRefreshHold&) = delete;
};

/// Level 0's frame timer period and the held display refresh period of one saver.
struct LevelZeroPeriods {
    uint32_t timer_ms = 0;
    uint32_t refresh_ms = 0;
};

/// Starts a fresh `Saver` while the global refresh hold is out, as ScreensaverManager runs
/// one, with `configured_ms` as HELIX_SCREENSAVER_REFR_PERIOD_MS (0 for unset), and reads
/// its level 0 periods.
template <typename Saver> LevelZeroPeriods level_zero_periods(uint32_t configured_ms) {
    helix::ScopedTimerPeriods restore;
    helix::ScopedTimerPeriods::set(40, 40); // unlike 16, 20 and 33
    ScopedGlobalRefreshHold clean_hold;
    helix::RefreshPeriodHold& hold = helix::active_refresh_period_hold();
    hold.set_period(configured_ms);
    hold.acquire();
    LevelZeroPeriods periods;
    Saver saver;
    ScreensaverStopOnExit<Saver> stop_on_exit{saver};
    saver.start();
    if (saver.is_active() && SaverTestAccess::timer(saver) != nullptr) {
        periods.timer_ms = SaverTestAccess::timer(saver)->period;
    }
    periods.refresh_ms = helix::default_refr_timer_period();
    saver.stop();
    hold.release();
    return periods;
}

} // namespace helix::ui

using helix::ui::level_zero_periods;
using helix::ui::LevelZeroPeriods;
using helix::ui::SaverTestAccess;
using helix::ui::ScopedGlobalRefreshHold;
```

Then let the display refresh follow the running saver's level. In `include/refresh_period_hold.h`, in the class comment replace

```cpp
 * 30 fps on a 60 Hz panel. While this hold is out, the default display's refresh timer
 * and the animation timer run at period() instead. The refresh timer pauses itself
 * whenever nothing is invalidated, so a shorter period costs nothing on a still frame.
 *
 * - The first acquire() records both timers' periods and sets them to period(). With no
 *   period configured, no default display, or no refresh timer, it changes nothing.
```

with

```cpp
 * 30 fps on a 60 Hz panel. While this hold is out, the default display's refresh timer
 * and the animation timer run at period() instead, and at the running saver's frame period
 * once the saver gives one. The refresh timer pauses itself whenever nothing is
 * invalidated, so a shorter period costs nothing on a still frame.
 *
 * - The first acquire() records both timers' periods and sets them to period(). With no
 *   period configured, no default display, or no refresh timer, it changes nothing.
 * - follow() runs both timers at the saver's frame period from then on, taking them if
 *   acquire() did not. SaverBase gives period() as level 0's frame period when one is
 *   configured, so the saver and the display refresh stay equal at every level.
```

add this declaration directly after `void acquire();`:

```cpp
    /// While held, both timers run at `saver_period_ms`, the running saver's frame period,
    /// until the final release(). Not held, or given 0, it does nothing.
    void follow(uint32_t saver_period_ms);
```

and in the private section replace `    uint32_t m_period_ms = 0;` with:

```cpp
    uint32_t m_period_ms = 0;
    uint32_t m_saver_period_ms = 0; ///< the running saver's frame period while held, or 0

    uint32_t effective_period() const {
        return m_saver_period_ms != 0 ? m_saver_period_ms : m_period_ms;
    }
```

In `src/application/display_manager.cpp`, add `RefreshPeriodHold::follow` after `RefreshPeriodHold::acquire()` (which is unchanged), and replace `RefreshPeriodHold::take_timers()` and `RefreshPeriodHold::release()`, with:

```cpp
void RefreshPeriodHold::follow(uint32_t saver_period_ms) {
    if (m_count == 0 || saver_period_ms == 0) {
        return;
    }
    m_saver_period_ms = saver_period_ms;
    if (!lv_is_initialized()) {
        return;
    }
    if (m_display == nullptr) {
        // acquire() had no period to run at and left the timers alone.
        take_timers();
        return;
    }
    if (display_is_live(m_display)) {
        if (lv_timer_t* refr = lv_display_get_refr_timer(m_display)) {
            lv_timer_set_period(refr, saver_period_ms);
        }
    }
    if (m_saved_anim) {
        if (lv_timer_t* anim = lv_anim_get_timer()) {
            lv_timer_set_period(anim, saver_period_ms);
        }
    }
}

void RefreshPeriodHold::take_timers() {
    const uint32_t period_ms = effective_period();
    if (period_ms == 0 || !lv_is_initialized()) {
        return;
    }
    lv_display_t* disp = lv_display_get_default();
    lv_timer_t* refr = disp != nullptr ? lv_display_get_refr_timer(disp) : nullptr;
    if (refr == nullptr) {
        return;
    }
    m_display = disp;
    m_saved_refr_period_ms = refr->period;
    lv_timer_set_period(refr, period_ms);
    if (lv_timer_t* anim = lv_anim_get_timer()) {
        m_saved_anim_period_ms = anim->period;
        m_saved_anim = true;
        lv_timer_set_period(anim, period_ms);
    }
    spdlog::debug("[RefreshPeriodHold] Refresh period {} ms -> {} ms", m_saved_refr_period_ms,
                  period_ms);
}

void RefreshPeriodHold::release() {
    if (m_count == 0 || --m_count > 0) {
        return;
    }
    restore_timers();
    m_saver_period_ms = 0;
}
```

In `tests/test_helpers/refresh_period_hold_test_access.h#RefreshPeriodHoldTestAccess::reset`, add `hold.m_saver_period_ms = 0;` after `hold.m_period_ms = 0;`.

In `include/screensaver.h#ScreensaverManager::start`, replace the doc lines

```cpp
     * For the same span the display refreshes at HELIX_SCREENSAVER_REFR_PERIOD_MS when
     * one is configured (helix::RefreshPeriodHold), set before the saver starts so the
     * saver's own timer follows it.
```

with

```cpp
     * For the same span the display refreshes at the running saver's frame period,
     * following its level; level 0 runs at HELIX_SCREENSAVER_REFR_PERIOD_MS when one is
     * configured (helix::RefreshPeriodHold).
```

In `docs/devel/ENVIRONMENT_VARIABLES.md`, section `HELIX_SCREENSAVER_REFR_PERIOD_MS`, replace the first paragraph's first two sentences (`Display refresh and animation period while a screensaver runs. Set before the saver starts, so the saver's own tick timer, which follows the refresh period, runs at it too.`) with `Level 0 frame period of every screensaver, for manual testing: the saver draws at it, and the display refreshes and animates at it. Without it, level 0 runs at 16 ms. Levels 1 and up keep their own periods (33 ms and slower) either way, and while a saver runs the display refresh follows it to them.`, replace the table row `| **Default** | Unset: savers run at the global period |` with `| **Default** | Unset: level 0 runs at 16 ms |`, and the row `| **File** | \`include/refresh_period_hold.h\`, \`src/ui/screensaver_manager.cpp\` |` with `| **File** | \`include/refresh_period_hold.h\`, \`src/ui/screensaver_manager.cpp\`, \`src/ui/screensaver_base.cpp\` |`.

- [ ] **Step 8: Wire the build**

In `Makefile`, replace the block

```make
# Exclude screensaver when not enabled
ifneq ($(ENABLE_SCREENSAVER),yes)
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/ui_screensaver.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/screensaver_manager.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/screensaver_starfield.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/screensaver_starfield_sim.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/screensaver_pipes.cpp,$(APP_SRCS))
endif
```

with

```make
# Screensaver sources: every saver, the parts they share and the manager
SCREENSAVER_SRCS := $(SRC_DIR)/ui/ui_screensaver.cpp $(wildcard $(SRC_DIR)/ui/screensaver_*.cpp)
# Exclude screensaver when not enabled
ifneq ($(ENABLE_SCREENSAVER),yes)
    APP_SRCS := $(filter-out $(SCREENSAVER_SRCS),$(APP_SRCS))
endif
```

In `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`, replace the four lines from `src/ui/screensaver_manager.cpp  # not in the v1 Core+AMS cut` through `src/ui/screensaver_starfield_sim.cpp  # not in the v1 Core+AMS cut` with:

```
src/ui/screensaver_base.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_canvas.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_frame.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_frame_timer.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_manager.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_overlay.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_pipes.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_starfield.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_starfield_sim.cpp  # not in the v1 Core+AMS cut
```

Run: `python3 scripts/check_esp32_app_srcs.py; echo "exit $?"` and `make -n ENABLE_SCREENSAVER=no > "$SS_SCRATCH/dry-nosaver.log" 2>&1; grep -c 'screensaver_' "$SS_SCRATCH/dry-nosaver.log"`
Expected: `exit 0`; the count is `0`.

- [ ] **Step 9: Build and run the shared-part tests**

Run: `scripts/syntax_check.py src/ui/screensaver_frame.cpp src/ui/screensaver_overlay.cpp src/ui/screensaver_canvas.cpp src/ui/screensaver_frame_timer.cpp src/ui/screensaver_base.cpp` then `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver_parts]"`.
Expected: syntax check clean; `exit 0`; every `[screensaver_parts]` case passes. Then `./build/bin/helix-tests "[refresh_period]"`: the two new hold cases and every existing refresh-period case pass.

- [ ] **Step 10: Move pipes onto the base**

Replace `include/screensaver_pipes.h` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_motion.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

/**
 * @brief Windows-style 3D Pipes screensaver
 *
 * Pipes grow through a 3D grid rendered with perspective projection.
 * Multiple pipes grow simultaneously, each with a different color.
 * Ball joints appear at direction changes. Camera angle randomizes on reset.
 * Ported from https://github.com/1j01/pipes
 */
class PipesScreensaver : public helix::ui::SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::PIPES_3D;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::PIPES_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return helix::ui::TWO_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return helix::ui::two_level_period_ms(level);
    }

  private:
    // Test-only seam: reads and sets the grid and pipe state. See
    // tests/test_helpers/screensaver_test_access.h.
    friend class PipesScreensaverTestAccess;

    // Grid: 21x21x21 centered at origin (-10..+10), matching reference
    static constexpr int GRID_DIM = 21;
    static constexpr int GRID_OFFSET = 10;
    static constexpr int MAX_SEGMENTS = 500;
    static constexpr int MAX_ACTIVE_PIPES = 3;

    enum class Direction { POS_X = 0, NEG_X, POS_Y, NEG_Y, POS_Z, NEG_Z };

    struct GridPos {
        int x = 0, y = 0, z = 0;
    };

    struct ActivePipe {
        GridPos pos;
        Direction dir{};
        lv_color_t color{};
        lv_color_t shadow_color{};
        lv_color_t highlight_color{};
        int segment_count = 0;
        bool alive = false;
        bool has_prev_dir = false;
    };

    void reset_grid();
    void setup_camera();
    void start_new_pipe(ActivePipe& pipe);
    bool grow_pipe(ActivePipe& pipe, lv_layer_t* layer);
    bool project(float wx, float wy, float wz, int& sx, int& sy, float& depth) const;
    void draw_segment(lv_layer_t* layer, int sx1, int sy1, int sx2, int sy2, float depth,
                      const ActivePipe& pipe);
    void draw_joint(lv_layer_t* layer, int sx, int sy, float depth, const ActivePipe& pipe);
    GridPos next_pos(GridPos pos, Direction dir) const;
    bool in_bounds(GridPos pos) const;

    /// Grows every alive pipe one grid step, and starts a pipe when one has died
    void grow_step();

    // Grid occupancy
    bool grid_[GRID_DIM][GRID_DIM][GRID_DIM]{};

    // Multiple active pipes
    ActivePipe pipes_[MAX_ACTIVE_PIPES]{};
    int color_index_ = 0;
    int total_segments_ = 0;

    helix::ui::screensaver::StepAccumulator steps_;

    // Perspective camera (precomputed basis vectors)
    float cam_pos_[3]{};
    float cam_right_[3]{};
    float cam_up_[3]{};
    float cam_fwd_[3]{};
    float focal_ = 0;
};

#endif // HELIX_ENABLE_SCREENSAVER
```

Save this script as `$SS_SCRATCH/pipes_onto_base.py` and run `python3 "$SS_SCRATCH/pipes_onto_base.py"`; every assertion must hold, or the source moved and the edit stops before writing:

```python
import re
from pathlib import Path

path = Path("src/ui/screensaver_pipes.cpp")
text = path.read_text()


def replace_once(old, new):
    global text
    assert text.count(old) == 1, f"expected one {old[:50]!r}, found {text.count(old)}"
    text = text.replace(old, new)


def replace_between(start, end, new):
    global text
    assert text.count(start) == 1 and text.count(end) == 1, (start, end)
    a = text.index(start)
    b = text.index(end, a)
    text = text[:a] + new + text[b:]


def sub_expect(pattern, repl, expected):
    global text
    text, count = re.subn(pattern, repl, text)
    assert count == expected, f"{pattern}: replaced {count}, expected {expected}"


LIFECYCLE = """// ---------- Lifecycle ----------

bool PipesScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting 3D pipes");

    setup_camera();
    reset_grid();

    // Start 2 pipes initially (reference starts 1-3)
    color_index_ = 0;
    for (int i = 0; i < 2; i++) {
        start_new_pipe(pipes_[i]);
    }
    steps_.reset();

    spdlog::debug("[Screensaver] Pipes started ({}x{}, perspective camera)", screen_w_, screen_h_);
    return true;
}

void PipesScreensaver::on_stop() {
    for (auto& p : pipes_)
        p.alive = false;
}

"""

FRAME = """// ---------- Frame ----------

// Pipes draws in layer sessions, which invalidate the areas they mark, so a frame adds nothing
// to the dirty list.
void PipesScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& /*dirty*/) {
    const uint32_t steps = steps_.steps_due(dt_ms, STEP_MS, MAX_STEPS_PER_TICK);
    for (uint32_t step = 0; step < steps; step++) {
        // Reset when grid is full. Steps still due belong to the finished scene, and the
        // new scene's first step comes a full step later.
        if (total_segments_ > MAX_SEGMENTS) {
            canvas().fill_black();
            reset_grid();
            setup_camera(); // New random camera angle on reset
            for (auto& p : pipes_)
                p.alive = false;
            for (int i = 0; i < 2; i++) {
                start_new_pipe(pipes_[i]);
            }
            steps_.reset();
            return;
        }
        grow_step();
    }
}

"""

replace_once('#include "ui_timer_guard.h" // lv_timer_cancel_safe\n#include "ui_utils.h"\n\n', "")
replace_once("#include <ctime>\n", "")
replace_once("using helix::ui::PIPES_CANVAS_FORMAT;\n", "using helix::ui::DirtyRect;\n")
replace_between("// ---------- Lifecycle ----------", "// ---------- Grid ----------", LIFECYCLE)
replace_between("// ---------- Tick ----------", "void PipesScreensaver::grow_step() {", FRAME)
replace_between("// ---------- Layer sessions ----------", "// ---------- Growth ----------", "")
sub_expect(r"\brng_\b", "rng()", 8)
sub_expect(r"\bscreen_w_\b", "screen_w()", 3)
sub_expect(r"\bscreen_h_\b", "screen_h()", 3)
sub_expect(r"lv_canvas_init_layer\(canvas_, &layer\);", "canvas().begin_layer(&layer);", 2)
sub_expect(r"(?<!\.)\bfinish_layer\(&layer\);", "canvas().finish_layer(&layer);", 2)
sub_expect(r"(?m)^(\s+)mark_dirty\(", r"\1canvas().mark_dirty(", 2)
path.write_text(text)
print("pipes moved onto SaverBase")
```

(`screen_w_` and `screen_h_` each appear three times after the block replacements: once in `setup_camera`, once in `project` and once in the new `on_start` debug line.)

In `tests/test_helpers/screensaver_test_access.h`, delete these members from `PipesScreensaverTestAccess`: `draw_buf_size`, `overlay`, `canvas`, `timer`, `set_fixed_seed`. Then move every pipes call site onto the generic access:

```bash
perl -pi -e 's/\bPipesAccess::(overlay|canvas|timer|set_fixed_seed)\(/SaverTestAccess::$1(/g; s/\bPipesScreensaverTestAccess::(overlay|canvas|timer|set_fixed_seed|draw_buf_size)\(/SaverTestAccess::$1(/g' tests/unit/test_screensaver.cpp tests/unit/test_screensaver_canvas_stride.cpp tests/unit/test_screensaver_pipes_canvas.cpp tests/unit/test_screensaver_motion.cpp tests/unit/test_screensaver_fingerprint.cpp
grep -n -E 'PipesAccess::(overlay|canvas|timer|set_fixed_seed)|PipesScreensaverTestAccess::(overlay|canvas|timer|set_fixed_seed|draw_buf_size)' tests/unit/*.cpp; echo "leftover grep exit $?"
```

Expected: `leftover grep exit 1` (no leftovers).

- [ ] **Step 11: Build and prove pipes did not change**

Run: `scripts/syntax_check.py src/ui/screensaver_pipes.cpp src/application/display_manager.cpp` then `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver_fingerprint]"` then `./build/bin/helix-tests "[screensaver],[refresh_period]"` then `python3 scripts/check_timer_destructor_cancel.py; echo "exit $?"`.
Expected: `exit 0`; the pipes and starfield fingerprints pass unchanged; every `[screensaver]` case passes (including `each pipes grow step invalidates less than the canvas and every pixel it changes` and `a pipes grid reset invalidates the whole canvas`); the timer gate exits 0.

- [ ] **Step 12: Measure the pipes refactor on the Pi 3B**

After `task2-control.done` exists:

```bash
ARM=task3-pipes BUILD=1 HELIX_PERF_BUILD_TREE="$PWD" BASELINES="task2-control" ENV_EXTRA="HELIX_SCREENSAVER_REFR_PERIOD_MS=16" \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$SS_SCRATCH/task3-pipes.log" 2>&1 < /dev/null &
```

When `$HELIX_PERF_SCRATCH/task3-pipes.done` exists, read `$HELIX_PERF_SCRATCH/task3-pipes_summary.txt`. Pass criteria: `CRASHES_DURING_SMOKE=0` and `CRASHES_DURING_MEASUREMENT=0` in the log; its log shows `APP_ENV ok`; its hottest `THERMAL` reading and its count of throttled readings (commands in `scripts/screensaver-perf/README.md`) go in the report beside its numbers; every load-gate row `PASS`; for each saver, task3-pipes mean CPU total is no more than 1.0 percentage point above task2-control's largest run, and mean fps is no more than 1.0 below task2-control's smallest run. A failure blocks the commit: stop and report the two summary rows.

- [ ] **Step 13: Mutate, then commit**

Mutation: in `SaverCanvas::finish_layer`, replace `invalidate(layer_dirty_);` with `lv_obj_invalidate(canvas_);`, rebuild, and confirm `each pipes grow step invalidates less than the canvas and every pixel it changes` and `a saver canvas layer session invalidates only the areas it marked` fail. Restore.

Expected survivor, if `make mutate-diff` or a hand mutation tries it: swapping the hide and the free in `SaverCanvas::release`. `releasing a saver canvas hides it and frees its buffer` sees only the state after `release()` returns, and no refresh can run between two statements of one call on the host, so the order is kept by the comment in `release()` and by review, not by a test. Task 11 Step 10 lists it.

```bash
.venv/bin/clang-format -i include/screensaver_frame.h src/ui/screensaver_frame.cpp include/screensaver_overlay.h src/ui/screensaver_overlay.cpp include/screensaver_canvas.h src/ui/screensaver_canvas.cpp include/screensaver_frame_timer.h src/ui/screensaver_frame_timer.cpp include/screensaver_base.h src/ui/screensaver_base.cpp include/screensaver_pipes.h src/ui/screensaver_pipes.cpp include/screensaver_starfield_sim.h include/refresh_period_hold.h src/application/display_manager.cpp include/screensaver.h tests/test_helpers/refresh_period_hold_test_access.h tests/test_helpers/screensaver_test_access.h tests/unit/test_screensaver_parts.cpp tests/unit/application/test_refresh_period_hold.cpp tests/unit/test_screensaver.cpp tests/unit/test_screensaver_canvas_stride.cpp tests/unit/test_screensaver_pipes_canvas.cpp tests/unit/test_screensaver_motion.cpp tests/unit/test_screensaver_fingerprint.cpp
git add -- include/screensaver_frame.h src/ui/screensaver_frame.cpp include/screensaver_overlay.h src/ui/screensaver_overlay.cpp include/screensaver_canvas.h src/ui/screensaver_canvas.cpp include/screensaver_frame_timer.h src/ui/screensaver_frame_timer.cpp include/screensaver_base.h src/ui/screensaver_base.cpp include/screensaver_pipes.h src/ui/screensaver_pipes.cpp include/screensaver_starfield_sim.h include/refresh_period_hold.h src/application/display_manager.cpp include/screensaver.h docs/devel/ENVIRONMENT_VARIABLES.md tests/test_helpers/refresh_period_hold_test_access.h tests/test_helpers/screensaver_test_access.h tests/unit/test_screensaver_parts.cpp tests/unit/application/test_refresh_period_hold.cpp tests/unit/test_screensaver.cpp tests/unit/test_screensaver_canvas_stride.cpp tests/unit/test_screensaver_pipes_canvas.cpp tests/unit/test_screensaver_motion.cpp tests/unit/test_screensaver_fingerprint.cpp Makefile firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "refactor(screensaver): shared saver parts, with pipes as their first saver" -m "SaverOverlay, SaverCanvas (stride allocation, dirty areas merged to 32, the dirty-box layer session), SaverFrameTimer and SaverBase now own what each saver hand-rolled, and pipes implements only its scene hooks and a two-level ladder of 16 ms then 33 ms. HELIX_SCREENSAVER_REFR_PERIOD_MS replaces level 0's period when set, and while a saver runs the display refresh follows its current period. The pipes fingerprint is unchanged and the Pi 3B arm shows no regression against the control arm." -m "Mutation: finish_layer invalidated the whole canvas; the pipes grow-step invalidation test and the layer-session test went red"
```

---
### Task 4: Starfield on the base, with the 32-bit PixelWriter

Moves `FrameTarget` into the shared frame header, adds `PixelWriter` (XRGB8888 path) as the one place pixels are written, and moves starfield onto `SaverBase`. `StarfieldSim` stays. The starfield fingerprint must not change.

**Files:**
- Create: `include/screensaver_pixel_writer.h`
- Modify: `include/screensaver_frame.h` (`PixelFormat`, `Rgb`, `FrameTarget`), `include/screensaver_canvas.h` (`SaverCanvas::frame`), `include/screensaver_starfield_sim.h`, `src/ui/screensaver_starfield_sim.cpp`, `include/screensaver_starfield.h`, `src/ui/screensaver_starfield.cpp`, `tests/test_helpers/screensaver_test_access.h` (`StarfieldScreensaverTestAccess` keeps only star state), `tests/unit/test_screensaver.cpp`, `tests/unit/test_screensaver_canvas_stride.cpp`, `tests/unit/test_screensaver_sim.cpp`, `tests/unit/test_screensaver_motion.cpp`, `tests/unit/test_screensaver_fingerprint.cpp`
- Test: `tests/unit/test_screensaver_pixel_writer.cpp` (new), `[screensaver_fingerprint]`, `[starfield_sim]`, `[screensaver]`

**Interfaces:**
- Consumes: Task 3 `SaverBase`, `SaverCanvas`, `DirtyRect`, `TWO_LEVEL_COUNT`, `two_level_period_ms`, `SaverTestAccess`, `level_zero_periods`.
- Produces (Tasks 9, 10):
  - `include/screensaver_frame.h`: `enum class PixelFormat : uint8_t { XRGB8888 };` (Task 9 adds `RGB565`), `struct Rgb { uint8_t r, g, b; bool operator==(const Rgb&) const; };`, `struct FrameTarget { uint8_t* data; uint32_t stride; uint32_t w; uint32_t h; PixelFormat format = PixelFormat::XRGB8888; };`
  - `include/screensaver_pixel_writer.h`: `class PixelWriter { explicit PixelWriter(const FrameTarget& frame); bool contains(int32_t x, int32_t y) const; void put(int32_t x, int32_t y, Rgb c); Rgb get(int32_t x, int32_t y) const; void fill(Rgb c); };`
  - `FrameTarget SaverCanvas::frame() const;`
  - `helix::ui::fill_starfield_black` is removed; callers use `PixelWriter(frame).fill(Rgb{0, 0, 0})`.

- [ ] **Step 1: Write the failing PixelWriter tests**

Create `tests/unit/test_screensaver_pixel_writer.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_pixel_writer.h"

#include <cstdint>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::FrameTarget;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

namespace {

constexpr uint8_t UNTOUCHED = 0xAA;

/// A heap frame whose every byte starts as UNTOUCHED, so a write outside its pixel shows.
struct TestFrame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    TestFrame(uint32_t w, uint32_t h, uint32_t stride, PixelFormat format)
        : bytes(static_cast<size_t>(stride) * h, UNTOUCHED) {
        target = {bytes.data(), stride, w, h, format};
    }
    TestFrame(const TestFrame&) = delete;
    TestFrame& operator=(const TestFrame&) = delete;
};

} // namespace

TEST_CASE("PixelWriter writes XRGB8888 as B, G, R and an opaque X byte",
          "[screensaver][pixel_writer]") {
    TestFrame frame(4, 3, 4 * 4 + 8, PixelFormat::XRGB8888);
    PixelWriter writer(frame.target);

    writer.put(3, 2, Rgb{0x12, 0x34, 0x56});

    const size_t at = 2 * static_cast<size_t>(frame.target.stride) + 3 * 4;
    CHECK(frame.bytes[at] == 0x56);
    CHECK(frame.bytes[at + 1] == 0x34);
    CHECK(frame.bytes[at + 2] == 0x12);
    CHECK(frame.bytes[at + 3] == 0xFF);
    // Rows step by the stride, so the padding after the row's last pixel is left alone.
    CHECK(frame.bytes[at + 4] == UNTOUCHED);
    CHECK(frame.bytes[at - 1] == UNTOUCHED);
    CHECK(writer.get(3, 2) == Rgb{0x12, 0x34, 0x56});
}

TEST_CASE("PixelWriter fill covers every XRGB8888 pixel and no row padding",
          "[screensaver][pixel_writer]") {
    constexpr uint32_t W = 5;
    constexpr uint32_t H = 4;
    constexpr uint32_t STRIDE = W * 4 + 12;
    TestFrame frame(W, H, STRIDE, PixelFormat::XRGB8888);

    PixelWriter(frame.target).fill(Rgb{1, 2, 3});

    size_t wrong_pixels = 0;
    size_t touched_padding = 0;
    for (uint32_t y = 0; y < H; y++) {
        const uint8_t* row = frame.bytes.data() + static_cast<size_t>(y) * STRIDE;
        for (uint32_t x = 0; x < W; x++) {
            const uint8_t* px = row + x * 4;
            wrong_pixels += (px[0] != 3 || px[1] != 2 || px[2] != 1 || px[3] != 0xFF) ? 1 : 0;
        }
        for (uint32_t i = W * 4; i < STRIDE; i++) {
            touched_padding += row[i] != UNTOUCHED ? 1 : 0;
        }
    }
    CHECK(wrong_pixels == 0);
    CHECK(touched_padding == 0);
}

TEST_CASE("PixelWriter knows which points lie inside the frame", "[screensaver][pixel_writer]") {
    TestFrame frame(4, 3, 16, PixelFormat::XRGB8888);
    const PixelWriter writer(frame.target);
    CHECK(writer.contains(0, 0));
    CHECK(writer.contains(3, 2));
    CHECK_FALSE(writer.contains(4, 0));
    CHECK_FALSE(writer.contains(0, 3));
    CHECK_FALSE(writer.contains(-1, 0));
    CHECK_FALSE(writer.contains(0, -1));
}
```

In `tests/unit/test_screensaver_motion.cpp`, replace the test case `"StarfieldScreensaver ticks at the display refresh period"` with:

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "StarfieldScreensaver level 0: configured period, else 16 ms, with the refresh equal",
                 "[screensaver][screensaver_motion]") {
    const uint32_t configured_ms = GENERATE(as<uint32_t>{}, 0, 20);
    INFO("configured period " << configured_ms << " ms");
    const LevelZeroPeriods periods = level_zero_periods<StarfieldScreensaver>(configured_ms);
    CHECK(periods.timer_ms == (configured_ms != 0 ? configured_ms : helix::ui::SAVER_FAST_PERIOD));
    CHECK(periods.refresh_ms == periods.timer_ms);
}
```

- [ ] **Step 2: Run the build and watch it fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; the log names `screensaver_pixel_writer.h: No such file or directory`.

- [ ] **Step 3: Move FrameTarget into the frame header and write PixelWriter**

In `include/screensaver_frame.h`, insert directly after `inline constexpr size_t SAVER_MAX_DIRTY_AREAS = 32;` and its comment:

```cpp
/// How a FrameTarget stores its pixels.
enum class PixelFormat : uint8_t {
    /// 4 bytes per pixel: B, G, R, and an X byte of 0xFF
    XRGB8888,
};

/// An 8-bit-per-channel colour.
struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb& o) const {
        return r == o.r && g == o.g && b == o.b;
    }
};

/// A frame a saver writes pixels into directly: rows start `stride` bytes apart.
struct FrameTarget {
    uint8_t* data = nullptr;
    uint32_t stride = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    PixelFormat format = PixelFormat::XRGB8888;
};
```

Create `include/screensaver_pixel_writer.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_frame.h"

#include <cstddef>
#include <cstdint>

namespace helix::ui {

/**
 * @brief Writes colours into a FrameTarget
 *
 * No LVGL: the frame's format is data, so the host test build exercises every format
 * whatever LV_COLOR_DEPTH the app is built with. Callers keep coordinates inside the frame,
 * which contains() answers.
 */
class PixelWriter {
  public:
    explicit PixelWriter(const FrameTarget& frame) : frame_(frame) {}

    bool contains(int32_t x, int32_t y) const {
        return x >= 0 && y >= 0 && x < static_cast<int32_t>(frame_.w) &&
               y < static_cast<int32_t>(frame_.h);
    }

    /// Writes `c` at (x, y), with the X byte at 0xFF.
    void put(int32_t x, int32_t y, Rgb c) {
        uint8_t* p = pixel(x, y);
        p[0] = c.b;
        p[1] = c.g;
        p[2] = c.r;
        p[3] = 0xFF;
    }

    /// The colour stored at (x, y).
    Rgb get(int32_t x, int32_t y) const {
        const uint8_t* p = pixel(x, y);
        return {p[2], p[1], p[0]};
    }

    /// Writes `c` over every pixel, leaving row padding alone.
    void fill(Rgb c) {
        for (int32_t y = 0; y < static_cast<int32_t>(frame_.h); y++) {
            for (int32_t x = 0; x < static_cast<int32_t>(frame_.w); x++) {
                put(x, y, c);
            }
        }
    }

  private:
    uint8_t* pixel(int32_t x, int32_t y) const {
        return frame_.data + static_cast<size_t>(y) * frame_.stride + static_cast<size_t>(x) * 4;
    }

    FrameTarget frame_;
};

} // namespace helix::ui
```

In `include/screensaver_canvas.h`, add inside `class SaverCanvas`, directly after the `height()` accessor as Task 3 wrote it (the three lines `int32_t height() const {`, `return h_;`, `}`):

```cpp
    /// The buffer as a frame for direct pixel writes, for a canvas in a format PixelWriter writes.
    FrameTarget frame() const {
        return {buf_, stride_, static_cast<uint32_t>(w_), static_cast<uint32_t>(h_),
                PixelFormat::XRGB8888};
    }
```

- [ ] **Step 4: Run the PixelWriter tests**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[pixel_writer]"`.
Expected: the build fails in `include/screensaver_starfield_sim.h` with `redefinition of 'struct helix::ui::FrameTarget'`; continue to Step 5, which removes the old definition, then expect `exit 0` and all three `[pixel_writer]` cases passing.

- [ ] **Step 5: Write the starfield simulation on PixelWriter**

In `include/screensaver_starfield_sim.h`, delete the `struct FrameTarget { ... };` definition with its doc comment and the declaration `void fill_starfield_black(FrameTarget& target);` with its comment. The file keeps `#include "screensaver_frame.h"` from Task 3.

Replace `src/ui/screensaver_starfield_sim.cpp` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_starfield_sim.h"

#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <cmath>

namespace helix::ui {

namespace {

using screensaver::random_below;
using screensaver::unit_random;

// A star's speed is its depth change per this many ms of frame time
constexpr float SPEED_FRAME_MS = 33.0f;
constexpr float COLOR_THRESHOLD = 0.35f; // stars closer than this show color

constexpr Rgb BLACK = {0, 0, 0};

// Star color tints: blue dwarfs, red giants, yellow suns, blue-white hot stars
constexpr uint8_t STAR_TINTS[][3] = {
    {255, 255, 255}, // white (most common)
    {255, 255, 255}, // white
    {255, 255, 255}, // white
    {255, 200, 150}, // warm yellow
    {255, 160, 120}, // orange
    {255, 120, 100}, // red giant
    {150, 180, 255}, // blue dwarf
    {200, 220, 255}, // blue-white
};
constexpr int NUM_TINTS = sizeof(STAR_TINTS) / sizeof(STAR_TINTS[0]);

void assign_tint(std::minstd_rand& rng, uint8_t& r, uint8_t& g, uint8_t& b) {
    int idx = random_below(rng, NUM_TINTS);
    r = STAR_TINTS[idx][0];
    g = STAR_TINTS[idx][1];
    b = STAR_TINTS[idx][2];
}

/// Paints the size x size square at (sx, sy), clipped to the frame, and adds what it wrote
/// to `dirty`.
void fill_square(PixelWriter& writer, const FrameTarget& target, int sx, int sy, int size,
                 Rgb color, DirtyRect& dirty) {
    const int x1 = std::max(sx, 0);
    const int y1 = std::max(sy, 0);
    const int x2 = std::min(sx + size, static_cast<int>(target.w)) - 1;
    const int y2 = std::min(sy + size, static_cast<int>(target.h)) - 1;
    if (x2 < x1 || y2 < y1) {
        return;
    }
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            writer.put(x, y, color);
        }
    }
    dirty.add(x1, y1, x2, y2);
}

} // namespace

void StarfieldSim::init(uint32_t w, uint32_t h, std::minstd_rand& rng) {
    cx_ = static_cast<float>(w) / 2.0f;
    cy_ = static_cast<float>(h) / 2.0f;
    focal_ = static_cast<float>(w) / 3.0f;

    stars_.resize(NUM_STARS);
    for (auto& star : stars_) {
        float angle = unit_random(rng) * 2.0f * 3.14159265f;
        float radius = 0.1f + unit_random(rng) * 0.9f;
        star.x = radius * std::cos(angle);
        star.y = radius * std::sin(angle);
        star.z = 0.01f + unit_random(rng) * 0.99f;
        star.speed = 0.008f + unit_random(rng) * 0.017f;
        assign_tint(rng, star.tint_r, star.tint_g, star.tint_b);
        star.prev_sx = 0;
        star.prev_sy = 0;
        star.prev_size = 0;
    }
}

void StarfieldSim::recycle(Star& star, std::minstd_rand& rng) {
    // Pick random angle + radius so stars fly uniformly in all directions
    float angle = unit_random(rng) * 2.0f * 3.14159265f;
    float radius = 0.3f + unit_random(rng) * 0.7f;
    star.x = radius * std::cos(angle);
    star.y = radius * std::sin(angle);
    star.z = 1.0f;
    star.speed = 0.008f + unit_random(rng) * 0.017f;
    assign_tint(rng, star.tint_r, star.tint_g, star.tint_b);
}

DirtyRect StarfieldSim::step(uint32_t dt_ms, FrameTarget& target, std::minstd_rand& rng) {
    DirtyRect dirty;
    PixelWriter writer(target);
    const int w = static_cast<int>(target.w);
    const int h = static_cast<int>(target.h);

    // Erase previous star positions (an incremental clear, which avoids a full-frame fill)
    for (auto& star : stars_) {
        if (star.prev_size == 0) {
            continue;
        }
        fill_square(writer, target, star.prev_sx, star.prev_sy, star.prev_size, BLACK, dirty);
        star.prev_size = 0;
    }

    const float frames = static_cast<float>(dt_ms) / SPEED_FRAME_MS;
    for (auto& star : stars_) {
        // Move star closer by its speed, scaled to the time since the previous frame
        star.z -= star.speed * frames;

        if (star.z <= 0.01f) {
            recycle(star, rng);
            continue;
        }

        // Project to frame coordinates
        float sx = cx_ + (star.x / star.z) * focal_;
        float sy = cy_ + (star.y / star.z) * focal_;

        if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
            recycle(star, rng);
            continue;
        }

        int isx = static_cast<int>(sx);
        int isy = static_cast<int>(sy);

        // Size: larger when closer (z near 0)
        int size = std::max(1, static_cast<int>(3.0f * (1.0f - star.z)));

        // Brightness: brighter when closer, with minimum floor
        float bright_f = 80.0f + 175.0f * (1.0f - star.z);

        // Close stars show their color tint; distant stars stay white
        uint8_t r, g, b;
        if (star.z < COLOR_THRESHOLD) {
            float tint_mix = (COLOR_THRESHOLD - star.z) / COLOR_THRESHOLD;
            r = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_r / 255.0f));
            g = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_g / 255.0f));
            b = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_b / 255.0f));
        } else {
            r = g = b = static_cast<uint8_t>(bright_f);
        }

        fill_square(writer, target, isx, isy, size, Rgb{r, g, b}, dirty);

        // Remember position for next step's erase pass
        star.prev_sx = static_cast<int16_t>(isx);
        star.prev_sy = static_cast<int16_t>(isy);
        star.prev_size = static_cast<uint8_t>(size);
    }

    return dirty;
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
```

In `tests/unit/test_screensaver_sim.cpp`, add `#include "screensaver_pixel_writer.h"` after `#include "screensaver_starfield_sim.h"`, and replace `helix::ui::fill_starfield_black(target);` with `helix::ui::PixelWriter(target).fill(helix::ui::Rgb{0, 0, 0});`.

- [ ] **Step 6: Move the starfield saver onto the base**

Replace `include/screensaver_starfield.h` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_starfield_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

/**
 * @brief Windows 95-style Starfield screensaver
 *
 * Stars fly outward from the center of the screen. Each star starts small
 * and dim near the center, growing larger and brighter as it approaches
 * the edges.
 *
 * helix::ui::StarfieldSim draws each frame with direct pixel writes, erasing only where
 * stars were, and moves the stars by the time since the previous frame; the frame returns
 * only the part of the canvas the step changed.
 */
class StarfieldScreensaver : public helix::ui::SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::STARFIELD;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::SCREENSAVER_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return helix::ui::TWO_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return helix::ui::two_level_period_ms(level);
    }

  private:
    // Test-only seam: reads and places the stars. See tests/test_helpers/screensaver_test_access.h.
    friend class StarfieldScreensaverTestAccess;

    helix::ui::StarfieldSim sim_;
};

#endif // HELIX_ENABLE_SCREENSAVER
```

Replace `src/ui/screensaver_starfield.cpp` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_starfield.h"

#include <spdlog/spdlog.h>

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::StarfieldSim;

static_assert(LV_COLOR_DEPTH == 32, "the starfield canvas is XRGB8888 on a 32 bpp display");

bool StarfieldScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting starfield");
    sim_.init(static_cast<uint32_t>(screen_w()), static_cast<uint32_t>(screen_h()), rng());
    spdlog::debug("[Screensaver] Starfield started ({}x{}, {} stars)", screen_w(), screen_h(),
                  StarfieldSim::NUM_STARS);
    return true;
}

void StarfieldScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    FrameTarget target = canvas().frame();
    dirty.push_back(sim_.step(dt_ms, target, rng()));
}

void StarfieldScreensaver::on_stop() {
    sim_.stars().clear();
}

#endif // HELIX_ENABLE_SCREENSAVER
```

In `tests/test_helpers/screensaver_test_access.h`, delete these members from `StarfieldScreensaverTestAccess`: `draw_buf_size`, `draw_buf_stride`, `overlay`, `canvas`, `timer`, `set_fixed_seed`. Then:

```bash
perl -pi -e 's/\bStarAccess::(overlay|canvas|timer|set_fixed_seed|draw_buf_size|draw_buf_stride)\(/SaverTestAccess::$1(/g; s/\bStarfieldScreensaverTestAccess::(overlay|canvas|timer|set_fixed_seed|draw_buf_size|draw_buf_stride)\(/SaverTestAccess::$1(/g' tests/unit/test_screensaver.cpp tests/unit/test_screensaver_canvas_stride.cpp tests/unit/test_screensaver_sim.cpp tests/unit/test_screensaver_motion.cpp tests/unit/test_screensaver_fingerprint.cpp
grep -n -E '(StarAccess|StarfieldScreensaverTestAccess)::(overlay|canvas|timer|set_fixed_seed|draw_buf_size|draw_buf_stride)' tests/unit/*.cpp; echo "leftover grep exit $?"
```

Expected: `leftover grep exit 1`.

- [ ] **Step 7: Build and prove starfield did not change**

Run: `scripts/syntax_check.py src/ui/screensaver_starfield.cpp src/ui/screensaver_starfield_sim.cpp` then `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver_fingerprint]"` then `./build/bin/helix-tests "[pixel_writer],[starfield_sim],[screensaver]"`.
Expected: `exit 0`; both fingerprints pass unchanged; every listed case passes.

- [ ] **Step 8: Measure the starfield refactor on the Pi 3B**

```bash
ARM=task4-starfield BUILD=1 HELIX_PERF_BUILD_TREE="$PWD" BASELINES="task2-control task3-pipes" ENV_EXTRA="HELIX_SCREENSAVER_REFR_PERIOD_MS=16" \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$SS_SCRATCH/task4-starfield.log" 2>&1 < /dev/null &
```

When `task4-starfield.done` exists, apply the Task 3 Step 12 pass criteria against `task2-control`. A failure blocks the commit.

- [ ] **Step 9: Mutate, then commit**

Mutation: in `PixelWriter::put`, change `p[3] = 0xFF;` to `p[3] = 0x00;`, rebuild, confirm `PixelWriter writes XRGB8888 as B, G, R and an opaque X byte`, `every X byte of a StarfieldSim frame stays 0xFF as stars move and recycle` and `the starfield canvas matches its recorded pixel fingerprint` fail. Restore.

```bash
.venv/bin/clang-format -i include/screensaver_frame.h include/screensaver_pixel_writer.h include/screensaver_canvas.h include/screensaver_starfield_sim.h src/ui/screensaver_starfield_sim.cpp include/screensaver_starfield.h src/ui/screensaver_starfield.cpp tests/test_helpers/screensaver_test_access.h tests/unit/test_screensaver_pixel_writer.cpp tests/unit/test_screensaver.cpp tests/unit/test_screensaver_canvas_stride.cpp tests/unit/test_screensaver_sim.cpp tests/unit/test_screensaver_motion.cpp tests/unit/test_screensaver_fingerprint.cpp
git add -- include/screensaver_frame.h include/screensaver_pixel_writer.h include/screensaver_canvas.h include/screensaver_starfield_sim.h src/ui/screensaver_starfield_sim.cpp include/screensaver_starfield.h src/ui/screensaver_starfield.cpp tests/test_helpers/screensaver_test_access.h tests/unit/test_screensaver_pixel_writer.cpp tests/unit/test_screensaver.cpp tests/unit/test_screensaver_canvas_stride.cpp tests/unit/test_screensaver_sim.cpp tests/unit/test_screensaver_motion.cpp tests/unit/test_screensaver_fingerprint.cpp
git commit -m "refactor(screensaver): starfield on the shared parts, writing through PixelWriter" -m "FrameTarget moves to the shared frame header and PixelWriter becomes the one writer of XRGB8888 pixels, X byte included. The starfield saver keeps StarfieldSim and implements only its hooks and a 16 ms then 33 ms ladder. Its fingerprint is unchanged and the Pi 3B arm shows no regression." -m "Mutation: PixelWriter::put wrote an X byte of 0x00; the XRGB8888 bits test, the starfield X-byte test and the starfield fingerprint went red"
```

---

### Task 5: Toasters on the base

Moves flying toasters onto `SaverBase` with an overlay, a frame timer and no canvas, on a three-rung ladder: 16 ms with every sprite, 33 ms with every sprite, 33 ms with 10 sprites. The lowest rung takes over the tier-based sprite cap; until the gate lands in Task 6, a BASIC or EMBEDDED board still flies the capped count at every level, so the Pi 3B arm stays like for like. The existing toaster tests keep their assertions except the level 0 period; the per-saver timer switch in the manager tests collapses onto the generic access, `screensaver_timer_period_ms()` loses its last caller and goes, and the refresh-period tests configure 20 ms, apart from the 16 ms default.

**Files:**
- Modify: `include/ui_screensaver.h` (rewritten), `src/ui/ui_screensaver.cpp` (lifecycle, frame and sprite-cap sections), `include/screensaver.h` and `src/ui/screensaver_manager.cpp` (`screensaver_timer_period_ms` removed), `tests/test_helpers/screensaver_test_access.h` (`FlyingToasterScreensaverTestAccess::tick_timer` removed), `tests/unit/test_screensaver.cpp` (toaster timer call sites, `running_saver_timer_period`, the toaster period case, the refresh-period cases, one new test case)
- Test: `[screensaver]`, `[screensaver_motion]`

**Interfaces:**
- Consumes: Task 3 `SaverBase` (including the `on_level_request` hook), `SaverTestAccess`, `SAVER_FAST_PERIOD`, `ScopedRefreshPeriod`, `ScopedGlobalRefreshHold`, `level_zero_periods`.
- Produces (Task 6): all three existing savers derive from `helix::ui::SaverBase`, so `static_cast<helix::ui::SaverBase&>` on any registered `Screensaver` is valid; toasters have `ladder_size() == 3` with `LEVEL_PERIODS_MS = {SAVER_FAST_PERIOD, 33, 33}`, `CAPPED_LEVEL = 2`, private `size_t sprite_limit(size_t level) const`, `void drop_sprites_past(size_t limit)` and `bool m_low_tier` (removed in Task 6); `FlyingToasterScreensaverTestAccess` keeps `sprites`, `frames_decoded`, `frame_of`.

- [ ] **Step 1: Write the failing ladder test and the generic timer helper**

In `tests/unit/test_screensaver.cpp`, replace the whole body of `uint32_t running_saver_timer_period(ScreensaverType type)` with:

```cpp
uint32_t running_saver_timer_period(ScreensaverType type) {
    Screensaver* active =
        helix::ScreensaverManagerTestAccess::active(ScreensaverManager::instance());
    REQUIRE(active != nullptr);
    REQUIRE(active->type() == type);
    // Every registered saver runs on SaverBase.
    const lv_timer_t* timer =
        SaverTestAccess::timer(static_cast<const helix::ui::SaverBase&>(*active));
    REQUIRE(timer != nullptr);
    return timer->period;
}
```

Append this test case at the end of the file, before `#endif // HELIX_ENABLE_SCREENSAVER`:

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "each existing saver runs 16 ms at level 0, then 33 ms, without a motion jump",
                 "[screensaver][screensaver_motion]") {
    // Matches neither LVGL's default refresh period nor the level 0 period.
    ScopedRefreshPeriod refresh(20);
    ScopedGlobalRefreshHold clean_hold; // no configured period, so level 0 is 16 ms

    SECTION("flying toasters") {
        if (!helix::PlatformCapabilities::detect().supports_animations) {
            SKIP("BASIC and EMBEDDED hosts fly ten sprites at every level until the gate lands");
        }
        FlyingToasterScreensaver ss;
        ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        REQUIRE(ToasterAccess::frames_decoded(ss));
        const auto initial = shown_frames(ss);
        const auto all_sprites = ToasterAccess::sprites(ss);
        REQUIRE(all_sprites.size() > 10);
        CHECK(ss.level_count() == 3);
        CHECK(SaverTestAccess::timer(ss)->period == helix::ui::SAVER_FAST_PERIOD);
        for (int i = 0; i < 10; i++) {
            lv_tick_inc(400);
            run_toaster_tick(ss);
        }

        ss.request_level(1);
        CHECK(ss.level() == 1);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
        CHECK(ToasterAccess::sprites(ss).size() == all_sprites.size());

        lv_tick_inc(100);
        run_toaster_tick(ss);
        // Sprites sit exactly where 4.1 s of flight puts them, so the switch cost no time.
        CHECK(check_sprites_at(ss, 4100, initial).visible_toasters > 0);

        // The lowest rung flies ten sprites and lets the rest go.
        ss.request_level(2);
        CHECK(ss.level() == 2);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
        CHECK(ToasterAccess::sprites(ss).size() == 10);
        CHECK(lv_obj_has_flag(all_sprites[10].img, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("flying toasters started at the lowest rung") {
        FlyingToasterScreensaver ss;
        ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};
        ss.set_start_level(2);
        ss.start();
        REQUIRE(ss.is_active());
        CHECK(ToasterAccess::sprites(ss).size() == 10);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
    }

    SECTION("starfield") {
        StarfieldScreensaver ss;
        ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        CHECK(ss.level_count() == 2);
        CHECK(SaverTestAccess::timer(ss)->period == helix::ui::SAVER_FAST_PERIOD);
        ss.request_level(1);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
    }

    SECTION("pipes") {
        PipesScreensaver ss;
        ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        CHECK(ss.level_count() == 2);
        CHECK(SaverTestAccess::timer(ss)->period == helix::ui::SAVER_FAST_PERIOD);
        ss.request_level(1);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
    }
}
```

Then move the toaster period case onto the shared level 0 helper, and configure 20 ms in the refresh-period cases so the configured period is visibly not the default. Save as `$SS_SCRATCH/toaster_period_tests.py` and run it before the `perl` below:

```python
from pathlib import Path

path = Path("tests/unit/test_screensaver.cpp")
text = path.read_text()


def replace_once(old, new):
    global text
    assert text.count(old) == 1, old[:60]
    text = text.replace(old, new)


case_start = text.index('TEST_CASE_METHOD(LVGLTestFixture, "FlyingToasterScreensaver ticks at the display refresh period",')
case_end = text.index("\n}\n", case_start) + len("\n}\n")
text = text[:case_start] + '''TEST_CASE_METHOD(LVGLTestFixture,
                 "FlyingToasterScreensaver level 0: configured period, else 16 ms, with the refresh equal",
                 "[screensaver][screensaver_motion]") {
    const uint32_t configured_ms = GENERATE(as<uint32_t>{}, 0, 20);
    INFO("configured period " << configured_ms << " ms");
    const LevelZeroPeriods periods = level_zero_periods<FlyingToasterScreensaver>(configured_ms);
    CHECK(periods.timer_ms == (configured_ms != 0 ? configured_ms : helix::ui::SAVER_FAST_PERIOD));
    CHECK(periods.refresh_ms == periods.timer_ms);
}
''' + text[case_end:]
replace_once("constexpr uint32_t SAVER_PERIOD_MS = 16;",
             "// Unlike the 16 ms default, so the checks see the configured period reach each saver.\n"
             "constexpr uint32_t SAVER_PERIOD_MS = 20;")
replace_once('setenv("HELIX_SCREENSAVER_REFR_PERIOD_MS", "16", 1);', 'setenv("HELIX_SCREENSAVER_REFR_PERIOD_MS", "20", 1);')
replace_once("""    // The saver reads the refresh period when it starts, so this is only the configured
    // value if the period was set before start().
""", """    // A saver reads the configured period as it starts, as its level 0 frame period.
""")
path.write_text(text)
print("toaster and refresh-period cases updated")
```

Move the toaster timer call sites to the generic access:

```bash
perl -pi -e 's/\bToasterAccess::tick_timer\(/SaverTestAccess::timer(/g; s/\bFlyingToasterScreensaverTestAccess::tick_timer\(/SaverTestAccess::timer(/g' tests/unit/test_screensaver.cpp
```

`screensaver_base.h` (Task 3) and `platform_capabilities.h` are already included.

- [ ] **Step 2: Run the build and watch it fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; the log reports that `FlyingToasterScreensaver` has no member `level_count` and cannot convert to `const helix::ui::SaverBase&`.

- [ ] **Step 3: Rewrite the toaster header**

Replace `include/ui_screensaver.h` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_motion.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

/**
 * @brief Flying Toasters screensaver (After Dark, 1989)
 *
 * Toasters and toast fly diagonally across a black screen until a touch wakes the UI.
 * Every sprite's position and wing frame are computed from the time the saver has run, so
 * they keep their speed however often or unevenly the frame timer fires, and a level change
 * that changes the timer's period changes nothing on screen but the frame rate. The lowest
 * rung also flies fewer sprites.
 */
class FlyingToasterScreensaver : public helix::ui::SaverBase {
  public:
    FlyingToasterScreensaver() = default;
    FlyingToasterScreensaver(const FlyingToasterScreensaver&) = delete;
    FlyingToasterScreensaver& operator=(const FlyingToasterScreensaver&) = delete;

    ScreensaverType type() const override {
        return ScreensaverType::FLYING_TOASTERS;
    }

  protected:
    /// Sprites are LVGL images on the overlay; there is no canvas.
    std::optional<lv_color_format_t> canvas_format() const override {
        return std::nullopt;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return sizeof(LEVEL_PERIODS_MS) / sizeof(LEVEL_PERIODS_MS[0]);
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return LEVEL_PERIODS_MS[level];
    }
    /// Applies a level at once; the lowest rung also lets go of the sprites past its cap.
    void on_level_request(size_t level) override;

  private:
    // Test-only seam: reads the sprites and decoded frames so frame-time-driven motion can be
    // pinned. See tests/test_helpers/screensaver_test_access.h.
    friend class FlyingToasterScreensaverTestAccess;

    /// Frame period per level: 16 ms with every sprite, 33 ms with every sprite, 33 ms with the
    /// capped sprite count.
    static constexpr uint32_t LEVEL_PERIODS_MS[] = {helix::ui::SAVER_FAST_PERIOD, 33, 33};
    /// First level that flies the capped sprite count.
    static constexpr size_t CAPPED_LEVEL = 2;

    struct FlyingObject {
        lv_obj_t* img;
        bool is_toaster;
        int16_t start_x;
        int16_t start_y;
        int fly_ms;
        int delay_ms;
        // Wing flap (toasters only)
        uint8_t initial_frame;
        uint8_t flap_frame;    // frame currently shown
        uint16_t flap_step_ms; // how long each wing frame holds
        // Previous position; lv_obj_set_pos() is skipped when unchanged to avoid invalidation
        int16_t prev_x = INT16_MIN;
        int16_t prev_y = INT16_MIN;
    };

    /** @brief Spawn the first `count` flying objects with staggered positions and delays */
    void spawn_objects(size_t count);

    /** @brief Sprites flown at `level` */
    size_t sprite_limit(size_t level) const;

    /** @brief Lets go of the sprites past `limit`: hidden now, deleted on a later timer pass */
    void drop_sprites_past(size_t limit);

    /** @brief Create a single flying object */
    void create_flying_object(int start_x, int start_y, bool is_toaster, bool reverse_flap,
                              int speed_ms, int delay_ms);

    /** @brief Get image scale factor based on screen width */
    int get_scale_factor() const;

    /** @brief Pre-decode all PNG sprites into persistent RAM buffers */
    void decode_sprites();

    /** @brief Free pre-decoded sprite buffers */
    void free_sprites();

    std::vector<FlyingObject> m_objects;
    uint32_t m_elapsed_ms = 0; // time the saver has run
    bool m_low_tier = false; // a BASIC or EMBEDDED board flies the capped count at every level

    // Pre-decoded sprite buffers (avoid per-frame PNG file I/O + decompression)
    lv_draw_buf_t* m_decoded_frames[4] = {}; // toaster_0..3
    lv_draw_buf_t* m_decoded_toast = nullptr;
};

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 4: Move the toaster source onto the base**

Save as `$SS_SCRATCH/toasters_onto_base.py` and run `python3 "$SS_SCRATCH/toasters_onto_base.py"`:

```python
import re
from pathlib import Path

path = Path("src/ui/ui_screensaver.cpp")
text = path.read_text()


def replace_once(old, new):
    global text
    assert text.count(old) == 1, f"expected one {old[:50]!r}, found {text.count(old)}"
    text = text.replace(old, new)


def replace_between(start, end, new):
    global text
    assert text.count(start) == 1 and text.count(end) == 1, (start, end)
    a = text.index(start)
    b = text.index(end, a)
    text = text[:a] + new + text[b:]


LIFECYCLE = """bool FlyingToasterScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting flying toasters");

    const auto caps = helix::PlatformCapabilities::detect();
    m_low_tier = !caps.supports_animations;

    m_elapsed_ms = 0;
    decode_sprites();
    spawn_objects(sprite_limit(level()));
    return true;
}

void FlyingToasterScreensaver::on_stop() {
    // The sprites are children of the overlay the base has already queued for deletion.
    m_objects.clear();
    free_sprites();
}

void FlyingToasterScreensaver::on_level_request(size_t level) {
    apply_level(level);
    drop_sprites_past(sprite_limit(this->level()));
}

size_t FlyingToasterScreensaver::sprite_limit(size_t level) const {
    const bool capped = m_low_tier || level >= CAPPED_LEVEL;
    return static_cast<size_t>(capped ? std::min(NUM_OBJECTS, SPRITE_CAP_LOW) : NUM_OBJECTS);
}

void FlyingToasterScreensaver::drop_sprites_past(size_t limit) {
    if (m_objects.size() <= limit) {
        return;
    }
    // Each sprite is hidden now and freed on the next timer pass.
    for (size_t i = limit; i < m_objects.size(); i++) {
        helix::ui::safe_delete_deferred(m_objects[i].img);
    }
    m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(limit), m_objects.end());
}

"""

FRAME = """// Sprites are LVGL images, which invalidate their own old and new areas when they move or
// change frame, so a frame adds nothing to the dirty list.
void FlyingToasterScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& /*dirty*/) {
    m_elapsed_ms += dt_ms;

    lv_display_t* disp = lv_display_get_default();
    int screen_w = disp ? lv_display_get_horizontal_resolution(disp) : 800;
    int screen_h = disp ? lv_display_get_vertical_resolution(disp) : 480;
    int obj_size = 64;
    int scale = get_scale_factor();
    if (scale != 256) {
        obj_size = obj_size * scale / 256;
    }

    for (auto& obj : m_objects) {
        if (!obj.img)
            continue;

        // Position is a function of elapsed time, never of how often the timer fired
        const FlightPos pos = flight_pos_at(m_elapsed_ms, obj.start_x, obj.start_y, obj.fly_ms,
                                            obj.delay_ms, FLIGHT_DISTANCE);

        // Skip objects still in their start delay
        if (!pos.started) {
            continue;
        }
        auto new_x = static_cast<int16_t>(pos.x);
        auto new_y = static_cast<int16_t>(pos.y);

        // Hide objects that are entirely off-screen (LVGL skips hidden objects in render)
        bool on_screen =
            (new_x + obj_size > 0 && new_x < screen_w && new_y + obj_size > 0 && new_y < screen_h);
        if (!on_screen) {
            if (!lv_obj_has_flag(obj.img, LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(obj.img, LV_OBJ_FLAG_HIDDEN);
            obj.prev_x = new_x;
            obj.prev_y = new_y;
            continue;
        }
        if (lv_obj_has_flag(obj.img, LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(obj.img, LV_OBJ_FLAG_HIDDEN);

        if (new_x != obj.prev_x || new_y != obj.prev_y) {
            lv_obj_set_pos(obj.img, new_x, new_y);
            obj.prev_x = new_x;
            obj.prev_y = new_y;
        }

        // Flap wing frames (toasters only)
        if (!obj.is_toaster)
            continue;

        // Wing frame cycles 0→1→2→3→2→1 on elapsed time
        const uint8_t frame =
            flap_frame_at(m_elapsed_ms, obj.delay_ms, obj.flap_step_ms, obj.initial_frame);

        // Only update image source when frame actually changed (RAM buffer, no file I/O)
        if (frame != obj.flap_frame) {
            obj.flap_frame = frame;
            lv_image_set_src(obj.img, m_decoded_frames[frame]);
        }
    }
}

"""

replace_once('#include "ui_timer_guard.h" // lv_timer_cancel_safe\n', "")
replace_once("using helix::ui::screensaver::FlightPos;\n",
             "using helix::ui::screensaver::FlightPos;\nusing helix::ui::DirtyRect;\n")
replace_once("void FlyingToasterScreensaver::spawn_objects(bool low_tier) {",
             "void FlyingToasterScreensaver::spawn_objects(size_t count) {")
replace_once("    const int active_count = low_tier ? std::min(NUM_OBJECTS, SPRITE_CAP_LOW) : NUM_OBJECTS;",
             "    const int active_count = std::min(NUM_OBJECTS, static_cast<int>(count));")
replace_once("""    spdlog::debug("[Screensaver] Spawned {}/{} flying objects ({}x{} screen, {}px sprites, "
                  "low_tier={})",
                  m_objects.size(), NUM_OBJECTS, screen_w, screen_h, obj_size, low_tier);""",
             """    spdlog::debug("[Screensaver] Spawned {}/{} flying objects ({}x{} screen, {}px sprites)",
                  m_objects.size(), NUM_OBJECTS, screen_w, screen_h, obj_size);""")
cap_start = text.index("// Low-tier sprite cap")
cap_end = text.index("static constexpr int SPRITE_CAP_LOW = 10;\n")
text = (text[:cap_start]
        + "// Sprites flown at the lowest level, and at every level on a BASIC or EMBEDDED board: every\n"
        + "// visible sprite costs dirty-region work each frame. OBJECTS[] is ordered by delay and wave,\n"
        + "// so the first ones keep a representative mix.\n"
        + text[cap_end:])
replace_between("void FlyingToasterScreensaver::start() {",
                "void FlyingToasterScreensaver::spawn_objects(size_t count) {", LIFECYCLE)
replace_between("void FlyingToasterScreensaver::tick_cb(lv_timer_t* timer) {",
                "void FlyingToasterScreensaver::decode_sprites() {", FRAME)
text, count = re.subn(r"\bm_overlay\b", "overlay().obj()", text)
assert count == 2, f"m_overlay: replaced {count}, expected 2"
for gone in ("m_tick_timer", "m_clock", "m_active", "self->"):
    assert gone not in text, gone
# The lifecycle above adds the m_low_tier member; only the old low_tier parameter must be gone.
assert not re.search(r"\blow_tier\b", text), "the low_tier parameter is still used"
path.write_text(text)

# screensaver_timer_period_ms() has no caller left.
header = Path("include/screensaver.h")
h = header.read_text()
start = h.index("/**\n * @brief Period for a screensaver's frame timer")
end = h.index("uint32_t screensaver_timer_period_ms();\n") + len("uint32_t screensaver_timer_period_ms();\n\n")
header.write_text(h[:start] + h[end:])
manager = Path("src/ui/screensaver_manager.cpp")
m = manager.read_text()
for old in ('#include "lvgl/src/misc/lv_timer_private.h" // lv_timer_t::period; LVGL has no period getter\n',
            "uint32_t helix::ui::screensaver_timer_period_ms() {\n"
            "    lv_display_t* disp = lv_display_get_default();\n"
            "    const lv_timer_t* refr = disp ? lv_display_get_refr_timer(disp) : nullptr;\n"
            "    return refr ? refr->period : LV_DEF_REFR_PERIOD;\n"
            "}\n\n"):
    assert m.count(old) == 1, old[:50]
    m = m.replace(old, "")
manager.write_text(m)
print("toasters moved onto SaverBase")
```

Then `grep -rn screensaver_timer_period_ms src include tests; echo "grep exit $?"` must print `grep exit 1`.

In `tests/test_helpers/screensaver_test_access.h`, delete the `tick_timer` member from `FlyingToasterScreensaverTestAccess`.

- [ ] **Step 5: Build and run the saver tests**

Run: `scripts/syntax_check.py src/ui/ui_screensaver.cpp src/ui/screensaver_manager.cpp` then `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver]"` then `python3 scripts/check_timer_destructor_cancel.py; echo "exit $?"`.
Expected: `exit 0`; every `[screensaver]` case passes, including the `FlyingToasterScreensaver ...` motion cases, `FlyingToasterScreensaver level 0: configured period, else 16 ms, with the refresh equal`, the refresh-period cases at a configured 20 ms and `each existing saver runs 16 ms at level 0, then 33 ms, without a motion jump`; the gate exits 0.

- [ ] **Step 6: Measure the toaster refactor on the Pi 3B**

```bash
ARM=task5-toasters BUILD=1 HELIX_PERF_BUILD_TREE="$PWD" BASELINES="task2-control task4-starfield" ENV_EXTRA="HELIX_SCREENSAVER_REFR_PERIOD_MS=16" \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$SS_SCRATCH/task5-toasters.log" 2>&1 < /dev/null &
```

When `task5-toasters.done` exists, apply the Task 3 Step 12 pass criteria against `task2-control`. The Pi 3B is still BASIC, so toasters fly the capped count in both arms. A failure blocks the commit.

- [ ] **Step 7: Mutate, then commit**

Mutation: in `src/ui/ui_screensaver.cpp#FlyingToasterScreensaver::sprite_limit`, change `const bool capped = m_low_tier || level >= CAPPED_LEVEL;` to `const bool capped = m_low_tier;`, rebuild, confirm the two toaster sections of `each existing saver runs 16 ms at level 0, then 33 ms, without a motion jump` fail on `sprites(ss).size() == 10`. Restore.

```bash
.venv/bin/clang-format -i include/ui_screensaver.h src/ui/ui_screensaver.cpp include/screensaver.h src/ui/screensaver_manager.cpp tests/test_helpers/screensaver_test_access.h tests/unit/test_screensaver.cpp
git add -- include/ui_screensaver.h src/ui/ui_screensaver.cpp include/screensaver.h src/ui/screensaver_manager.cpp tests/test_helpers/screensaver_test_access.h tests/unit/test_screensaver.cpp
git commit -m "refactor(screensaver): flying toasters on the shared parts, with a three-rung ladder" -m "Toasters use the shared overlay and frame timer with no canvas, on a ladder of 16 ms and 33 ms with every sprite, then 33 ms with ten; the lowest rung carries the sprite cap, which BASIC and EMBEDDED boards keep at every level until the gate lands. Every saver now runs on SaverBase, level 0 runs at 16 ms unless HELIX_SCREENSAVER_REFR_PERIOD_MS sets it, and screensaver_timer_period_ms has no caller left." -m "Mutation: sprite_limit ignored the level; both lowest-rung sprite checks went red"
```

---
### Task 6: The gate

Every running saver measures its own CPU cost. `ScreensaverManager` samples process CPU time on the display manager's idle-check tick, keeps an idle baseline, closes 5 s windows after a 1 s warm-up, and asks a pure decision function whether to keep the level, step down or mark the board too heavy. Levels are stored per saver, app version and board. Two environment switches override the budget or force a level. Toasters stop reading the platform tier; their lowest ladder rung keeps the sprite cap.

**Files:**
- Create: `include/env_whole_number.h`, `include/screensaver_cpu_clock.h`, `src/ui/screensaver_cpu_clock.cpp`, `include/screensaver_gate.h`, `src/ui/screensaver_gate.cpp`, `include/screensaver_level_store.h`, `src/ui/screensaver_level_store.cpp`
- Modify: `include/refresh_timing_env.h#refresh_timing_detail::ms_from_env`, `include/display_backend.h` (new `display_backend_key`), `include/screensaver.h` (rewritten: `SaverHost`, gate members), `src/ui/screensaver_manager.cpp` (rewritten), `include/display_manager.h` (`DisplayManager::screensaver_host`), `src/application/display_manager.cpp#DisplayManager::init`, `#DisplayManager::check_display_sleep`, `#DisplayManager::screensaver_host`, `include/ui_screensaver.h` and `src/ui/ui_screensaver.cpp` (no tier input), `tests/unit/test_screensaver.cpp` (the Task 5 host guard goes), `tests/test_helpers/screensaver_manager_test_access.h`, `docs/devel/ENVIRONMENT_VARIABLES.md`, `docs/user/CONFIGURATION.md`, `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`
- Test: `tests/unit/test_screensaver_gate.cpp` (new, pure), `tests/unit/test_screensaver_gate_manager.cpp` (new, manager), `tests/unit/application/test_display_screensaver_gate.cpp` (new, DisplayManager wiring), `[refresh_timing]` (unchanged, must stay green)

**Interfaces:**
- Consumes: Task 1 `ScreensaverInfo`, `find_screensaver`; Task 3 `SaverBase::{level, level_count, set_start_level, request_level}`, `SaverOverlay`; Task 5 (every saver is a `SaverBase`; toasters have three levels); Task 0 `arm_measure.sh`, `pi3b_service.sh ACTION=dropin|stop|start|journal|display_value|unset_display`, `pi3b_measure.sh` (`SETTLE_S`), `perf_pi3b_take`, `perf_pi3b_release`, `PERF_PACING_ENV`.
- Produces (Tasks 8, 10, 11):
  - `helix::whole_number_from_env(const char* name, uint32_t min_value, uint32_t max_value, const char* log_tag, const char* what) -> std::optional<uint32_t>`
  - `helix::ui::CpuSample { uint64_t cpu_ns; uint64_t wall_ns; }`, `using CpuClockFn = std::function<CpuSample()>;`, `CpuSample read_process_cpu_clock();`
  - `double saver_budget_share(int cores, bool printing);`, `enum class GateDecision { KEEP, STEP_DOWN, TOO_HEAVY };`, `GateDecision decide_saver_level(double share, double budget, size_t level, size_t level_count);`
  - `class IdleBaseline { SPAN_NS; MIN_SPAN_NS; void add(CpuSample); void reset(); double rate() const; size_t size() const; };`
  - `class SaverGateSession { WARMUP_NS; WINDOW_NS; void begin(CpuSample start, double baseline_rate); void restart_window(CpuSample now); std::optional<double> add(CpuSample sample); };`
  - `struct SaverEnvOverrides { std::optional<uint32_t> budget_pct; std::optional<uint32_t> level; };`, `SaverEnvOverrides saver_env_overrides();`
  - `struct BoardFacts { std::string display_backend; int cores; float bogomips; int32_t width; int32_t height; int color_depth; };`, `std::string board_fingerprint(const BoardFacts&);`
  - `struct SaverLevelEntry { size_t level; bool too_heavy; std::string version; std::string board; };`, `std::string level_store_path(const char* saver_name);`, `std::optional<SaverLevelEntry> parse_level_entry(const nlohmann::json* node);`, `nlohmann::json level_entry_json(const SaverLevelEntry&);`, `SaverLevelEntry start_entry(const std::optional<SaverLevelEntry>& stored, const std::string& version, const std::string& board, size_t level_count);`, `std::optional<SaverLevelEntry> load_level_entry(const helix::Config&, const char* saver_name);`, `void save_level_entry(helix::Config&, const char* saver_name, const SaverLevelEntry&);`
  - `inline const char* display_backend_key(DisplayBackendType type, bool gpu_accelerated);`
  - Global `struct SaverHost { std::function<bool()> is_printing; std::string display_backend; };`, `void ScreensaverManager::set_host(SaverHost host);`, `void ScreensaverManager::on_idle_check_tick();`, `static constexpr uint64_t ScreensaverManager::SAMPLE_INTERVAL_NS = 250000000;`
  - `static SaverHost DisplayManager::screensaver_host(const DisplayBackend* backend);`
  - `helix::ScreensaverManagerTestAccess`: `active(mgr) -> helix::ui::SaverBase*`, `set_cpu_clock(mgr, CpuClockFn)`, `reset_baseline(mgr)`, `baseline_rate(mgr)`, `baseline_samples(mgr)`, `showing_black_screen(mgr)`, `black_screen(mgr) -> lv_obj_t*`, `current_board(mgr) -> std::string`.

- [ ] **Step 1: Write the failing pure gate and store tests**

Create `tests/unit/test_screensaver_gate.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/scoped_env.h"
#include "config.h"
#include "display_backend.h"
#include "screensaver_cpu_clock.h"
#include "screensaver_gate.h"
#include "screensaver_level_store.h"

#include <cstdint>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::BoardFacts;
using helix::ui::CpuSample;
using helix::ui::GateDecision;
using helix::ui::IdleBaseline;
using helix::ui::SaverGateSession;
using helix::ui::SaverLevelEntry;

namespace {

constexpr uint64_t NS = 1000000000ULL;

CpuSample at(double wall_s, double cpu_s) {
    return {static_cast<uint64_t>(cpu_s * NS), static_cast<uint64_t>(wall_s * NS)};
}

} // namespace

// ============================================================================
// Budget and decision
// ============================================================================

TEST_CASE("the saver budget follows the core count and halves while printing",
          "[screensaver][screensaver_gate]") {
    using helix::ui::saver_budget_share;
    CHECK(saver_budget_share(8, false) == Catch::Approx(0.50));
    CHECK(saver_budget_share(4, false) == Catch::Approx(0.50));
    CHECK(saver_budget_share(3, false) == Catch::Approx(0.37));
    CHECK(saver_budget_share(2, false) == Catch::Approx(0.25));
    CHECK(saver_budget_share(1, false) == Catch::Approx(0.10));
    CHECK(saver_budget_share(0, false) == Catch::Approx(0.10));
    CHECK(saver_budget_share(4, true) == Catch::Approx(0.25));
    CHECK(saver_budget_share(3, true) == Catch::Approx(0.185));
    CHECK(saver_budget_share(2, true) == Catch::Approx(0.125));
    CHECK(saver_budget_share(1, true) == Catch::Approx(0.05));
}

TEST_CASE("over budget steps down one level, at the bottom it is too heavy, and it never steps up",
          "[screensaver][screensaver_gate]") {
    using helix::ui::decide_saver_level;
    CHECK(decide_saver_level(0.51, 0.50, 0, 2) == GateDecision::STEP_DOWN);
    CHECK(decide_saver_level(0.51, 0.50, 1, 2) == GateDecision::TOO_HEAVY);
    CHECK(decide_saver_level(0.90, 0.50, 2, 4) == GateDecision::STEP_DOWN);
    CHECK(decide_saver_level(0.90, 0.50, 3, 4) == GateDecision::TOO_HEAVY);
    CHECK(decide_saver_level(0.50, 0.50, 0, 2) == GateDecision::KEEP);
    // Far under budget at a lower level keeps the level: there is no step up.
    CHECK(decide_saver_level(0.01, 0.50, 1, 2) == GateDecision::KEEP);
    CHECK(decide_saver_level(0.51, 0.50, 0, 1) == GateDecision::TOO_HEAVY);
}

// ============================================================================
// Idle baseline and windows
// ============================================================================

TEST_CASE("the idle baseline is 0 under 3 s of samples and the rate over the last 10 s after",
          "[screensaver][screensaver_gate]") {
    IdleBaseline baseline;
    baseline.add(at(0.0, 0.0));
    baseline.add(at(1.0, 0.1));
    baseline.add(at(2.9, 0.29));
    CHECK(baseline.rate() == 0.0);

    baseline.add(at(3.0, 0.30));
    CHECK(baseline.rate() == Catch::Approx(0.10));

    // Ten busy seconds, then ten quiet ones: only the last ten count.
    IdleBaseline stretch;
    double cpu = 0.0;
    for (int s = 0; s <= 20; s++) {
        stretch.add(at(s, cpu));
        cpu += s < 10 ? 0.9 : 0.05;
    }
    CHECK(stretch.size() == 11);
    CHECK(stretch.rate() == Catch::Approx(0.05));

    stretch.reset();
    CHECK(stretch.size() == 0);
    CHECK(stretch.rate() == 0.0);
}

TEST_CASE("a gate session ignores the first second, then measures 5 s windows net of the baseline",
          "[screensaver][screensaver_gate]") {
    SaverGateSession session;
    session.begin(at(0.0, 0.0), 0.1);

    // A full core during the warm-up second is not counted.
    CHECK_FALSE(session.add(at(0.5, 0.5)).has_value());
    CHECK_FALSE(session.add(at(1.0, 1.0)).has_value());
    CHECK_FALSE(session.add(at(3.0, 1.8)).has_value());

    // 0.4 of a core for 5 s, less 0.1 of baseline.
    const auto first = session.add(at(6.0, 3.0));
    REQUIRE(first.has_value());
    CHECK(*first == Catch::Approx(0.3));

    const auto second = session.add(at(11.0, 4.0));
    REQUIRE(second.has_value());
    CHECK(*second == Catch::Approx(0.1));

    // A restarted window measures from the restart, with no warm-up.
    session.restart_window(at(12.0, 4.5));
    CHECK_FALSE(session.add(at(16.9, 5.9)).has_value());
    const auto third = session.add(at(17.0, 6.0));
    REQUIRE(third.has_value());
    CHECK(*third == Catch::Approx(0.2));
}

TEST_CASE("the process CPU clock moves forward with work and time", "[screensaver][screensaver_gate]") {
    const CpuSample before = helix::ui::read_process_cpu_clock();
    volatile uint64_t sum = 0;
    for (uint64_t i = 0; i < 30000000ULL; i++) {
        sum += i;
    }
    const CpuSample after = helix::ui::read_process_cpu_clock();
    CHECK(after.cpu_ns > before.cpu_ns);
    CHECK(after.wall_ns > before.wall_ns);
}

// ============================================================================
// Environment switches
// ============================================================================

TEST_CASE("screensaver gate switches parse strictly and ignore malformed values",
          "[screensaver][screensaver_gate]") {
    helix::ScopedEnv budget{"HELIX_SCREENSAVER_BUDGET_PCT"};
    helix::ScopedEnv level{"HELIX_SCREENSAVER_LEVEL"};

    unsetenv("HELIX_SCREENSAVER_BUDGET_PCT");
    unsetenv("HELIX_SCREENSAVER_LEVEL");
    CHECK_FALSE(helix::ui::saver_env_overrides().budget_pct.has_value());
    CHECK_FALSE(helix::ui::saver_env_overrides().level.has_value());

    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "25", 1);
    setenv("HELIX_SCREENSAVER_LEVEL", "2", 1);
    CHECK(helix::ui::saver_env_overrides().budget_pct == 25u);
    CHECK(helix::ui::saver_env_overrides().level == 2u);

    for (const char* bad : {"", "abc", "25%", "-5", "0", "401", " 25", "2.5"}) {
        CAPTURE(bad);
        setenv("HELIX_SCREENSAVER_BUDGET_PCT", bad, 1);
        CHECK_FALSE(helix::ui::saver_env_overrides().budget_pct.has_value());
    }
    for (const char* bad : {"", "x", "-1", "100", "1e1"}) {
        CAPTURE(bad);
        setenv("HELIX_SCREENSAVER_LEVEL", bad, 1);
        CHECK_FALSE(helix::ui::saver_env_overrides().level.has_value());
    }
}

// ============================================================================
// Level store
// ============================================================================

TEST_CASE("the display backend key names the running display path", "[screensaver][screensaver_gate]") {
    CHECK(std::string(display_backend_key(DisplayBackendType::SDL, false)) == "sdl");
    CHECK(std::string(display_backend_key(DisplayBackendType::FBDEV, false)) == "fbdev");
    CHECK(std::string(display_backend_key(DisplayBackendType::DRM, false)) == "drm");
    CHECK(std::string(display_backend_key(DisplayBackendType::DRM, true)) == "egl");
}

TEST_CASE("the board fingerprint changes with every component and rounds bogomips to 100",
          "[screensaver][screensaver_gate]") {
    const BoardFacts base{"egl", 4, 1234.0f, 800, 480, 32};
    const std::string print = helix::ui::board_fingerprint(base);
    CHECK(print == "egl/4c/1200bm/800x480/32bpp");

    BoardFacts same_bogomips = base;
    same_bogomips.bogomips = 1249.0f;
    CHECK(helix::ui::board_fingerprint(same_bogomips) == print);

    BoardFacts changed = base;
    changed.display_backend = "drm";
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.cores = 2;
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.bogomips = 1251.0f;
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.width = 1024;
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.height = 600;
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.color_depth = 16;
    CHECK(helix::ui::board_fingerprint(changed) != print);
}

TEST_CASE("a stored level is used only for the same version and board, clamped to the ladder",
          "[screensaver][screensaver_gate]") {
    const SaverLevelEntry stored{2, false, "1.2.3 (abc)", "egl/4c/0bm/800x480/32bpp"};

    SaverLevelEntry start = helix::ui::start_entry(stored, stored.version, stored.board, 4);
    CHECK(start.level == 2);
    CHECK_FALSE(start.too_heavy);

    start = helix::ui::start_entry(stored, stored.version, stored.board, 2);
    CHECK(start.level == 1);

    const SaverLevelEntry heavy{1, true, stored.version, stored.board};
    CHECK(helix::ui::start_entry(heavy, stored.version, stored.board, 2).too_heavy);

    for (const auto& [version, board] :
         {std::pair<std::string, std::string>{"1.2.4 (abd)", stored.board},
          std::pair<std::string, std::string>{stored.version, "drm/4c/0bm/800x480/32bpp"}}) {
        INFO("version " << version << ", board " << board);
        const SaverLevelEntry fresh = helix::ui::start_entry(heavy, version, board, 2);
        CHECK(fresh.level == 0);
        CHECK_FALSE(fresh.too_heavy);
        CHECK(fresh.version == version);
        CHECK(fresh.board == board);
    }
    CHECK(helix::ui::start_entry(std::nullopt, stored.version, stored.board, 2).level == 0);
}

TEST_CASE("malformed level entries are ignored and a written entry parses back",
          "[screensaver][screensaver_gate]") {
    using nlohmann::json;
    const SaverLevelEntry entry{1, true, "1.2.3 (abc)", "sdl/8c/0bm/800x480/32bpp"};
    const json written = helix::ui::level_entry_json(entry);
    CHECK(helix::ui::parse_level_entry(&written) == entry);

    const json parsed_from_text =
        json::parse(R"({"level": 1, "too_heavy": true, "version": "1.2.3 (abc)", "board": "sdl/8c/0bm/800x480/32bpp"})");
    CHECK(helix::ui::parse_level_entry(&parsed_from_text) == entry);

    CHECK_FALSE(helix::ui::parse_level_entry(nullptr).has_value());
    for (const char* bad : {
             R"("level one")",
             R"({"too_heavy": false, "version": "v", "board": "b"})",
             R"({"level": -1, "too_heavy": false, "version": "v", "board": "b"})",
             R"({"level": 1.5, "too_heavy": false, "version": "v", "board": "b"})",
             R"({"level": "1", "too_heavy": false, "version": "v", "board": "b"})",
             R"({"level": 1, "too_heavy": "no", "version": "v", "board": "b"})",
             R"({"level": 1, "too_heavy": false, "board": "b"})",
             R"({"level": 1, "too_heavy": false, "version": "v", "board": 7})",
         }) {
        CAPTURE(bad);
        const json node = json::parse(bad);
        CHECK_FALSE(helix::ui::parse_level_entry(&node).has_value());
    }
}

TEST_CASE("a level entry saved to config loads back from its path", "[screensaver][screensaver_gate]") {
    helix::Config* config = helix::Config::get_instance();
    REQUIRE(config != nullptr);
    constexpr const char* NAME = "gate_store_probe";
    CHECK(helix::ui::level_store_path(NAME) == "/display/screensaver_levels/gate_store_probe");

    const SaverLevelEntry entry{3, false, "9.9.9 (probe)", "fbdev/2c/1000bm/480x272/16bpp"};
    helix::ui::save_level_entry(*config, NAME, entry);
    CHECK(helix::ui::load_level_entry(*config, NAME) == entry);

    config->get_json("/display/screensaver_levels").erase(NAME);
    CHECK_FALSE(helix::ui::load_level_entry(*config, NAME).has_value());
}

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 2: Run the build and watch it fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; the log names `screensaver_cpu_clock.h: No such file or directory`.

- [ ] **Step 3: Write the shared strict env parser and point refresh timing at it**

Create `include/env_whole_number.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace helix {

/**
 * @brief A whole decimal number in [min_value, max_value] from the environment
 *
 * Unset reads as nullopt. A set value that is not a plain decimal number in range reads as
 * nullopt with the warning "<log_tag> Ignoring NAME='value': expected <what>, min to max".
 * strtoul skips leading whitespace and accepts a sign, so only a value whose first character
 * is a digit is parsed.
 */
inline std::optional<uint32_t> whole_number_from_env(const char* name, uint32_t min_value,
                                                     uint32_t max_value, const char* log_tag,
                                                     const char* what) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    bool ok = *value >= '0' && *value <= '9';
    unsigned long parsed = 0;
    if (ok) {
        errno = 0;
        char* end = nullptr;
        parsed = std::strtoul(value, &end, 10);
        ok = errno == 0 && *end == '\0' && parsed >= min_value && parsed <= max_value;
    }
    if (!ok) {
        spdlog::warn("{} Ignoring {}='{}': expected {}, {} to {}", log_tag, name, value, what,
                     min_value, max_value);
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

} // namespace helix
```

In `include/refresh_timing_env.h`, add `#include "env_whole_number.h"` below `#include "refresh_timing.h"` and replace the body of `refresh_timing_detail::ms_from_env` with:

```cpp
inline std::optional<uint32_t> ms_from_env(const char* name, uint32_t min_ms, uint32_t max_ms) {
    return whole_number_from_env(name, min_ms, max_ms, "[RefreshTiming]", "whole milliseconds");
}
```

(keep its doc comment `/// Whole milliseconds in [min_ms, max_ms]. ...` above it).

- [ ] **Step 4: Write the CPU clock, the gate and the level store**

Create `include/screensaver_cpu_clock.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>

namespace helix::ui {

/// CPU time the whole process has used and the steady clock, read together.
struct CpuSample {
    uint64_t cpu_ns = 0;
    uint64_t wall_ns = 0;
};

/// Source of CpuSamples; tests pass a scripted one.
using CpuClockFn = std::function<CpuSample()>;

/// Reads CLOCK_PROCESS_CPUTIME_ID, which counts every thread of the process, LVGL's draw
/// thread included, and the steady clock.
CpuSample read_process_cpu_clock();

} // namespace helix::ui
```

Create `src/ui/screensaver_cpu_clock.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_cpu_clock.h"

#include <chrono>
#include <ctime>

namespace helix::ui {

CpuSample read_process_cpu_clock() {
    CpuSample sample;
    timespec cpu{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0) {
        sample.cpu_ns = static_cast<uint64_t>(cpu.tv_sec) * 1000000000ULL +
                        static_cast<uint64_t>(cpu.tv_nsec);
    }
    sample.wall_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
    return sample;
}

} // namespace helix::ui
```

Create `include/screensaver_gate.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_cpu_clock.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

/**
 * @file screensaver_gate.h
 * @brief How much CPU a running screensaver may use, and what happens when it uses more
 *
 * Pure: the manager feeds it samples from the process CPU clock and acts on its answers.
 */

namespace helix::ui {

/// Share of one core a running saver may use: 4 or more cores 0.50, 3 cores 0.37, 2 cores 0.25,
/// otherwise 0.10 (a core count that failed to parse reads as one core). Halved while printing.
double saver_budget_share(int cores, bool printing);

enum class GateDecision {
    KEEP,      ///< within budget: the level stays
    STEP_DOWN, ///< over budget: run one level lower
    TOO_HEAVY, ///< over budget at the lowest level: the board cannot afford this saver
};

/// What a window measuring `share` of one core against `budget` means for a saver running at
/// `level` of `level_count` levels. A run never steps up.
GateDecision decide_saver_level(double share, double budget, size_t level, size_t level_count);

/// The app's CPU rate while no saver runs, over the last idle stretch of up to SPAN_NS.
class IdleBaseline {
  public:
    static constexpr uint64_t SPAN_NS = 10000000000ULL;
    /// With fewer seconds of samples than this the rate reads as 0, which can only make the
    /// gate step down early, never late.
    static constexpr uint64_t MIN_SPAN_NS = 3000000000ULL;

    void add(CpuSample sample);
    void reset();
    /// Cores of CPU per core of wall time between the oldest and newest sample held.
    double rate() const;
    size_t size() const {
        return samples_.size();
    }

  private:
    std::deque<CpuSample> samples_;
};

/// Measures one run of a saver: a warm-up, then consecutive windows for as long as it runs.
class SaverGateSession {
  public:
    static constexpr uint64_t WARMUP_NS = 1000000000ULL;
    static constexpr uint64_t WINDOW_NS = 5000000000ULL;

    /// Starts a run at `start`, with `baseline_rate` cores of idle work to subtract.
    void begin(CpuSample start, double baseline_rate);
    /// Starts a new window at `now` without a warm-up, for a level that just changed.
    void restart_window(CpuSample now);
    /// Feeds a sample; returns `(cpu delta - baseline * wall) / wall` of the window that closed
    /// at it, as a share of one core, or nullopt while none has.
    std::optional<double> add(CpuSample sample);

  private:
    CpuSample start_{};
    CpuSample window_start_{};
    bool warming_up_ = true;
    double baseline_rate_ = 0.0;
};

/// HELIX_SCREENSAVER_BUDGET_PCT (a whole percent of one core, 1 to 400) and
/// HELIX_SCREENSAVER_LEVEL (a whole level, 0 to 99); a malformed value warns and reads as unset.
struct SaverEnvOverrides {
    std::optional<uint32_t> budget_pct;
    std::optional<uint32_t> level;
};

SaverEnvOverrides saver_env_overrides();

} // namespace helix::ui
```

Create `src/ui/screensaver_gate.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_gate.h"

#include "env_whole_number.h"

namespace helix::ui {

double saver_budget_share(int cores, bool printing) {
    double share = 0.10;
    if (cores >= 4) {
        share = 0.50;
    } else if (cores == 3) {
        share = 0.37;
    } else if (cores == 2) {
        share = 0.25;
    }
    return printing ? share / 2.0 : share;
}

GateDecision decide_saver_level(double share, double budget, size_t level, size_t level_count) {
    if (share <= budget) {
        return GateDecision::KEEP;
    }
    return level + 1 < level_count ? GateDecision::STEP_DOWN : GateDecision::TOO_HEAVY;
}

void IdleBaseline::add(CpuSample sample) {
    samples_.push_back(sample);
    while (samples_.size() > 1 && sample.wall_ns - samples_.front().wall_ns > SPAN_NS) {
        samples_.pop_front();
    }
}

void IdleBaseline::reset() {
    samples_.clear();
}

double IdleBaseline::rate() const {
    if (samples_.size() < 2) {
        return 0.0;
    }
    const CpuSample& first = samples_.front();
    const CpuSample& last = samples_.back();
    const uint64_t wall = last.wall_ns - first.wall_ns;
    if (wall < MIN_SPAN_NS) {
        return 0.0;
    }
    return static_cast<double>(last.cpu_ns - first.cpu_ns) / static_cast<double>(wall);
}

void SaverGateSession::begin(CpuSample start, double baseline_rate) {
    start_ = start;
    window_start_ = start;
    warming_up_ = true;
    baseline_rate_ = baseline_rate;
}

void SaverGateSession::restart_window(CpuSample now) {
    window_start_ = now;
    warming_up_ = false;
}

std::optional<double> SaverGateSession::add(CpuSample sample) {
    if (warming_up_) {
        if (sample.wall_ns - start_.wall_ns < WARMUP_NS) {
            return std::nullopt;
        }
        warming_up_ = false;
        window_start_ = sample;
        return std::nullopt;
    }
    const uint64_t wall = sample.wall_ns - window_start_.wall_ns;
    if (wall < WINDOW_NS) {
        return std::nullopt;
    }
    const double cpu = static_cast<double>(sample.cpu_ns - window_start_.cpu_ns);
    const double wall_d = static_cast<double>(wall);
    window_start_ = sample;
    return (cpu - baseline_rate_ * wall_d) / wall_d;
}

SaverEnvOverrides saver_env_overrides() {
    return {whole_number_from_env("HELIX_SCREENSAVER_BUDGET_PCT", 1, 400, "[ScreensaverManager]",
                                  "a whole percent of one core"),
            whole_number_from_env("HELIX_SCREENSAVER_LEVEL", 0, 99, "[ScreensaverManager]",
                                  "a whole level number")};
}

} // namespace helix::ui
```

Create `include/screensaver_level_store.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix {
class Config;
}

/**
 * @file screensaver_level_store.h
 * @brief The quality level each screensaver settled on, per board and app version
 *
 * Stored in config at /display/screensaver_levels/<saver name>. An entry written by another
 * app version or on another board is ignored, so the saver is measured again from level 0.
 */

namespace helix::ui {

/// What the board fingerprint is made of.
struct BoardFacts {
    std::string display_backend; ///< "drm", "egl", "fbdev" or "sdl"
    int cores = 0;
    float bogomips = 0.0f;
    int32_t width = 0;
    int32_t height = 0;
    int color_depth = 0;
};

/// "<backend>/<cores>c/<bogomips to the nearest 100>bm/<width>x<height>/<depth>bpp"
std::string board_fingerprint(const BoardFacts& facts);

/// A saver's remembered level on one board and app version.
struct SaverLevelEntry {
    size_t level = 0;
    bool too_heavy = false;
    std::string version;
    std::string board;

    bool operator==(const SaverLevelEntry& o) const {
        return level == o.level && too_heavy == o.too_heavy && version == o.version &&
               board == o.board;
    }
};

/// "/display/screensaver_levels/<saver_name>"
std::string level_store_path(const char* saver_name);

/// The entry at `node`, or nullopt unless it is an object with an unsigned integer "level", a
/// boolean "too_heavy" and string "version" and "board".
std::optional<SaverLevelEntry> parse_level_entry(const nlohmann::json* node);

nlohmann::json level_entry_json(const SaverLevelEntry& entry);

/// Where a run starts: the stored level (clamped below `level_count`) and too-heavy mark when
/// `stored` was written by `version` on `board`, otherwise level 0 and not too heavy.
SaverLevelEntry start_entry(const std::optional<SaverLevelEntry>& stored,
                            const std::string& version, const std::string& board,
                            size_t level_count);

std::optional<SaverLevelEntry> load_level_entry(const helix::Config& config,
                                                const char* saver_name);

/// Writes the entry and saves the config file.
void save_level_entry(helix::Config& config, const char* saver_name, const SaverLevelEntry& entry);

} // namespace helix::ui
```

Create `src/ui/screensaver_level_store.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_level_store.h"

#include "config.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

std::string board_fingerprint(const BoardFacts& facts) {
    const long bogomips = std::lround(facts.bogomips / 100.0f) * 100;
    return facts.display_backend + "/" + std::to_string(facts.cores) + "c/" +
           std::to_string(bogomips) + "bm/" + std::to_string(facts.width) + "x" +
           std::to_string(facts.height) + "/" + std::to_string(facts.color_depth) + "bpp";
}

std::string level_store_path(const char* saver_name) {
    return std::string("/display/screensaver_levels/") + saver_name;
}

std::optional<SaverLevelEntry> parse_level_entry(const nlohmann::json* node) {
    if (node == nullptr || !node->is_object()) {
        return std::nullopt;
    }
    const auto level = node->find("level");
    const auto too_heavy = node->find("too_heavy");
    const auto version = node->find("version");
    const auto board = node->find("board");
    if (level == node->end() || !level->is_number_unsigned() || too_heavy == node->end() ||
        !too_heavy->is_boolean() || version == node->end() || !version->is_string() ||
        board == node->end() || !board->is_string()) {
        return std::nullopt;
    }
    SaverLevelEntry entry;
    entry.level = level->get<size_t>();
    entry.too_heavy = too_heavy->get<bool>();
    entry.version = version->get<std::string>();
    entry.board = board->get<std::string>();
    return entry;
}

nlohmann::json level_entry_json(const SaverLevelEntry& entry) {
    return nlohmann::json{{"level", entry.level},
                          {"too_heavy", entry.too_heavy},
                          {"version", entry.version},
                          {"board", entry.board}};
}

SaverLevelEntry start_entry(const std::optional<SaverLevelEntry>& stored,
                            const std::string& version, const std::string& board,
                            size_t level_count) {
    if (stored && stored->version == version && stored->board == board) {
        SaverLevelEntry entry = *stored;
        entry.level = std::min(entry.level, level_count > 0 ? level_count - 1 : 0);
        return entry;
    }
    SaverLevelEntry fresh;
    fresh.version = version;
    fresh.board = board;
    return fresh;
}

std::optional<SaverLevelEntry> load_level_entry(const helix::Config& config,
                                                const char* saver_name) {
    return parse_level_entry(config.try_get_json(level_store_path(saver_name)));
}

void save_level_entry(helix::Config& config, const char* saver_name, const SaverLevelEntry& entry) {
    config.set<nlohmann::json>(level_store_path(saver_name), level_entry_json(entry));
    config.save();
}

} // namespace helix::ui
```

In `include/display_backend.h`, insert directly after the closing brace of `display_backend_type_to_string`:

```cpp
/**
 * @brief Short lowercase name of the running display path, for keys and fingerprints
 *
 * "sdl", "fbdev", "drm", or "egl" for DRM rendering through EGL/OpenGL ES.
 */
inline const char* display_backend_key(DisplayBackendType type, bool gpu_accelerated) {
    switch (type) {
    case DisplayBackendType::SDL:
        return "sdl";
    case DisplayBackendType::FBDEV:
        return "fbdev";
    case DisplayBackendType::DRM:
        return gpu_accelerated ? "egl" : "drm";
    case DisplayBackendType::AUTO:
        return "auto";
    }
    return "unknown";
}
```

In `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`, add these three lines inside the screensaver block, keeping it alphabetical (after `screensaver_canvas.cpp`, after `screensaver_frame_timer.cpp`, and after `screensaver_gate.cpp` respectively):

```
src/ui/screensaver_cpu_clock.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_gate.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_level_store.cpp  # not in the v1 Core+AMS cut
```

- [ ] **Step 5: Run the pure tests**

Run: `python3 scripts/check_esp32_app_srcs.py; echo "exit $?"` then `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver_gate]"` then `./build/bin/helix-tests "[refresh_timing]"`.
Expected: `exit 0` twice; every `[screensaver_gate]` case passes; `[refresh_timing]` stays green.

- [ ] **Step 6: Write the failing manager and wiring tests**

Create `tests/unit/test_screensaver_gate_manager.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/screensaver_manager_test_access.h"
#include "../test_helpers/screensaver_test_access.h"
#include "config.h"
#include "helix_version.h"
#include "platform_capabilities.h"
#include "refresh_period_hold.h"
#include "screen_hide_hold.h"
#include "screensaver.h"
#include "screensaver_base.h"
#include "screensaver_gate.h"
#include "screensaver_level_store.h"

#include <cstdlib>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ScreensaverManagerTestAccess;
using helix::ui::CpuSample;
using helix::ui::SaverLevelEntry;

namespace {

constexpr const char* LEVELS_PATH = "/display/screensaver_levels";

/// Drives the shared manager's gate with a scripted CPU clock, and puts the clock, the host,
/// the stored levels and the gate environment back however the test ends.
class GateHarness {
  public:
    GateHarness() {
        mgr.stop();
        helix::Config* config = helix::Config::get_instance();
        if (const nlohmann::json* levels = config->try_get_json(LEVELS_PATH)) {
            saved_levels_ = *levels;
        }
        clear_levels();
        unsetenv("HELIX_SCREENSAVER_BUDGET_PCT");
        unsetenv("HELIX_SCREENSAVER_LEVEL");
        ScreensaverManagerTestAccess::set_cpu_clock(mgr,
                                                    [this] { return CpuSample{cpu_ns, wall_ns}; });
        ScreensaverManagerTestAccess::reset_baseline(mgr);
        mgr.set_host(SaverHost{[this] { return printing; }, "sdl"});
    }

    ~GateHarness() {
        mgr.stop();
        ScreensaverManagerTestAccess::set_cpu_clock(mgr, helix::ui::read_process_cpu_clock);
        ScreensaverManagerTestAccess::reset_baseline(mgr);
        mgr.set_host(SaverHost{});
        clear_levels();
        if (saved_levels_) {
            helix::Config::get_instance()->set<nlohmann::json>(LEVELS_PATH, *saved_levels_);
        }
    }

    GateHarness(const GateHarness&) = delete;
    GateHarness& operator=(const GateHarness&) = delete;

    /// Advances both clocks by `seconds` in 250 ms steps with the process using `cores` of CPU,
    /// calling the idle-check tick after each step.
    void run(double seconds, double cores) {
        constexpr uint64_t STEP_NS = 250000000ULL;
        const int steps = static_cast<int>(seconds * 4.0 + 0.5);
        for (int i = 0; i < steps; i++) {
            wall_ns += STEP_NS;
            cpu_ns += static_cast<uint64_t>(cores * static_cast<double>(STEP_NS));
            mgr.on_idle_check_tick();
        }
    }

    std::optional<SaverLevelEntry> stored(const char* name) const {
        return helix::ui::load_level_entry(*helix::Config::get_instance(), name);
    }

    void store(const char* name, const SaverLevelEntry& entry) {
        helix::ui::save_level_entry(*helix::Config::get_instance(), name, entry);
    }

    helix::ui::SaverBase* running() const {
        return ScreensaverManagerTestAccess::active(mgr);
    }

    ScreensaverManager& mgr = ScreensaverManager::instance();
    helix::ScopedEnv budget_env{"HELIX_SCREENSAVER_BUDGET_PCT"};
    helix::ScopedEnv level_env{"HELIX_SCREENSAVER_LEVEL"};
    uint64_t cpu_ns = 0;
    uint64_t wall_ns = 1000ULL * 1000000000ULL;
    bool printing = false;

  private:
    static void clear_levels() {
        helix::Config* config = helix::Config::get_instance();
        if (config->try_get_json(LEVELS_PATH) != nullptr) {
            config->get_json("/display").erase("screensaver_levels");
        }
    }

    std::optional<nlohmann::json> saved_levels_;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the gate subtracts the idle baseline from a running saver's windows",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "20", 1);
    gate.run(10.0, 0.05);
    CHECK(ScreensaverManagerTestAccess::baseline_rate(gate.mgr) == Catch::Approx(0.05).margin(0.001));

    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    REQUIRE(gate.running()->level() == 0);

    // 24% of a core with 5% of idle work is the saver using 19%, within the 20% budget.
    gate.run(6.25, 0.24);
    CHECK(gate.running()->level() == 0);
    CHECK_FALSE(gate.stored("toasters").has_value());

    // With no idle stretch behind it the baseline is 0, and the same load is over budget.
    gate.mgr.stop();
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    gate.run(6.25, 0.24);
    CHECK(gate.running()->level() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "an over-budget saver steps down one level, stores it, and the next run starts there",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "20", 1);
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);

    gate.run(0.75, 0.9); // the warm-up second
    CHECK(gate.running()->level() == 0);
    gate.run(5.5, 0.5);
    REQUIRE(gate.running()->level() == 1);
    CHECK(SaverTestAccess::timer(*gate.running())->period == 33);
    const std::optional<SaverLevelEntry> entry = gate.stored("toasters");
    REQUIRE(entry.has_value());
    CHECK(entry->level == 1);
    CHECK_FALSE(entry->too_heavy);
    CHECK(entry->version == helix_version_full());
    CHECK(entry->board == ScreensaverManagerTestAccess::current_board(gate.mgr));

    // Far under budget from here on: a run never steps back up.
    gate.run(20.0, 0.01);
    CHECK(gate.running()->level() == 1);

    gate.mgr.stop();
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->level() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "over budget at the lowest level stores the board as too heavy and shows a black screen",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "10", 1);
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    helix::ui::SaverBase* toasters = gate.running();
    REQUIRE(toasters != nullptr);

    gate.run(6.25, 0.9);
    REQUIRE(toasters->level() == 1);
    gate.run(5.5, 0.9);
    REQUIRE(toasters->level() == 2);
    gate.run(5.5, 0.9);

    CHECK(gate.running() == nullptr);
    CHECK_FALSE(toasters->is_active());
    CHECK(SaverTestAccess::timer(*toasters) == nullptr);
    CHECK(gate.mgr.is_active());
    REQUIRE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    lv_obj_t* black = ScreensaverManagerTestAccess::black_screen(gate.mgr);
    REQUIRE(black != nullptr);
    CHECK(lv_obj_get_parent(black) == lv_layer_top());
    CHECK(lv_obj_get_style_bg_opa(black, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(helix::active_screen_hide_hold().is_held());
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    const std::optional<SaverLevelEntry> entry = gate.stored("toasters");
    REQUIRE(entry.has_value());
    CHECK(entry->level == 2);
    CHECK(entry->too_heavy);

    gate.mgr.stop();
    CHECK_FALSE(gate.mgr.is_active());
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());

    // The next run goes straight to the black screen.
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    CHECK(gate.mgr.is_active());
    CHECK(gate.running() == nullptr);
    CHECK_FALSE(toasters->is_active());
    CHECK(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    CHECK(helix::active_screen_hide_hold().is_held());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a stored level from another app version or board, or a malformed one, starts at level 0",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    const std::string version = helix_version_full();

    SECTION("the same version and board start at the stored level") {
        gate.store("toasters", {1, false, version, board});
        gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->level() == 1);
    }
    SECTION("another app version") {
        gate.store("toasters", {1, true, "0.0.0 (other)", board});
        gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->level() == 0);
        CHECK_FALSE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    }
    SECTION("another board") {
        gate.store("toasters", {1, true, version, "fbdev/1c/0bm/1x1/16bpp"});
        gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->level() == 0);
        CHECK_FALSE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    }
    SECTION("a malformed entry") {
        helix::Config::get_instance()->set<nlohmann::json>("/display/screensaver_levels/toasters",
                                                            nlohmann::json("level one"));
        gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->level() == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "HELIX_SCREENSAVER_LEVEL forces a level with the gate off, and past the ladder is ignored",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "1", 1);
    setenv("HELIX_SCREENSAVER_LEVEL", "1", 1);
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->level() == 1);

    gate.run(30.0, 1.0);
    CHECK(gate.running() != nullptr);
    CHECK_FALSE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    CHECK_FALSE(gate.stored("toasters").has_value());

    gate.mgr.stop();
    unsetenv("HELIX_SCREENSAVER_BUDGET_PCT");
    setenv("HELIX_SCREENSAVER_LEVEL", "7", 1);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    // Toasters have levels 0 to 2; a clamp would give 2, ignoring gives the fresh level 0.
    CHECK(gate.running()->level() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the budget follows the core count and halves when a print starts mid-run",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    const double full =
        helix::ui::saver_budget_share(helix::PlatformCapabilities::detect().cpu_cores, false);
    const double load = full * 0.75;
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);

    gate.run(6.25, load);
    CHECK(gate.running()->level() == 0);

    gate.printing = true;
    gate.run(5.0, load);
    CHECK(gate.running()->level() == 1);
}

#endif // HELIX_ENABLE_SCREENSAVER
```

Create `tests/unit/application/test_display_screensaver_gate.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "screensaver.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/screensaver_manager_test_access.h"

#include "../../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

/// Gives the shared manager a scripted clock for the test and the real one back after it.
struct ScriptedGateClock {
    uint64_t wall_ns = 5000ULL * 1000000000ULL;
    ScreensaverManager& savers = ScreensaverManager::instance();

    ScriptedGateClock() {
        savers.stop();
        helix::ScreensaverManagerTestAccess::set_cpu_clock(
            savers, [this] { return helix::ui::CpuSample{0, wall_ns}; });
        helix::ScreensaverManagerTestAccess::reset_baseline(savers);
    }
    ~ScriptedGateClock() {
        helix::ScreensaverManagerTestAccess::set_cpu_clock(savers, helix::ui::read_process_cpu_clock);
        helix::ScreensaverManagerTestAccess::reset_baseline(savers);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the display manager's idle-check tick samples the screensaver gate",
                 "[application][display][screensaver_gate]") {
    ScriptedGateClock clock;
    DisplayManager mgr;

    for (int i = 0; i < 4; i++) {
        lv_display_trigger_activity(nullptr);
        mgr.check_display_sleep();
        clock.wall_ns += 300000000ULL;
    }

    CHECK(helix::ScreensaverManagerTestAccess::baseline_samples(clock.savers) == 4);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the screensaver host reports a print holding the machine and names a missing backend",
                 "[application][display][screensaver_gate]") {
    helix::PrinterState& printer_state = get_printer_state();
    helix::PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    const auto drive = [&](const char* wire_state) {
        printer_state.update_from_status(nlohmann::json{{"print_stats", {{"state", wire_state}}}});
        printer_state.set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
        process_lvgl(10);
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    };

    const SaverHost host = DisplayManager::screensaver_host(nullptr);
    CHECK(host.display_backend == "unknown");
    REQUIRE(host.is_printing);

    drive("printing");
    REQUIRE(printer_state.get_print_lifecycle() == PrintState::Printing);
    CHECK(host.is_printing());

    drive("standby");
    REQUIRE(printer_state.get_print_lifecycle() == PrintState::Idle);
    CHECK_FALSE(host.is_printing());

    helix::PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
}

#endif // HELIX_ENABLE_SCREENSAVER
```

Replace `tests/test_helpers/screensaver_manager_test_access.h` with:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screensaver.h"

#ifdef HELIX_ENABLE_SCREENSAVER

#include "platform_capabilities.h"
#include "screensaver_base.h"
#include "screensaver_level_store.h"

#include <string>
#include <utility>

namespace helix {

// Test-only seam. The manager owns its saver instances, its gate state and its clock privately.
class ScreensaverManagerTestAccess {
  public:
    /// The saver the manager is running, or nullptr (also while it shows a black screen).
    static helix::ui::SaverBase* active(const ScreensaverManager& mgr) {
        return mgr.active_;
    }

    /// Replaces the CPU clock; the next idle-check tick samples it at once.
    static void set_cpu_clock(ScreensaverManager& mgr, helix::ui::CpuClockFn clock) {
        mgr.cpu_clock_ = std::move(clock);
        mgr.sampled_ = false;
    }

    static void reset_baseline(ScreensaverManager& mgr) {
        mgr.baseline_.reset();
        mgr.sampled_ = false;
    }

    static double baseline_rate(const ScreensaverManager& mgr) {
        return mgr.baseline_.rate();
    }

    static size_t baseline_samples(const ScreensaverManager& mgr) {
        return mgr.baseline_.size();
    }

    static bool showing_black_screen(const ScreensaverManager& mgr) {
        return mgr.black_screen_type_ != ScreensaverType::OFF;
    }

    static lv_obj_t* black_screen(const ScreensaverManager& mgr) {
        return mgr.black_screen_.obj();
    }

    /// The board fingerprint a saver started now would store.
    static std::string current_board(const ScreensaverManager& mgr) {
        return helix::ui::board_fingerprint(
            mgr.board_facts(helix::PlatformCapabilities::detect()));
    }
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 7: Run the build and watch it fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; the log reports `SaverHost` was not declared and `ScreensaverManager` has no member `on_idle_check_tick`.

- [ ] **Step 8: Rewrite the manager header**

Replace `include/screensaver.h` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_registry.h"

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_cpu_clock.h"
#include "screensaver_gate.h"
#include "screensaver_overlay.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Color format of the starfield canvas
 *
 * Opaque, so the full-screen canvas draws as a straight copy instead of an alpha blend.
 * Production 32 bpp displays run ARGB8888 and copy an XRGB8888 image byte for byte, so
 * every canvas pixel's X byte must be 0xFF or that pixel draws transparent ("The alpha
 * trap" in docs/devel/GPU_ACCELERATION.md). The starfield writes every pixel itself with
 * that byte set. A canvas drawn with lv_draw_* cannot rely on it: see PIPES_CANVAS_FORMAT.
 */
inline constexpr lv_color_format_t SCREENSAVER_CANVAS_FORMAT = LV_COLOR_FORMAT_XRGB8888;

/**
 * @brief Color format of the pipes canvas
 *
 * Pipes draws antialiased segments and joints with lv_draw_*. On aarch64, LVGL's NEON
 * blends into an XRGB8888 destination write 0 into each pixel's 4th byte, and an XRGB8888
 * canvas is copied byte for byte into the ARGB8888 display, so those pixels would draw
 * transparent. An ARGB8888 canvas keeps real alpha, and it is filled opaque before anything
 * is drawn on it, so it still covers every pixel under the transparent overlay.
 */
inline constexpr lv_color_format_t PIPES_CANVAS_FORMAT = LV_COLOR_FORMAT_ARGB8888;

/**
 * @brief Row pitch in bytes lv_canvas_set_buffer() uses for a w-wide canvas in format cf
 *
 * lv_canvas_set_buffer() does not take the caller's word for packing: it
 * derives an LV_DRAW_BUF_STRIDE_ALIGN-rounded stride and sizes the canvas
 * extent from it, so a buffer allocated at w * h * bytes-per-pixel under-runs
 * the extent whenever the stride exceeds w * bytes-per-pixel
 * (prestonbrown/helixscreen#1591). The canvas screensavers size their
 * allocations and direct pixel writes from this pitch so they cannot disagree
 * with what LVGL steps rows by.
 */
inline uint32_t screensaver_canvas_stride_bytes(int32_t w, lv_color_format_t cf) {
    return lv_draw_buf_width_to_stride(static_cast<uint32_t>(w), cf);
}

class SaverBase;
struct BoardFacts;

} // namespace helix::ui

namespace helix {
struct PlatformCapabilities;
class ScreensaverManagerTestAccess;
} // namespace helix

/**
 * @brief Interface the manager drives every screensaver through
 */
class Screensaver {
  public:
    virtual ~Screensaver() = default;

    /** @brief Create overlay and begin rendering */
    virtual void start() = 0;

    /** @brief Stop rendering and destroy overlay */
    virtual void stop() = 0;

    /** @brief Check if this screensaver is currently running */
    virtual bool is_active() const = 0;

    /** @brief Return the type identifier for this screensaver */
    virtual ScreensaverType type() const = 0;
};

/// What the screensaver gate needs from the app.
struct SaverHost {
    /// True while a print job holds the machine; unset reads as not printing.
    std::function<bool()> is_printing;
    /// Running display path for the board fingerprint: "sdl", "fbdev", "drm" or "egl".
    std::string display_backend;
};

/**
 * @brief Owns the screensavers, starts the configured one, and keeps it within its CPU budget
 */
class ScreensaverManager {
  public:
    /** @brief Must not be called before LVGL initialization */
    static ScreensaverManager& instance();

    ScreensaverManager(const ScreensaverManager&) = delete;
    ScreensaverManager& operator=(const ScreensaverManager&) = delete;

    /// Samples closer together than this are skipped by on_idle_check_tick().
    static constexpr uint64_t SAMPLE_INTERVAL_NS = 250000000ULL;

    /**
     * @brief Start the specified screensaver type
     *
     * Stops any currently active screensaver first. OFF stops and starts nothing.
     *
     * The saver starts at the level stored for it on this board and app version, or at level 0
     * (HELIX_SCREENSAVER_LEVEL forces a level and turns the gate off for the run). A saver
     * stored as too heavy shows a static black screen instead.
     *
     * While a saver runs, the active screen is hidden beneath its overlay
     * (helix::ScreenHideHold), and it stays hidden across a switch between types.
     * For the same span the display refreshes at the running saver's frame period,
     * following its level; level 0 runs at HELIX_SCREENSAVER_REFR_PERIOD_MS when one is
     * configured (helix::RefreshPeriodHold). The black screen does not take that hold.
     * A saver that fails to start leaves the manager inactive, and the screen and
     * refresh period as they were before any saver ran.
     */
    void start(ScreensaverType type);

    /** @brief Stop whatever screensaver or black screen is active */
    void stop();

    /** @brief True while a screensaver or its black screen is up */
    bool is_active() const;

    /** @brief Read configured screensaver type from DisplaySettingsManager, clamped */
    static ScreensaverType configured_type();

    /// Gives the gate the app's print state and display path.
    void set_host(SaverHost host);

    /**
     * @brief Samples the process CPU clock for the gate
     *
     * Call on the display manager's idle-check tick. While no saver runs, samples feed the idle
     * baseline. While one runs they close the gate's windows: over budget steps the saver down
     * one level and stores that level; over budget at the lowest level stores the board as too
     * heavy and replaces the saver with a static black screen.
     */
    void on_idle_check_tick();

  private:
    friend class helix::ScreensaverManagerTestAccess;

    struct StartPlan {
        size_t level = 0;
        bool too_heavy = false;
        bool gated = true;
    };

    ScreensaverManager();
    ~ScreensaverManager();

    /** @brief Find screensaver instance by type, or nullptr */
    helix::ui::SaverBase* find(ScreensaverType type) const;

    /// Level, too-heavy mark and gating a run of `saver` starts with. Records the board and version.
    StartPlan plan_start(const helix::ui::SaverBase& saver, const helix::ui::ScreensaverInfo& info);

    helix::ui::BoardFacts board_facts(const helix::PlatformCapabilities& caps) const;

    /// Starts measuring the saver that just started.
    void begin_gate(const StartPlan& plan);

    /// Stops the running saver or black screen without giving back the holds.
    void end_current();

    /// Shows the static black overlay in place of `type`.
    void show_black_screen(ScreensaverType type);

    /// Stores `level` and `too_heavy` for the running saver on this board and version.
    void record_level(size_t level, bool too_heavy);

    /** @brief Take the active-screen hide hold, once however many savers run in turn */
    void hold_screen();

    /** @brief Give back the hold taken by hold_screen(), if one is out */
    void release_screen();

    /** @brief Take the refresh period hold, once however many savers run in turn */
    void hold_refresh_period();

    /** @brief Give back the hold taken by hold_refresh_period(), if one is out */
    void release_refresh_period();

    std::vector<std::unique_ptr<helix::ui::SaverBase>> screensavers_;
    helix::ui::SaverBase* active_ = nullptr;
    const helix::ui::ScreensaverInfo* active_info_ = nullptr;
    helix::ui::SaverOverlay black_screen_;
    ScreensaverType black_screen_type_ = ScreensaverType::OFF;
    bool holds_screen_ = false;
    bool holds_refresh_period_ = false;

    SaverHost host_;
    helix::ui::CpuClockFn cpu_clock_;
    helix::ui::IdleBaseline baseline_;
    helix::ui::SaverGateSession session_;
    bool gate_enabled_ = false;
    std::optional<double> budget_override_;
    int cores_ = 0;
    size_t gated_level_ = 0;
    size_t requested_level_ = 0;
    uint64_t last_sample_ns_ = 0;
    bool sampled_ = false;
    std::string version_;
    std::string board_;
};

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 9: Rewrite the manager**

Replace `src/ui/screensaver_manager.cpp` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include "config.h"
#include "display_settings_manager.h"
#include "helix_version.h"
#include "platform_capabilities.h"
#include "refresh_period_hold.h"
#include "screen_hide_hold.h"
#include "screensaver.h"
#include "screensaver_base.h"
#include "screensaver_level_store.h"
#include "screensaver_pipes.h"
#include "screensaver_starfield.h"

#include <spdlog/spdlog.h>

#include <utility>

using helix::ui::CpuSample;
using helix::ui::GateDecision;
using helix::ui::SaverLevelEntry;

ScreensaverManager& ScreensaverManager::instance() {
    static ScreensaverManager mgr;
    return mgr;
}

ScreensaverManager::ScreensaverManager() : cpu_clock_(helix::ui::read_process_cpu_clock) {
    screensavers_.push_back(std::make_unique<FlyingToasterScreensaver>());
    screensavers_.push_back(std::make_unique<StarfieldScreensaver>());
    screensavers_.push_back(std::make_unique<PipesScreensaver>());
}

ScreensaverManager::~ScreensaverManager() = default;

void ScreensaverManager::set_host(SaverHost host) {
    host_ = std::move(host);
}

void ScreensaverManager::start(ScreensaverType type) {
    if (type == ScreensaverType::OFF) {
        stop();
        return;
    }

    const bool already_running =
        (active_ && active_->type() == type && active_->is_active()) || black_screen_type_ == type;
    if (already_running) {
        return;
    }

    // The screen hold stays out until the new saver has started or failed, so the panel is
    // never uncovered between the two.
    end_current();

    helix::ui::SaverBase* saver = find(type);
    const helix::ui::ScreensaverInfo* info = helix::ui::find_screensaver(type);
    if (!saver || !info) {
        spdlog::warn("[ScreensaverManager] No screensaver registered for type {}",
                     static_cast<int>(type));
        release_screen();
        release_refresh_period();
        return;
    }

    const StartPlan plan = plan_start(*saver, *info);
    if (plan.too_heavy) {
        spdlog::warn("[ScreensaverManager] {} is stored as too heavy for board {} on {}; showing "
                     "a black screen",
                     info->name, board_, version_);
        release_refresh_period();
        show_black_screen(type);
        hold_screen();
        return;
    }

    // The saver moves the held display refresh to its level's period as it starts.
    hold_refresh_period();
    saver->set_start_level(plan.level);
    saver->start();
    if (!saver->is_active()) {
        spdlog::warn("[ScreensaverManager] Screensaver type {} did not start",
                     static_cast<int>(type));
        release_screen();
        release_refresh_period();
        return;
    }

    active_ = saver;
    active_info_ = info;
    begin_gate(plan);
    hold_screen();
    spdlog::info("[ScreensaverManager] Started screensaver type {}", static_cast<int>(type));
}

void ScreensaverManager::stop() {
    end_current();
    // The idle stretch the next run subtracts starts now.
    baseline_.reset();
    sampled_ = false;
    release_screen();
    release_refresh_period();
}

void ScreensaverManager::end_current() {
    if (active_) {
        active_->stop();
        spdlog::info("[ScreensaverManager] Stopped screensaver type {}",
                     static_cast<int>(active_->type()));
        active_ = nullptr;
        active_info_ = nullptr;
    }
    if (black_screen_type_ != ScreensaverType::OFF) {
        black_screen_.destroy();
        spdlog::info("[ScreensaverManager] Stopped screensaver type {} (black screen)",
                     static_cast<int>(black_screen_type_));
        black_screen_type_ = ScreensaverType::OFF;
    }
    gate_enabled_ = false;
}

bool ScreensaverManager::is_active() const {
    return (active_ && active_->is_active()) || black_screen_type_ != ScreensaverType::OFF;
}

ScreensaverType ScreensaverManager::configured_type() {
    const int type_int = helix::DisplaySettingsManager::instance().get_screensaver_type();
    return static_cast<ScreensaverType>(helix::ui::clamp_screensaver_type(type_int));
}

helix::ui::SaverBase* ScreensaverManager::find(ScreensaverType type) const {
    for (const auto& saver : screensavers_) {
        if (saver->type() == type) {
            return saver.get();
        }
    }
    return nullptr;
}

helix::ui::BoardFacts
ScreensaverManager::board_facts(const helix::PlatformCapabilities& caps) const {
    lv_display_t* disp = lv_display_get_default();
    helix::ui::BoardFacts facts;
    facts.display_backend = host_.display_backend.empty() ? "unknown" : host_.display_backend;
    facts.cores = caps.cpu_cores;
    facts.bogomips = caps.bogomips;
    facts.width = disp ? lv_display_get_horizontal_resolution(disp) : 0;
    facts.height = disp ? lv_display_get_vertical_resolution(disp) : 0;
    facts.color_depth = LV_COLOR_DEPTH;
    return facts;
}

ScreensaverManager::StartPlan ScreensaverManager::plan_start(const helix::ui::SaverBase& saver,
                                                             const helix::ui::ScreensaverInfo& info) {
    const helix::PlatformCapabilities caps = helix::PlatformCapabilities::detect();
    cores_ = caps.cpu_cores;
    version_ = helix_version_full();
    board_ = helix::ui::board_fingerprint(board_facts(caps));

    const helix::ui::SaverEnvOverrides env = helix::ui::saver_env_overrides();
    budget_override_.reset();
    if (env.budget_pct) {
        budget_override_ = static_cast<double>(*env.budget_pct) / 100.0;
    }
    if (env.level) {
        if (*env.level < saver.level_count()) {
            spdlog::info("[ScreensaverManager] HELIX_SCREENSAVER_LEVEL={} runs {} at that level "
                         "with the gate off",
                         *env.level, info.name);
            return {*env.level, false, false};
        }
        spdlog::warn("[ScreensaverManager] Ignoring HELIX_SCREENSAVER_LEVEL={}: {} has levels 0 "
                     "to {}",
                     *env.level, info.name, saver.level_count() - 1);
    }

    std::optional<SaverLevelEntry> stored;
    if (const helix::Config* config = helix::Config::get_instance()) {
        stored = helix::ui::load_level_entry(*config, info.name);
    }
    const SaverLevelEntry entry =
        helix::ui::start_entry(stored, version_, board_, saver.level_count());
    return {entry.level, entry.too_heavy, true};
}

void ScreensaverManager::begin_gate(const StartPlan& plan) {
    gate_enabled_ = plan.gated;
    gated_level_ = active_->level();
    requested_level_ = gated_level_;
    const CpuSample now = cpu_clock_();
    session_.begin(now, baseline_.rate());
    last_sample_ns_ = now.wall_ns;
    sampled_ = true;
}

void ScreensaverManager::on_idle_check_tick() {
    const CpuSample now = cpu_clock_();
    if (sampled_ && now.wall_ns - last_sample_ns_ < SAMPLE_INTERVAL_NS) {
        return;
    }
    last_sample_ns_ = now.wall_ns;
    sampled_ = true;

    if (!active_ || !active_->is_active()) {
        if (black_screen_type_ == ScreensaverType::OFF) {
            baseline_.add(now);
        }
        return;
    }
    if (!gate_enabled_) {
        return;
    }
    if (active_->level() != gated_level_) {
        // The saver applied a lower level: measure that level on its own.
        gated_level_ = active_->level();
        session_.restart_window(now);
        return;
    }

    const std::optional<double> share = session_.add(now);
    // While a step-down waits for the saver's next natural break, windows only keep closing.
    if (!share || requested_level_ != gated_level_) {
        return;
    }

    const bool printing = host_.is_printing && host_.is_printing();
    const double budget =
        budget_override_.value_or(helix::ui::saver_budget_share(cores_, printing));
    switch (helix::ui::decide_saver_level(*share, budget, gated_level_, active_->level_count())) {
    case GateDecision::KEEP:
        return;
    case GateDecision::STEP_DOWN:
        requested_level_ = gated_level_ + 1;
        spdlog::info("[ScreensaverManager] {} used {:.1f}% of a core against a {:.1f}% budget{}; "
                     "stepping down to level {}",
                     active_info_->name, *share * 100.0, budget * 100.0,
                     printing ? " while printing" : "", requested_level_);
        record_level(requested_level_, false);
        active_->request_level(requested_level_);
        return;
    case GateDecision::TOO_HEAVY: {
        spdlog::warn("[ScreensaverManager] {} used {:.1f}% of a core at its lowest level against a "
                     "{:.1f}% budget{}; showing a black screen until the app version or board "
                     "changes",
                     active_info_->name, *share * 100.0, budget * 100.0,
                     printing ? " while printing" : "");
        record_level(gated_level_, true);
        const ScreensaverType type = active_->type();
        active_->stop();
        active_ = nullptr;
        active_info_ = nullptr;
        release_refresh_period();
        show_black_screen(type);
        return;
    }
    }
}

void ScreensaverManager::show_black_screen(ScreensaverType type) {
    black_screen_.create();
    black_screen_type_ = type;
    gate_enabled_ = false;
}

void ScreensaverManager::record_level(size_t level, bool too_heavy) {
    helix::Config* config = helix::Config::get_instance();
    if (!config || !active_info_) {
        return;
    }
    helix::ui::save_level_entry(*config, active_info_->name, {level, too_heavy, version_, board_});
}

void ScreensaverManager::hold_screen() {
    if (!holds_screen_) {
        helix::active_screen_hide_hold().acquire(lv_screen_active());
        holds_screen_ = true;
    }
}

void ScreensaverManager::release_screen() {
    if (holds_screen_) {
        holds_screen_ = false;
        helix::active_screen_hide_hold().release();
    }
}

void ScreensaverManager::hold_refresh_period() {
    if (!holds_refresh_period_) {
        helix::active_refresh_period_hold().acquire();
        holds_refresh_period_ = true;
    }
}

void ScreensaverManager::release_refresh_period() {
    if (holds_refresh_period_) {
        holds_refresh_period_ = false;
        helix::active_refresh_period_hold().release();
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 10: Wire the display manager and drop the toasters' tier input**

In `include/display_manager.h`, directly above `class DisplayManager : public helix::ICalibrationSink {`, insert:

```cpp
#ifdef HELIX_ENABLE_SCREENSAVER
struct SaverHost;
#endif
```

and directly after `void preview_screensaver(int type);` (inside its `#ifdef HELIX_ENABLE_SCREENSAVER` block) insert:

```cpp

    /**
     * @brief What the screensaver gate needs from this app
     *
     * The print callback asks job_holds_machine() of the published print lifecycle; the
     * display path is `backend`'s display_backend_key(), or "unknown" without a backend.
     */
    static SaverHost screensaver_host(const DisplayBackend* backend);
```

In `src/application/display_manager.cpp`:

1. In `DisplayManager::init`, directly after the closing brace of the `if (m_refresh_timing.refr_period_ms != 0 || ...)` block that logs `Refresh pacing`, insert:

```cpp
#ifdef HELIX_ENABLE_SCREENSAVER
    // The gate halves the screensaver budget during prints and keys stored levels by display path.
    ScreensaverManager::instance().set_host(screensaver_host(m_backend.get()));
#endif
```

2. In `DisplayManager::check_display_sleep`, make the first statement inside its `#ifdef HELIX_ENABLE_SCREENSAVER` block (above the `HELIX_SCREENSAVER_NOW` comment):

```cpp
    ScreensaverManager::instance().on_idle_check_tick();
```

3. Directly before `void DisplayManager::preview_screensaver(int type) {` (inside the same `#ifdef HELIX_ENABLE_SCREENSAVER`), add:

```cpp
SaverHost DisplayManager::screensaver_host(const DisplayBackend* backend) {
    SaverHost host;
    host.is_printing = [] { return job_holds_machine(get_printer_state().get_print_lifecycle()); };
    host.display_backend =
        backend ? display_backend_key(backend->type(), backend->is_gpu_accelerated()) : "unknown";
    return host;
}

```

Save as `$SS_SCRATCH/toasters_no_tier.py` and run it. The lowest ladder rung keeps the sprite cap; only the tier input goes, and with it the host guard in the Task 5 ladder test:

```python
import re
from pathlib import Path

header = Path("include/ui_screensaver.h")
h = header.read_text()
# clang-format may pad the trailing comment to line up with its neighbours.
h, count = re.subn(r"^    bool m_low_tier = false; +//[^\n]*\n", "", h, flags=re.M)
assert count == 1, count
header.write_text(h)

source = Path("src/ui/ui_screensaver.cpp")
s = source.read_text()
edits = [
    ('#include "platform_capabilities.h"\n\n', ""),
    ("    const auto caps = helix::PlatformCapabilities::detect();\n    m_low_tier = !caps.supports_animations;\n\n", ""),
    ("    const bool capped = m_low_tier || level >= CAPPED_LEVEL;", "    const bool capped = level >= CAPPED_LEVEL;"),
    ("// Sprites flown at the lowest level, and at every level on a BASIC or EMBEDDED board: every\n"
     "// visible sprite costs dirty-region work each frame. OBJECTS[] is ordered by delay and wave,\n"
     "// so the first ones keep a representative mix.\n",
     "// Sprites flown at the lowest level: every visible sprite costs dirty-region work each frame.\n"
     "// OBJECTS[] is ordered by delay and wave, so the first ones keep a representative mix.\n"),
]
for old, new in edits:
    assert s.count(old) == 1, old[:60]
    s = s.replace(old, new)
assert "m_low_tier" not in s and "PlatformCapabilities" not in s
source.write_text(s)

test = Path("tests/unit/test_screensaver.cpp")
u = test.read_text()
guard = (
    "        if (!helix::PlatformCapabilities::detect().supports_animations) {\n"
    '            SKIP("BASIC and EMBEDDED hosts fly ten sprites at every level until the gate lands");\n'
    "        }\n"
)
assert u.count(guard) == 1
test.write_text(u.replace(guard, ""))
print("toasters no longer read the tier")
```

- [ ] **Step 11: Document the switches and the stored levels**

In `docs/devel/ENVIRONMENT_VARIABLES.md`, insert directly above the heading `### \`HELIX_LOOP_MIN_SLEEP_MS\``:

````markdown
### `HELIX_SCREENSAVER_BUDGET_PCT`

Replaces the screensaver gate's CPU budget, as a whole percent of one core. Each 5 s window of a running saver's CPU share, with the idle baseline subtracted, is compared with this number instead of the core-count budget (4 or more cores 50%, 3 cores 37%, 2 cores 25%, 1 core 10%, halved while a print runs), so a small value proves on a device that a saver steps down and remembers its level. Read when a saver starts.

| Property | Value |
|----------|-------|
| **Values** | Whole percent, `1` to `400` |
| **Default** | Unset: the core-count budget |
| **Invalid** | Anything else (`0`, `25%`, `-5`, empty) is ignored with a warning |
| **File** | `include/screensaver_gate.h#saver_env_overrides`, `src/ui/screensaver_manager.cpp` |

```bash
HELIX_SCREENSAVER_BUDGET_PCT=5 HELIX_SCREENSAVER_NOW=toasters ./build/bin/helix-screen --test -vv
```

Requires a build with `HELIX_ENABLE_SCREENSAVER`.

### `HELIX_SCREENSAVER_LEVEL`

Runs every saver that starts at this quality level and turns the gate off for that run: no step-down and no stored level. Level 0 is the most expensive. A level past the saver's ladder is ignored with a warning, and the stored level is used. Read when a saver starts.

| Property | Value |
|----------|-------|
| **Values** | Whole level number, `0` to `99` |
| **Default** | Unset: the stored level for this saver, app version and board |
| **Invalid** | Ignored with a warning |
| **File** | `include/screensaver_gate.h#saver_env_overrides`, `src/ui/screensaver_manager.cpp` |

```bash
HELIX_SCREENSAVER_LEVEL=1 HELIX_SCREENSAVER_NOW=pipes ./build/bin/helix-screen --test -vv
```

Requires a build with `HELIX_ENABLE_SCREENSAVER`.

````

In `docs/user/CONFIGURATION.md`, insert directly above the heading `### \`drm_device\``:

```markdown
### `screensaver_levels`
**Type:** object
**Default:** absent
**Description:** Written by HelixScreen; do not edit it. Each screensaver measures how much processor time it uses while it plays. If it would slow the printer, it lowers its frame rate or detail, and if even its lowest setting is too much it shows a plain black screen instead. The result is kept here for each screensaver, with the HelixScreen version and a description of the screen hardware, and is measured again after an update or on different hardware. Delete this entry to have every screensaver measured again.

```

- [ ] **Step 12: Build and run everything the gate touches**

Run: `scripts/syntax_check.py src/ui/screensaver_manager.cpp src/application/display_manager.cpp src/ui/ui_screensaver.cpp src/ui/screensaver_gate.cpp src/ui/screensaver_level_store.cpp src/ui/screensaver_cpu_clock.cpp` then `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver_gate]"` then `./build/bin/helix-tests "[screensaver],[refresh_timing],[refresh_period],[screen_hide],[display]"` then `make -j"$JOBS" > "$SS_SCRATCH/app-build.log" 2>&1; echo "exit $?"`.
Expected: every case passes, including the six manager gate cases and the two DisplayManager wiring cases; both builds `exit 0`.

Then a local mock run proves the manager logs a level:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy HELIX_SCREENSAVER_BUDGET_PCT=1 HELIX_SCREENSAVER_NOW=starfield \
  ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > "$SS_SCRATCH/mock-gate.log" 2>&1 &
MOCK_PID=$!
```

Wait until `grep -c 'showing a black screen' "$SS_SCRATCH/mock-gate.log"` prints `1` (about 12 s after startup; poll with a Monitor until-loop, not a sleep), then `kill "$MOCK_PID"`.
Expected in the log, in order: `Type 2 running at level 0`, `starfield used ... stepping down to level 1`, `starfield used ... showing a black screen`. `python3 -c 'import json; print(json.load(open("'"$HELIX_CONFIG_DIR"'/settings.json"))["display"]["screensaver_levels"])'` shows `starfield` with `"level": 1, "too_heavy": true`. Delete `$HELIX_CONFIG_DIR` afterwards.

- [ ] **Step 13: Prove the gate end to end on the Pi 3B, and re-run the load gate**

Preston approved device use this session (Task 3 Step 0)? If not, ask him first. Each `arm_measure.sh` arm below claims the Pi and checks the print state itself. The commands that drive the Pi directly run between `perf_pi3b_take` and `perf_pi3b_release`, and no arm is started while that claim is held; a failing `perf_pi3b_take` stops the step.

With the Task 3 Step 0 settings exported, clear any stored levels, then measure the gate build with no refresh override (the display follows each saver's level) under its default budget:

```bash
. scripts/screensaver-perf/perf_env.sh
SVC=scripts/screensaver-perf/pi3b_service.sh
perf_pi3b_take "clear stored saver levels" || exit 1
perf_run_remote "$SVC" ACTION=unset_display KEY=screensaver_levels
perf_pi3b_release
ARM=task6-gate BUILD=1 HELIX_PERF_BUILD_TREE="$PWD" BASELINES="task2-control task5-toasters" \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$SS_SCRATCH/task6-gate.log" 2>&1 < /dev/null &
```

When `task6-gate.done` exists, apply the Task 3 Step 12 pass criteria against `task5-toasters` for starfield and pipes. Toasters now fly every sprite on the Pi 3B at levels 0 and 1, since the tier no longer caps them, so their CPU is expected above `task5-toasters`; they must still pass the load gate.

The forced budget has to fall between what toasters cost at level 0 (16 ms, every sprite) and at level 1 (33 ms, every sprite), so the proof steps exactly once. Measure both rungs with the gate off, one arm after the other:

```bash
perf_pi3b_take "clear stored saver levels" || exit 1
perf_run_remote "$SVC" ACTION=unset_display KEY=screensaver_levels
perf_pi3b_release
ARM=task6-toasters-l0 BUILD=0 BIN_FROM=task6-gate ENV_EXTRA="HELIX_SCREENSAVER_LEVEL=0" WORKLOADS="idle toasters" SAVERS="toasters" \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$SS_SCRATCH/task6-toasters-l0.log" 2>&1 < /dev/null &
```

and, after `task6-toasters-l0.done`, the same with `ARM=task6-toasters-l1` and `ENV_EXTRA="HELIX_SCREENSAVER_LEVEL=1"`. Then compute the budget from the two arms' net CPU (toasters minus idle):

```bash
BUDGET=$(python3 - "$HELIX_PERF_SCRATCH/results/task6-toasters-l0.txt" "$HELIX_PERF_SCRATCH/results/task6-toasters-l1.txt" <<'PY'
import re
import statistics
import sys


def net(path):
    idle, toasters = [], []
    for line in open(path):
        match = re.match(r"^\S+ run=\d+ (idle|toasters) CPU total=([\d.]+)", line)
        if match:
            (idle if match.group(1) == "idle" else toasters).append(float(match.group(2)))
    return statistics.mean(toasters) - statistics.mean(idle)


level0, level1 = net(sys.argv[1]), net(sys.argv[2])
print(f"toasters net CPU: level 0 {level0:.1f}%, level 1 {level1:.1f}%", file=sys.stderr)
if level0 - level1 < 4.0:
    sys.exit("the two rungs differ by under 4 points of a core, so a forced budget cannot show one step")
print(round((level0 + level1) / 2))
PY
)
echo "budget: ${BUDGET:-none}"
```

An empty budget stops the proof: report the two net figures to Preston. Otherwise run it, holding the Pi until the proof is over:

```bash
perf_pi3b_take "forced-budget gate proof" || exit 1
perf_run_remote "$SVC" ACTION=unset_display KEY=screensaver_levels
perf_run_remote "$SVC" ACTION=dropin "ENV_EXTRA=$PERF_PACING_ENV HELIX_SCREENSAVER_BUDGET_PCT=$BUDGET"
perf_run_remote "$SVC" ACTION=stop
perf_run_remote "$SVC" ACTION=start
since=$(date -u '+%Y-%m-%d %H:%M:%S')
perf_run_remote scripts/screensaver-perf/pi3b_measure.sh TYPE=1 SETTLE_S=12
perf_run_remote "$SVC" ACTION=journal "SINCE=$since" "PATTERN=running at level|stepping down|black screen"
perf_run_remote "$SVC" ACTION=display_value KEY=screensaver_levels
```

`SETTLE_S=12` holds the Test Screensaver press back for 12 s with no ctl traffic, so the idle baseline the gate subtracts is the settled app and not its startup and the navigation to the button; with a baseline inflated by those, a midpoint budget can sit above the measured cost and never step. Expected: `Type 1 running at level 0 (16 ms frames` then `toasters used NN.N% of a core against a <BUDGET>.0% budget; stepping down to level 1`, no second step and no `black screen` line (level 1 costs less than the budget), and `{"toasters": {"board": "egl/4c/...", "level": 1, "too_heavy": false, "version": "..."}}`.

Restart and confirm the stored level is used:

```bash
perf_run_remote "$SVC" ACTION=stop
perf_run_remote "$SVC" ACTION=start
since=$(date -u '+%Y-%m-%d %H:%M:%S')
perf_run_remote scripts/screensaver-perf/pi3b_measure.sh TYPE=1 SETTLE_S=12
perf_run_remote "$SVC" ACTION=journal "SINCE=$since" "PATTERN=running at level"
```

Expected: `Type 1 running at level 1 (33 ms frames`. Put the Pi back and release it: `perf_run_remote "$SVC" ACTION=dropin ENV_EXTRA=`, then `perf_run_remote "$SVC" ACTION=unset_display KEY=screensaver_levels` (which restarts the app without the drop-in), then `perf_pi3b_release`. Put the net CPU figures, the budget and the two journal excerpts in the report to Preston; the Step 14 commit body states the result without this run's numbers.

- [ ] **Step 14: Mutate, then commit**

Mutation: in `SaverGateSession::add`, delete the `if (warming_up_) { ... }` block (so the warm-up second counts), rebuild, and confirm `a gate session ignores the first second, then measures 5 s windows net of the baseline` fails. Restore.

```bash
.venv/bin/clang-format -i include/env_whole_number.h include/refresh_timing_env.h include/screensaver_cpu_clock.h src/ui/screensaver_cpu_clock.cpp include/screensaver_gate.h src/ui/screensaver_gate.cpp include/screensaver_level_store.h src/ui/screensaver_level_store.cpp include/display_backend.h include/screensaver.h src/ui/screensaver_manager.cpp include/display_manager.h src/application/display_manager.cpp include/ui_screensaver.h src/ui/ui_screensaver.cpp tests/unit/test_screensaver.cpp tests/test_helpers/screensaver_manager_test_access.h tests/unit/test_screensaver_gate.cpp tests/unit/test_screensaver_gate_manager.cpp tests/unit/application/test_display_screensaver_gate.cpp
git add -- include/env_whole_number.h include/refresh_timing_env.h include/screensaver_cpu_clock.h src/ui/screensaver_cpu_clock.cpp include/screensaver_gate.h src/ui/screensaver_gate.cpp include/screensaver_level_store.h src/ui/screensaver_level_store.cpp include/display_backend.h include/screensaver.h src/ui/screensaver_manager.cpp include/display_manager.h src/application/display_manager.cpp include/ui_screensaver.h src/ui/ui_screensaver.cpp tests/unit/test_screensaver.cpp tests/test_helpers/screensaver_manager_test_access.h tests/unit/test_screensaver_gate.cpp tests/unit/test_screensaver_gate_manager.cpp tests/unit/application/test_display_screensaver_gate.cpp docs/devel/ENVIRONMENT_VARIABLES.md docs/user/CONFIGURATION.md firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(screensaver): savers measure their own CPU cost and step down to fit the board" -m "The manager samples process CPU time on the idle-check tick, subtracts the idle baseline, and compares 5 s windows after a 1 s warm-up with a per-core budget halved during prints. Over budget steps the saver down a level, and at the bottom level the run becomes a black screen. Levels are stored per saver, app version and board. HELIX_SCREENSAVER_BUDGET_PCT and HELIX_SCREENSAVER_LEVEL override the budget and the level, and toasters cap sprites only at their lowest rung, not by tier. On the Pi 3B a forced budget between toasters' level 0 and level 1 cost stepped them to 33 ms once, and the restart began at level 1." -m "Mutation: SaverGateSession counted the warm-up second; the warm-up window test went red"
```

---
### Task 7: Flying toasters for fresh installs, and the user docs

A fresh install selects flying toasters on every board that builds screensavers; the gate from Task 6 decides what the board can afford. Stored choices, including the ones config migration v16 turned off, stay as they are. The user docs and the settings template stop describing the old tier default.

**Files:**
- Modify: `include/screensaver_registry.h` (`DEFAULT_SCREENSAVER_TYPE`), `src/system/display_settings_manager.cpp#DisplaySettingsManager::init_subjects`, `docs/user/CONFIGURATION.md` (`screensaver_type`), `docs/user/guide/settings/display-sound.md` (`### Screensaver`), `config/settings.json.template` (`_screensaver_type_comment`)
- Test: `tests/unit/test_screensaver.cpp` (the tier default test becomes the fresh-install default test), `tests/unit/test_screensaver_registry.cpp` (one new case)

**Interfaces:**
- Consumes: Task 1 registry.
- Produces (Task 10 docs, Task 11): `inline constexpr ScreensaverType helix::ui::DEFAULT_SCREENSAVER_TYPE = ScreensaverType::FLYING_TOASTERS;`

- [ ] **Step 1: Write the failing default tests**

In `tests/unit/test_screensaver.cpp`, replace the whole first test case (`TEST_CASE_METHOD(LVGLTestFixture, "Screensaver defaults to tier-appropriate screensaver type when compiled in", ...)`) with:

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "a fresh install defaults the screensaver to flying toasters on every tier",
                 "[screensaver][display_settings]") {
    Config* config = Config::get_instance();
    const bool had_type = config->exists("/display/screensaver_type");
    const int stored_type = config->get<int>("/display/screensaver_type", 0);
    const bool had_legacy = config->exists("/display/screensaver_enabled");
    const bool stored_legacy = config->get<bool>("/display/screensaver_enabled", true);
    if (had_type) {
        config->get_json("/display").erase("screensaver_type");
    }
    if (had_legacy) {
        config->get_json("/display").erase("screensaver_enabled");
    }

    DisplaySettingsManager::instance().init_subjects();
    CHECK(DisplaySettingsManager::instance().get_screensaver_type() ==
          static_cast<int>(helix::ui::DEFAULT_SCREENSAVER_TYPE));
    DisplaySettingsManager::instance().deinit_subjects();

    if (had_type) {
        config->set<int>("/display/screensaver_type", stored_type);
    }
    if (had_legacy) {
        config->set<bool>("/display/screensaver_enabled", stored_legacy);
    }
}
```

In `tests/unit/test_screensaver_registry.cpp`, append:

```cpp
TEST_CASE("fresh installs default to flying toasters, a registered saver",
          "[screensaver][screensaver_registry]") {
    CHECK(helix::ui::DEFAULT_SCREENSAVER_TYPE == ScreensaverType::FLYING_TOASTERS);
    CHECK(helix::ui::find_screensaver(helix::ui::DEFAULT_SCREENSAVER_TYPE) != nullptr);
}
```

- [ ] **Step 2: Run the build and watch it fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; the log reports `DEFAULT_SCREENSAVER_TYPE` is not a member of `helix::ui`.

- [ ] **Step 3: Add the default and use it**

In `include/screensaver_registry.h`, insert directly after `inline constexpr const char* SCREENSAVER_OFF_LABEL_KEY = "Off";`:

```cpp

/// Type a fresh install runs, on every board that builds screensavers. The screensaver gate
/// measures it there and steps it down, or shows a black screen, when it costs too much.
inline constexpr ScreensaverType DEFAULT_SCREENSAVER_TYPE = ScreensaverType::FLYING_TOASTERS;
```

In `src/system/display_settings_manager.cpp#DisplaySettingsManager::init_subjects`, inside `#ifdef HELIX_ENABLE_SCREENSAVER`, replace the comment block that begins `// Screensaver type` together with the line `const int screensaver_default = PlatformCapabilities::detect().supports_animations ? 1 : 0;` with:

```cpp
    // Screensaver type. A fresh install runs the registry default on every tier; a stored
    // choice is kept as it is.
    const int screensaver_default = static_cast<int>(helix::ui::DEFAULT_SCREENSAVER_TYPE);
```

and, in the `else if (config->exists("/display/screensaver_enabled"))` branch, replace its three comment lines (beginning `// Legacy migration:`) with:

```cpp
        // Legacy screensaver_enabled: true reads as the default type, false as Off.
```

- [ ] **Step 4: Run the tests**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[display_settings],[screensaver_registry]"` then `./build/bin/helix-tests "[config][migration][v16]"`.
Expected: `exit 0`; every case passes, and the v16 migration tests are unchanged.

- [ ] **Step 5: Correct the user docs and the template**

In `docs/user/CONFIGURATION.md`, in the `### \`screensaver_type\`` section, append to the end of its **Description:** paragraph:

```markdown
 A fresh install selects Flying Toasters on every device that has screensavers. Each screensaver checks its own cost on your device and lowers its frame rate or detail, or shows a plain black screen, if it would slow the printer (see `screensaver_levels`).
```

In `docs/user/guide/settings/display-sound.md`, under `### Screensaver`, replace the two table rows

```markdown
| **Off** (default) | No screensaver — the screen dims/sleeps normally |
| **Flying Toasters** | Classic flying toasters animation |
```

with

```markdown
| **Off** | No screensaver; the screen dims and sleeps normally |
| **Flying Toasters** (default) | Classic flying toasters animation |
```

and insert this paragraph directly after the table:

```markdown
Each screensaver checks how much processor time it uses on your printer's screen. If it would slow the printer, it lowers its frame rate or detail, and if even that is too much it shows a black screen instead. It remembers the result and checks again after an update.
```

In `config/settings.json.template`, replace the value of `"_screensaver_type_comment"` with:

```json
"Screensaver shown when the display dims. 0=Off, 1=Flying Toasters (default), 2=Starfield, 3=3D Pipes. Replaces legacy screensaver_enabled boolean."
```

Run: `python3 -c 'import json; json.load(open("config/settings.json.template")); print("template ok")'`
Expected: `template ok`.

- [ ] **Step 6: Mutate, then commit**

Mutation: set `DEFAULT_SCREENSAVER_TYPE` to `ScreensaverType::STARFIELD`, rebuild, confirm `a fresh install defaults the screensaver to flying toasters on every tier` and `fresh installs default to flying toasters, a registered saver` fail; restore. Putting the tier expression back into `init_subjects` survives on a STANDARD test host, where it also yields flying toasters; the commit says so.

```bash
.venv/bin/clang-format -i include/screensaver_registry.h src/system/display_settings_manager.cpp tests/unit/test_screensaver.cpp tests/unit/test_screensaver_registry.cpp
git add -- include/screensaver_registry.h src/system/display_settings_manager.cpp tests/unit/test_screensaver.cpp tests/unit/test_screensaver_registry.cpp docs/user/CONFIGURATION.md docs/user/guide/settings/display-sound.md config/settings.json.template
git commit -m "feat(screensaver): fresh installs run flying toasters on every board with savers" -m "The default no longer depends on the platform tier: the gate measures the saver on the board and steps it down or shows a black screen when it costs too much. Stored choices, including those migration v16 turned off, are untouched. The user docs and the settings template now name the real default and the four options." -m "Mutation: DEFAULT_SCREENSAVER_TYPE set to STARFIELD; both default tests went red (the tier expression in init_subjects survives on this STANDARD test host)"
```

---

### Task 8: 1 GB quad-core boards are STANDARD tier

> **Landed early (2026-09-15):** the STANDARD threshold change shipped with the smoothness branch as `b336b2c90` (768 MB, 4 cores). Skip this task; verify the threshold at `include/platform_capabilities.h` and move on to Task 9.

`STANDARD_RAM_THRESHOLD_MB` drops from 2048 to 768 so a 1 GB Pi (about 856 MB reported) is STANDARD; four cores stay required. It lands only after the Pi 3B passes a UI check under the load gate with the new tier.

Behaviour this changes on boards with 768 MB to 2 GB and 4 or more cores (every caller of the tier found by `grep -rn 'supports_animations\|\.tier\b\|PlatformTier::' src include`):

1. `src/system/display_settings_manager.cpp#DisplaySettingsManager::init_subjects`: `animations_enabled` defaults to on when the user has no stored choice and the display is not software-rotated.
2. `src/ui/ui_panel_input_shaper.cpp#InputShaperPanel::create_chart_widgets` passes the tier to `src/ui/ui_frequency_response_chart.cpp#ui_frequency_response_chart_configure_for_platform`: charts hold 200 points instead of 50.
3. `src/system/config.cpp#migrate_v15_to_v16`: a config older than v16 upgraded on such a board keeps Flying Toasters instead of being switched off with a notice.
4. `src/ui/ui_keyboard_manager.cpp` logs the tier as `standard`; the keycap depth is unchanged because `src/ui/ui_keycap_style.cpp#decide_keycap_depth` treats BASIC and STANDARD alike.
5. Unchanged: the toaster sprite count and the screensaver default no longer read the tier (Tasks 6 and 7); crash reports, debug bundles and telemetry send RAM and core counts, not the tier; the log ring sizes by RAM.

**Files:**
- Modify: `include/platform_capabilities.h` (`STANDARD_RAM_THRESHOLD_MB`, the `PlatformTier` doc comments), `docs/devel/INPUT_SHAPER.md` (`### Tier Classification`, `### Platform Examples`, `### Constants`)
- Test: `tests/unit/test_platform_capabilities.cpp`

**Interfaces:**
- Consumes: Task 0 `arm_measure.sh` with `SAVERS="off ui"`, `pi3b_service.sh ACTION=display_value|unset_display`.
- Produces: `PlatformCapabilities::STANDARD_RAM_THRESHOLD_MB == 768`.

- [ ] **Step 1: Write the failing tier tests**

In `tests/unit/test_platform_capabilities.cpp`, replace the whole `TEST_CASE("Tier classification: boundary at exactly 2048MB", ...)` with:

```cpp
TEST_CASE("Tier classification: boundary at exactly 768MB", "[platform][tier][boundary]") {
    // 768 MB with 4 cores is STANDARD; one megabyte less is BASIC
    CHECK(PlatformCapabilities::from_metrics(768, 4, 1000.0f).tier == PlatformTier::STANDARD);
    CHECK(PlatformCapabilities::from_metrics(767, 4, 1000.0f).tier == PlatformTier::BASIC);
}

TEST_CASE("Tier classification: a 1 GB quad-core Pi is STANDARD", "[platform][tier]") {
    // A 1 GB Raspberry Pi 3B reports about 856 MB
    auto caps = PlatformCapabilities::from_metrics(856, 4, 38.0f);
    REQUIRE(caps.tier == PlatformTier::STANDARD);
    CHECK(caps.supports_animations);
    CHECK(caps.max_chart_points == 200);

    // Three cores at the same RAM stay BASIC
    CHECK(PlatformCapabilities::from_metrics(856, 3, 38.0f).tier == PlatformTier::BASIC);
}
```

- [ ] **Step 2: Run the tests and watch them fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[platform][tier]"`.
Expected: `exit 0`; `boundary at exactly 768MB` and `a 1 GB quad-core Pi is STANDARD` fail with `BASIC == STANDARD`.

- [ ] **Step 3: Lower the threshold**

In `include/platform_capabilities.h`:

- Replace `static constexpr size_t STANDARD_RAM_THRESHOLD_MB = 2048;` with `static constexpr size_t STANDARD_RAM_THRESHOLD_MB = 768;`.
- In the `BASIC` doc comment, replace ` * - RAM 512MB-2GB OR 2-3 cores` with ` * - RAM 512-767 MB OR 2-3 cores` and ` * - Examples: Raspberry Pi 3, older Pi 4 models` with ` * - Examples: 512 MB boards, 2-3 core boards`.
- In the `STANDARD` doc comment, replace ` * - RAM >= 2GB AND 4+ cores` with ` * - RAM >= 768 MB AND 4+ cores` and ` * - Examples: Raspberry Pi 4/5 (2GB+), desktop` with ` * - Examples: Raspberry Pi 3B (1 GB) and 4/5, desktop`.

In `docs/devel/INPUT_SHAPER.md`:

- Replace `| **BASIC** | 512 MB - 2 GB or 2-3 cores | 2-3 | Yes (simplified) | 50 |` with `| **BASIC** | 512-767 MB or 2-3 cores | 2-3 | Yes (simplified) | 50 |`.
- Replace `| **STANDARD** | >=2 GB and 4+ cores | 4+ | Yes (full) | 200 |` with `| **STANDARD** | >=768 MB and 4+ cores | 4+ | Yes (full) | 200 |`.
- Replace `- **BASIC**: Raspberry Pi 3 -- chart with 50 points` with `- **BASIC**: 512 MB or 2-3 core boards -- chart with 50 points`.
- Replace `- **STANDARD**: Raspberry Pi 4/5 (2 GB+), desktop -- full chart with 200 points` with `- **STANDARD**: Raspberry Pi 3B (1 GB), Pi 4/5, desktop -- full chart with 200 points`.
- Replace `static constexpr size_t STANDARD_RAM_THRESHOLD_MB = 2048;` with `static constexpr size_t STANDARD_RAM_THRESHOLD_MB = 768;`.

- [ ] **Step 4: Run the tier tests and the suites that read the tier**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[platform]"` then `./build/bin/helix-tests "[config][migration][v16],[display_settings],[screensaver]"`.
Expected: `exit 0`; every case passes.

- [ ] **Step 5: Check the Pi 3B UI under the load gate with the new tier**

Preston approved device use this session (Task 3 Step 0)? If not, ask him first. With the Task 3 Step 0 settings exported, hold the Pi while reading, and if need be clearing, the stored choice:

```bash
. scripts/screensaver-perf/perf_env.sh
perf_pi3b_take "check the stored animations choice" || exit 1
perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=display_value KEY=animations_enabled
```

If it prints anything other than `null`, the Pi carries a stored choice that hides the new default: note the value, then run `perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=unset_display KEY=animations_enabled` so the check exercises the STANDARD default (tell Preston the stored value was removed, and restore it after the check if he wants it back).

Release the Pi, then start the arm, which claims it and checks the print state itself:

```bash
perf_pi3b_release
ARM=task8-standard BUILD=1 HELIX_PERF_BUILD_TREE="$PWD" WORKLOADS="idle" SAVERS="off ui" BASELINES="task6-gate" \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$SS_SCRATCH/task8-standard.log" 2>&1 < /dev/null &
```

When `task8-standard.done` exists, confirm the Pi falls in the new tier: `perf_ssh 'grep MemTotal /proc/meminfo; nproc'` must show at least `786432 kB` (768 MB) and `4`. Pass criteria: both `off` and `ui` load-gate rows `PASS`, no crash lines, and the `idle` CPU total no more than 1.0 percentage point above `task6-gate`'s largest idle run. Ask Preston to look at the Pi 3B panel while the `ui` workload cycles panels (animations on) and say whether it looks smooth. A gate failure or his objection blocks the commit: stop and report the summary rows.

- [ ] **Step 6: Mutate, then commit**

Mutation: set `STANDARD_RAM_THRESHOLD_MB` back to `2048`, rebuild, confirm `Tier classification: boundary at exactly 768MB` and `Tier classification: a 1 GB quad-core Pi is STANDARD` fail; restore.

```bash
.venv/bin/clang-format -i include/platform_capabilities.h tests/unit/test_platform_capabilities.cpp
git add -- include/platform_capabilities.h tests/unit/test_platform_capabilities.cpp docs/devel/INPUT_SHAPER.md
git commit -m "feat(platform): 1 GB quad-core boards such as the Pi 3B are STANDARD tier" -m "STANDARD needs 768 MB and 4 cores. On those boards animations default on, input shaper charts hold 200 points, and a pre-v16 config upgrade keeps its screensaver; keycap depth, telemetry and the screensavers do not read the tier. The Pi 3B passed the load gate idle and cycling panels with animations on." -m "Mutation: STANDARD_RAM_THRESHOLD_MB back to 2048; the 768 MB boundary and 1 GB quad-core tests went red"
```

---
### Task 9: PixelWriter RGB565, ordered dither, max blending, lines; 16-bit canvas frames

Adds the RGB565 path to `PixelWriter`, a 4x4 ordered dither for fades, per-channel max blending and a Bresenham line visitor, and lets `SaverCanvas` hand out RGB565 frames. Everything is tested on plain buffers in the 32-bit host test build; the saver picks the display's format at compile time.

**Files:**
- Modify: `include/screensaver_frame.h` (`PixelFormat::RGB565`), `include/screensaver_pixel_writer.h` (rewritten), `include/screensaver_canvas.h` (`SAVER_BUILD_CANVAS_FORMAT`, `pixel_format_for`, `SaverCanvas::frame`)
- Test: `tests/unit/test_screensaver_pixel_writer.cpp` (four new cases), `tests/unit/test_screensaver_parts.cpp` (one new case), `[screensaver_fingerprint]` (unchanged)

**Interfaces:**
- Consumes: Task 4 `PixelWriter`, `FrameTarget`, `Rgb`; Task 3 `SaverCanvas`.
- Produces (Task 10):
  - `enum class PixelFormat : uint8_t { XRGB8888, RGB565 };`
  - `inline constexpr uint8_t BAYER_4X4[4][4];`, `constexpr uint8_t dither_channel(uint8_t c, uint8_t max_level, uint8_t threshold);`
  - `PixelWriter::put(int32_t, int32_t, Rgb)` for both formats, `void put_dithered(int32_t x, int32_t y, Rgb c);`, `Rgb get(int32_t, int32_t) const` for both formats, `void blend_max(int32_t x, int32_t y, Rgb c);`, `template <typename Plot> static void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Plot&& plot);`
  - `inline constexpr lv_color_format_t SAVER_BUILD_CANVAS_FORMAT;` (RGB565 when `LV_COLOR_DEPTH == 16`, else XRGB8888), `constexpr PixelFormat pixel_format_for(lv_color_format_t cf);`

- [ ] **Step 1: Write the failing RGB565, dither, blend and line tests**

Append to `tests/unit/test_screensaver_pixel_writer.cpp`:

```cpp
TEST_CASE("PixelWriter writes RGB565 as the top 5, 6 and 5 bits, little-endian",
          "[screensaver][pixel_writer]") {
    TestFrame frame(4, 2, 4 * 2 + 2, PixelFormat::RGB565);
    PixelWriter writer(frame.target);
    const size_t stride = frame.target.stride;

    writer.put(0, 0, Rgb{0x12, 0x34, 0x56});
    writer.put(1, 1, Rgb{255, 0, 0});
    writer.put(2, 1, Rgb{0, 255, 0});
    writer.put(3, 1, Rgb{0, 0, 255});

    // 0x12 -> red 2, 0x34 -> green 13, 0x56 -> blue 10: (2 << 11) | (13 << 5) | 10 = 0x11AA
    CHECK(frame.bytes[0] == 0xAA);
    CHECK(frame.bytes[1] == 0x11);
    CHECK(frame.bytes[stride + 2] == 0x00);
    CHECK(frame.bytes[stride + 3] == 0xF8);
    CHECK(frame.bytes[stride + 4] == 0xE0);
    CHECK(frame.bytes[stride + 5] == 0x07);
    CHECK(frame.bytes[stride + 6] == 0x1F);
    CHECK(frame.bytes[stride + 7] == 0x00);
    CHECK(frame.bytes[stride + 8] == UNTOUCHED); // row padding

    // Levels expand back to 8 bits by repeating their top bits.
    CHECK(writer.get(1, 1) == Rgb{255, 0, 0});
    CHECK(writer.get(2, 1) == Rgb{0, 255, 0});
    CHECK(writer.get(0, 0) == Rgb{16, 52, 82});
}

TEST_CASE("PixelWriter dithers RGB565 with a 4x4 ordered pattern and writes XRGB8888 exactly",
          "[screensaver][pixel_writer]") {
    TestFrame frame(4, 4, 8, PixelFormat::RGB565);
    PixelWriter writer(frame.target);
    for (int32_t y = 0; y < 4; y++) {
        for (int32_t x = 0; x < 4; x++) {
            writer.put_dithered(x, y, Rgb{70, 70, 70});
        }
    }

    // 70 is 8.51 red and blue levels of 31: half the cells round up, in a checkerboard.
    constexpr uint8_t RED_BLUE[4][4] = {{8, 9, 8, 9}, {9, 8, 9, 8}, {8, 9, 8, 9}, {9, 8, 9, 8}};
    // 70 is 17.29 green levels of 63: the five cells with the highest thresholds round up.
    int green_up = 0;
    for (int32_t y = 0; y < 4; y++) {
        for (int32_t x = 0; x < 4; x++) {
            CAPTURE(x, y);
            const size_t at = static_cast<size_t>(y) * 8 + static_cast<size_t>(x) * 2;
            const uint16_t v = static_cast<uint16_t>(frame.bytes[at] | frame.bytes[at + 1] << 8);
            CHECK((v >> 11) == RED_BLUE[y][x]);
            CHECK((v & 0x1F) == RED_BLUE[y][x]);
            const int green = (v >> 5) & 0x3F;
            CHECK((green == 17 || green == 18));
            green_up += green == 18 ? 1 : 0;
        }
    }
    CHECK(green_up == 5);

    // The ends of the range never dither.
    for (uint8_t t = 0; t < 16; t++) {
        CAPTURE(static_cast<int>(t));
        CHECK(helix::ui::dither_channel(0, 31, t) == 0);
        CHECK(helix::ui::dither_channel(255, 31, t) == 31);
        CHECK(helix::ui::dither_channel(255, 63, t) == 63);
    }

    TestFrame exact(1, 1, 4, PixelFormat::XRGB8888);
    PixelWriter(exact.target).put_dithered(0, 0, Rgb{70, 71, 72});
    CHECK(PixelWriter(exact.target).get(0, 0) == Rgb{70, 71, 72});
}

TEST_CASE("PixelWriter max blending brightens each channel and leaves a dimmer colour alone",
          "[screensaver][pixel_writer]") {
    SECTION("XRGB8888") {
        TestFrame frame(2, 1, 8, PixelFormat::XRGB8888);
        PixelWriter writer(frame.target);
        writer.put(0, 0, Rgb{10, 200, 30});
        writer.blend_max(0, 0, Rgb{100, 50, 40});
        CHECK(writer.get(0, 0) == Rgb{100, 200, 40});

        const std::vector<uint8_t> before = frame.bytes;
        writer.blend_max(0, 0, Rgb{5, 5, 5});
        CHECK(frame.bytes == before);
    }
    SECTION("RGB565") {
        TestFrame frame(2, 1, 4, PixelFormat::RGB565);
        PixelWriter writer(frame.target);
        writer.put(0, 0, Rgb{0, 0, 0});
        writer.blend_max(0, 0, Rgb{255, 255, 255});
        CHECK(writer.get(0, 0) == Rgb{255, 255, 255});

        writer.put(1, 0, Rgb{255, 255, 255});
        const std::vector<uint8_t> before = frame.bytes;
        writer.blend_max(1, 0, Rgb{0, 0, 0});
        CHECK(frame.bytes == before);
    }
}

TEST_CASE("PixelWriter lines visit both ends and step at most one pixel per axis",
          "[screensaver][pixel_writer]") {
    using Points = std::vector<std::pair<int32_t, int32_t>>;
    const auto trace = [](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        Points points;
        PixelWriter::line(x0, y0, x1, y1, [&](int32_t x, int32_t y) { points.emplace_back(x, y); });
        return points;
    };

    CHECK(trace(0, 0, 4, 2) == Points{{0, 0}, {1, 1}, {2, 1}, {3, 2}, {4, 2}});
    CHECK(trace(0, 0, 1, 3) == Points{{0, 0}, {0, 1}, {1, 2}, {1, 3}});
    CHECK(trace(2, 5, 2, 2) == Points{{2, 5}, {2, 4}, {2, 3}, {2, 2}});
    CHECK(trace(7, 7, 7, 7) == Points{{7, 7}});

    const std::vector<std::pair<Points::value_type, Points::value_type>> segments = {
        {{-3, 9}, {12, -4}}, {{5, 5}, {-6, 1}}, {{0, 0}, {-2, -9}}};
    for (const auto& [a, b] : segments) {
        const Points points = trace(a.first, a.second, b.first, b.second);
        INFO("from " << a.first << "," << a.second << " to " << b.first << "," << b.second);
        REQUIRE_FALSE(points.empty());
        CHECK(points.front() == a);
        CHECK(points.back() == b);
        const size_t expected = static_cast<size_t>(
            std::max(std::abs(b.first - a.first), std::abs(b.second - a.second)) + 1);
        CHECK(points.size() == expected);
        for (size_t i = 1; i < points.size(); i++) {
            CHECK(std::abs(points[i].first - points[i - 1].first) <= 1);
            CHECK(std::abs(points[i].second - points[i - 1].second) <= 1);
        }
    }
}
```

Add `#include <algorithm>`, `#include <cstdlib>` and `#include <utility>` to that file's includes.

Add `#include <utility>` to the includes of `tests/unit/test_screensaver_parts.cpp`, and append to it, before `#endif // HELIX_ENABLE_SCREENSAVER`:

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "a saver canvas frame describes its buffer in the matching pixel format",
                 "[screensaver][screensaver_parts]") {
    for (const auto& [cf, format] :
         {std::pair<lv_color_format_t, helix::ui::PixelFormat>{LV_COLOR_FORMAT_XRGB8888,
                                                              helix::ui::PixelFormat::XRGB8888},
          std::pair<lv_color_format_t, helix::ui::PixelFormat>{LV_COLOR_FORMAT_RGB565,
                                                              helix::ui::PixelFormat::RGB565}}) {
        INFO("canvas format " << static_cast<int>(cf));
        SaverOverlay overlay;
        overlay.create();
        SaverCanvas canvas;
        REQUIRE(canvas.create(overlay.obj(), 96, 40, cf));
        const helix::ui::FrameTarget frame = canvas.frame();
        CHECK(frame.format == format);
        CHECK(frame.data == canvas.data());
        CHECK(frame.stride == canvas.stride());
        CHECK(frame.w == 96u);
        CHECK(frame.h == 40u);
        canvas.release();
        overlay.destroy();
    }
    CHECK(helix::ui::pixel_format_for(LV_COLOR_FORMAT_RGB565) == helix::ui::PixelFormat::RGB565);
    CHECK(helix::ui::pixel_format_for(LV_COLOR_FORMAT_XRGB8888) == helix::ui::PixelFormat::XRGB8888);
}
```

- [ ] **Step 2: Run the build and watch it fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; the log reports `RGB565` is not a member of `helix::ui::PixelFormat`.

- [ ] **Step 3: Write the RGB565 writer**

In `include/screensaver_frame.h`, replace the `PixelFormat` enum with:

```cpp
/// How a FrameTarget stores its pixels.
enum class PixelFormat : uint8_t {
    /// 4 bytes per pixel: B, G, R, and an X byte of 0xFF
    XRGB8888,
    /// 2 bytes per pixel, little-endian: red in the top 5 bits, green in the middle 6, blue in the low 5
    RGB565,
};
```

Replace `include/screensaver_pixel_writer.h` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_frame.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace helix::ui {

/// 4x4 ordered-dither thresholds, 0 to 15, indexed [y & 3][x & 3].
inline constexpr uint8_t BAYER_4X4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/**
 * @brief An 8-bit channel quantized to 0..max_level at dither threshold `threshold` (0-15)
 *
 * floor(c * max_level / 255 + (2 * threshold + 1) / 32): over a 4x4 tile the levels average
 * back to the 8-bit value.
 */
constexpr uint8_t dither_channel(uint8_t c, uint8_t max_level, uint8_t threshold) {
    const uint32_t scaled = static_cast<uint32_t>(c) * max_level * 32u + (2u * threshold + 1u) * 255u;
    return static_cast<uint8_t>(std::min<uint32_t>(max_level, scaled / (255u * 32u)));
}

/**
 * @brief Writes colours into a FrameTarget
 *
 * No LVGL: the frame's format is data, so the host test build exercises every format
 * whatever LV_COLOR_DEPTH the app is built with. Callers keep coordinates inside the frame,
 * which contains() answers; line() visits points without that check.
 */
class PixelWriter {
  public:
    explicit PixelWriter(const FrameTarget& frame) : frame_(frame) {}

    bool contains(int32_t x, int32_t y) const {
        return x >= 0 && y >= 0 && x < static_cast<int32_t>(frame_.w) &&
               y < static_cast<int32_t>(frame_.h);
    }

    /// Writes `c` at (x, y). XRGB8888 stores B, G, R and an X byte of 0xFF; RGB565 keeps each
    /// channel's top bits.
    void put(int32_t x, int32_t y, Rgb c) {
        if (frame_.format == PixelFormat::RGB565) {
            write_565(x, y, static_cast<uint16_t>((c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3)));
            return;
        }
        uint8_t* p = pixel(x, y, 4);
        p[0] = c.b;
        p[1] = c.g;
        p[2] = c.r;
        p[3] = 0xFF;
    }

    /// Like put(), but RGB565 dithers each channel with BAYER_4X4 at (x, y), so a smooth fade
    /// shows as a fine pattern instead of bands. XRGB8888 is written exactly.
    void put_dithered(int32_t x, int32_t y, Rgb c) {
        if (frame_.format != PixelFormat::RGB565) {
            put(x, y, c);
            return;
        }
        const uint8_t t = BAYER_4X4[y & 3][x & 3];
        write_565(x, y,
                  static_cast<uint16_t>(dither_channel(c.r, 31, t) << 11 |
                                        dither_channel(c.g, 63, t) << 5 |
                                        dither_channel(c.b, 31, t)));
    }

    /// The colour at (x, y). RGB565 levels expand to 8 bits by repeating their top bits.
    Rgb get(int32_t x, int32_t y) const {
        if (frame_.format == PixelFormat::RGB565) {
            const uint8_t* p = pixel(x, y, 2);
            const auto v = static_cast<uint16_t>(p[0] | p[1] << 8);
            const auto r5 = static_cast<uint8_t>(v >> 11 & 0x1F);
            const auto g6 = static_cast<uint8_t>(v >> 5 & 0x3F);
            const auto b5 = static_cast<uint8_t>(v & 0x1F);
            return {static_cast<uint8_t>(r5 << 3 | r5 >> 2), static_cast<uint8_t>(g6 << 2 | g6 >> 4),
                    static_cast<uint8_t>(b5 << 3 | b5 >> 2)};
        }
        const uint8_t* p = pixel(x, y, 4);
        return {p[2], p[1], p[0]};
    }

    /// Raises each channel at (x, y) to at least `c`'s, so overlapping light shows its brightest
    /// part. A colour no brighter in any channel leaves the pixel untouched.
    void blend_max(int32_t x, int32_t y, Rgb c) {
        const Rgb current = get(x, y);
        if (c.r <= current.r && c.g <= current.g && c.b <= current.b) {
            return;
        }
        put_dithered(x, y, {std::max(c.r, current.r), std::max(c.g, current.g), std::max(c.b, current.b)});
    }

    /// Writes `c` over every pixel, leaving row padding alone.
    void fill(Rgb c) {
        for (int32_t y = 0; y < static_cast<int32_t>(frame_.h); y++) {
            for (int32_t x = 0; x < static_cast<int32_t>(frame_.w); x++) {
                put(x, y, c);
            }
        }
    }

    /**
     * @brief Visits every point of the line from (x0, y0) to (x1, y1), both ends included
     *
     * Bresenham's algorithm, in order from the first end: each step moves at most one pixel on
     * each axis. Points outside any frame are visited too.
     */
    template <typename Plot>
    static void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Plot&& plot) {
        const int32_t dx = std::abs(x1 - x0);
        const int32_t sx = x0 < x1 ? 1 : -1;
        const int32_t dy = -std::abs(y1 - y0);
        const int32_t sy = y0 < y1 ? 1 : -1;
        int32_t err = dx + dy;
        for (;;) {
            plot(x0, y0);
            if (x0 == x1 && y0 == y1) {
                return;
            }
            const int32_t e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

  private:
    uint8_t* pixel(int32_t x, int32_t y, size_t bytes) const {
        return frame_.data + static_cast<size_t>(y) * frame_.stride + static_cast<size_t>(x) * bytes;
    }

    void write_565(int32_t x, int32_t y, uint16_t v) {
        uint8_t* p = pixel(x, y, 2);
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>(v >> 8);
    }

    FrameTarget frame_;
};

} // namespace helix::ui
```

In `include/screensaver_canvas.h`, directly after `namespace helix::ui {`, insert:

```cpp
/// Canvas format for a saver that writes every pixel itself: the display's own depth.
inline constexpr lv_color_format_t SAVER_BUILD_CANVAS_FORMAT =
    LV_COLOR_DEPTH == 16 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_XRGB8888;

/// PixelWriter format of a canvas: RGB565 for an RGB565 canvas, XRGB8888 for a 4-byte one.
constexpr PixelFormat pixel_format_for(lv_color_format_t cf) {
    return cf == LV_COLOR_FORMAT_RGB565 ? PixelFormat::RGB565 : PixelFormat::XRGB8888;
}

```

and replace the body of `SaverCanvas::frame()` with:

```cpp
    FrameTarget frame() const {
        return {buf_, stride_, static_cast<uint32_t>(w_), static_cast<uint32_t>(h_),
                pixel_format_for(cf_)};
    }
```

(its doc comment stays: "The buffer as a frame for direct pixel writes, for a canvas in a format PixelWriter writes.").

- [ ] **Step 4: Run the tests, including the unchanged fingerprints**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[pixel_writer],[screensaver_parts],[starfield_sim]"` then `./build/bin/helix-tests "[screensaver_fingerprint]"`.
Expected: `exit 0`; every case passes; both fingerprints unchanged.

- [ ] **Step 5: Mutate, then commit**

Mutation: in `dither_channel`, change `(2u * threshold + 1u) * 255u` to `255u * 16u` (no dither), rebuild, confirm `PixelWriter dithers RGB565 with a 4x4 ordered pattern and writes XRGB8888 exactly` fails. Restore.

```bash
.venv/bin/clang-format -i include/screensaver_frame.h include/screensaver_pixel_writer.h include/screensaver_canvas.h tests/unit/test_screensaver_pixel_writer.cpp tests/unit/test_screensaver_parts.cpp
git add -- include/screensaver_frame.h include/screensaver_pixel_writer.h include/screensaver_canvas.h tests/unit/test_screensaver_pixel_writer.cpp tests/unit/test_screensaver_parts.cpp
git commit -m "feat(screensaver): PixelWriter writes RGB565 with ordered dither, max blending and lines" -m "PixelWriter now writes and reads RGB565 frames, dithers fades with a 4x4 Bayer pattern so 16-bit gradients do not band, raises channels to the brighter colour for overlapping light, and visits Bresenham lines. SaverCanvas frames carry the canvas format, and SAVER_BUILD_CANVAS_FORMAT picks the display's depth at compile time. Exact bits for both formats are tested on the 32-bit host." -m "Mutation: dither_channel rounded at a fixed half level; the ordered-dither pattern test went red"
```

---
### Task 10: Fireworks

A pure `FireworksSim` draws a night sky (computed per pixel, never stored), hills, rising shells and four burst kinds through `PixelWriter` into RGB565 or XRGB8888 frames, with one dirty box per rocket or burst and a spark pool fixed at start. `FireworksScreensaver` runs it on the saver base with the spec's four-level ladder. Fireworks becomes type 4 with a dropdown option, translations, CJK glyphs, ESP32 exclusions, a harness name and a developer doc.

**Files:**
- Create: `include/screensaver_fireworks_sim.h`, `src/ui/screensaver_fireworks_sim.cpp`, `include/screensaver_fireworks.h`, `src/ui/screensaver_fireworks.cpp`, `docs/devel/SCREENSAVERS.md`
- Modify: `include/screensaver_registry.h` (`ScreensaverType::FIREWORKS`, row), `include/screensaver_canvas.h` (`BUILD_SAVER_DEPTH`), `src/ui/screensaver_manager.cpp#ScreensaverManager::ScreensaverManager`, `include/display_settings_manager.h` (two type comments), `ui_xml/settings_display_sound_overlay.xml` (`row_screensaver`), `translations/*.yml` (9 files), `ui_xml/translations/*.xml` (regenerated), `assets/fonts/cjk/` (regenerated), `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`, `scripts/screensaver-perf/perf_env.sh#perf_saver_type`, `docs/devel/ENVIRONMENT_VARIABLES.md` (`HELIX_SCREENSAVER_NOW`), `docs/user/CONFIGURATION.md` (`screensaver_type`), `docs/user/guide/settings/display-sound.md`, `config/settings.json.template`, `docs/devel/CLAUDE.md`, `docs/README.md`, `docs/CLAUDE.md`
- Test: `tests/unit/test_screensaver_fireworks.cpp` (new), `[screensaver_registry]` (the dropdown test goes red, then green)

**Interfaces:**
- Consumes: Task 9 `PixelWriter::{put_dithered, blend_max, get, contains, line}`, `FrameTarget`, `Rgb`, `SAVER_BUILD_CANVAS_FORMAT`; Task 3 `SaverBase`, `SaverTestAccess`, `SAVER_FAST_PERIOD`; Task 1 registry; Task 6 `ScreensaverManagerTestAccess::active`; `helix::ui::screensaver::{random_below, unit_random}`.
- Produces (Task 11):
  - `ScreensaverType::FIREWORKS = 4`; registry row `{FIREWORKS, "fireworks", "Fireworks", SAVER_DEPTH_16 | SAVER_DEPTH_32}`
  - `struct FireworksLevel { uint32_t period_ms; uint16_t sparks_per_burst; uint8_t trail; uint8_t bursts_at_once; };`, `inline constexpr FireworksLevel FIREWORKS_LEVELS[]`, `inline constexpr size_t FIREWORKS_LEVEL_COUNT`
  - `struct FireworksPacing { uint32_t first_launch_ms = 300; uint32_t min_gap_ms = 800; uint32_t max_gap_ms = 2500; uint32_t finale_every_ms = 180000; uint8_t finale_min_shells = 6; uint8_t finale_max_shells = 10; uint32_t finale_span_ms = 4000; };`
  - `class FireworksSim { static constexpr size_t MAX_BURSTS = 6; static constexpr int STAR_COUNT = 80; void init(FrameTarget&, std::minstd_rand&, size_t level, const FireworksPacing& = {}); void step(uint32_t dt_ms, FrameTarget&, std::minstd_rand&, std::vector<DirtyRect>& dirty); void request_level(size_t); size_t level() const; size_t spark_capacity() const; size_t sparks_in_use() const; void clear(); Rgb sky_at(int32_t x, int32_t y) const; };`
  - `class FireworksScreensaver : public helix::ui::SaverBase`
  - `inline constexpr uint8_t helix::ui::BUILD_SAVER_DEPTH;`
  - Harness: `perf_saver_type fireworks` prints `4`.

- [ ] **Step 1: Write the failing fireworks tests**

Create `tests/unit/test_screensaver_fireworks.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/screensaver_manager_test_access.h"
#include "../test_helpers/screensaver_test_access.h"
#include "config.h"
#include "screensaver.h"
#include "screensaver_canvas.h"
#include "screensaver_fireworks.h"
#include "screensaver_fireworks_sim.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_registry.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FIREWORKS_LEVEL_COUNT;
using helix::ui::FIREWORKS_LEVELS;
using helix::ui::FireworksPacing;
using helix::ui::FireworksSim;
using helix::ui::FrameTarget;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

namespace {

constexpr uint32_t W = 320;
constexpr uint32_t H = 200;

uint32_t bytes_per_pixel(PixelFormat format) {
    return format == PixelFormat::RGB565 ? 2 : 4;
}

/// A heap frame whose rows are wider than w * bytes per pixel, as LVGL's aligned strides can be.
struct SimFrame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    explicit SimFrame(PixelFormat format) {
        const uint32_t bpp = bytes_per_pixel(format);
        const uint32_t stride = W * bpp + 3 * bpp;
        bytes.assign(static_cast<size_t>(stride) * H, 0);
        target = {bytes.data(), stride, W, H, format};
    }
    SimFrame(const SimFrame&) = delete;
    SimFrame& operator=(const SimFrame&) = delete;
};

/// A busy sky: a shell every 150 to 400 ms and a finale every 6 s.
FireworksPacing busy_pacing() {
    FireworksPacing pacing;
    pacing.first_launch_ms = 50;
    pacing.min_gap_ms = 150;
    pacing.max_gap_ms = 400;
    pacing.finale_every_ms = 6000;
    pacing.finale_span_ms = 1500;
    return pacing;
}

bool box_covers(const std::vector<DirtyRect>& boxes, int32_t x, int32_t y) {
    return std::any_of(boxes.begin(), boxes.end(), [&](const DirtyRect& b) {
        return x >= b.x1 && x <= b.x2 && y >= b.y1 && y <= b.y2;
    });
}

struct Diff {
    size_t changed = 0;
    size_t uncovered = 0;
};

/// Pixels of `frame` that differ from `before`, and how many of them no box in `boxes` covers.
Diff diff_frame(const std::vector<uint8_t>& before, const SimFrame& frame,
                const std::vector<DirtyRect>& boxes) {
    Diff diff;
    const uint32_t bpp = bytes_per_pixel(frame.target.format);
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            const size_t i = static_cast<size_t>(y) * frame.target.stride + x * bpp;
            if (std::memcmp(&before[i], &frame.bytes[i], bpp) == 0) {
                continue;
            }
            diff.changed++;
            diff.uncovered += box_covers(boxes, static_cast<int32_t>(x), static_cast<int32_t>(y)) ? 0 : 1;
        }
    }
    return diff;
}

/// Pixels brighter than any sky or star colour: a spark or a rocket.
size_t lit_pixels(const SimFrame& frame) {
    const PixelWriter writer(frame.target);
    size_t count = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(H); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(W); x++) {
            const Rgb c = writer.get(x, y);
            count += (c.r > 120 || c.g > 120) ? 1 : 0;
        }
    }
    return count;
}

int brightness(Rgb c) {
    return c.r + c.g + c.b;
}

void fire(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

} // namespace

// ============================================================================
// FireworksSim
// ============================================================================

TEST_CASE("the fireworks sky is a gradient with faint stars over rolling hills, in either format",
          "[screensaver][fireworks_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    std::minstd_rand rng(3);
    FireworksSim sim;
    SimFrame frame(format);
    sim.init(frame.target, rng, 0);

    // The frame holds exactly the computed sky, dithered as a redraw dithers it.
    SimFrame expected(format);
    PixelWriter painter(expected.target);
    for (int32_t y = 0; y < static_cast<int32_t>(H); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(W); x++) {
            painter.put_dithered(x, y, sim.sky_at(x, y));
        }
    }
    CHECK(frame.bytes == expected.bytes);

    const Rgb ground = sim.sky_at(0, static_cast<int32_t>(H) - 1);
    int32_t lowest_top = static_cast<int32_t>(H);
    int32_t highest_top = 0;
    size_t stars = 0;
    int32_t starless_column = -1;
    int32_t starless_top = 0;
    for (int32_t x = 0; x < static_cast<int32_t>(W); x++) {
        int32_t top = static_cast<int32_t>(H) - 1;
        while (top > 0 && sim.sky_at(x, top - 1) == ground) {
            top--;
        }
        lowest_top = std::min(lowest_top, top);
        highest_top = std::max(highest_top, top);
        size_t column_stars = 0;
        for (int32_t y = 0; y < top; y++) {
            column_stars += sim.sky_at(x, y).r >= 40 ? 1 : 0;
        }
        stars += column_stars;
        if (column_stars == 0 && starless_column < 0) {
            starless_column = x;
            starless_top = top;
        }
    }
    CHECK(highest_top > lowest_top);                  // rolling, not flat
    CHECK(lowest_top > static_cast<int32_t>(H) / 2);  // the hills stay low
    CHECK(stars > 0);
    CHECK(stars <= static_cast<size_t>(FireworksSim::STAR_COUNT));
    REQUIRE(starless_column >= 0);
    // Darker at the top of the sky than just above the hills.
    CHECK(brightness(sim.sky_at(starless_column, 0)) <
          brightness(sim.sky_at(starless_column, starless_top - 1)));

    if (format == PixelFormat::XRGB8888) {
        size_t not_opaque = 0;
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                not_opaque += frame.bytes[static_cast<size_t>(y) * frame.target.stride + x * 4 + 3] != 0xFF ? 1 : 0;
            }
        }
        CHECK(not_opaque == 0);
    }
}

TEST_CASE("a fireworks show replays exactly from a seed, and another seed differs",
          "[screensaver][fireworks_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const auto run = [format](uint32_t seed) {
        std::minstd_rand rng(seed);
        FireworksSim sim;
        SimFrame frame(format);
        sim.init(frame.target, rng, 0, busy_pacing());
        std::vector<std::vector<DirtyRect>> boxes;
        std::vector<DirtyRect> dirty;
        constexpr uint32_t FRAME_MS[] = {16, 33, 50, 7, 100};
        for (int i = 0; i < 400; i++) {
            sim.step(FRAME_MS[i % 5], frame.target, rng, dirty);
            boxes.push_back(dirty);
        }
        return std::make_pair(frame.bytes, boxes);
    };

    const auto first = run(9);
    CHECK(run(9) == first);
    CHECK_FALSE(run(10).first == first.first);
}

TEST_CASE("every pixel a fireworks frame changes lies in one of its dirty boxes",
          "[screensaver][fireworks_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    std::minstd_rand rng(21);
    FireworksSim sim;
    SimFrame frame(format);
    sim.init(frame.target, rng, 0, busy_pacing());

    std::vector<DirtyRect> dirty;
    size_t changed = 0;
    size_t uncovered = 0;
    size_t most_sparks = 0;
    size_t most_boxes = 0;
    size_t most_lit = 0;
    for (int i = 0; i < 700; i++) {
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(16, frame.target, rng, dirty);
        const Diff diff = diff_frame(before, frame, dirty);
        changed += diff.changed;
        uncovered += diff.uncovered;
        most_boxes = std::max(most_boxes, dirty.size());
        most_sparks = std::max(most_sparks, sim.sparks_in_use());
        if (i % 50 == 25) {
            most_lit = std::max(most_lit, lit_pixels(frame));
        }
    }
    REQUIRE(changed > 0);
    REQUIRE(most_sparks > 0);
    CHECK(uncovered == 0);
    CHECK(most_boxes > 1);
    CHECK(most_boxes <= 2 * FireworksSim::MAX_BURSTS);
    CHECK(most_lit > 0);
}

TEST_CASE("the spark pool is sized at start, never grows, and a full pool drops sparks",
          "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(5);
    FireworksSim sim;
    SimFrame frame(PixelFormat::XRGB8888);

    sim.init(frame.target, rng, 0, busy_pacing());
    CHECK(sim.spark_capacity() == static_cast<size_t>(FIREWORKS_LEVELS[0].sparks_per_burst) *
                                      FIREWORKS_LEVELS[0].bursts_at_once);

    // Sized for the cheapest level, then asked for the most expensive: one burst fills the pool.
    const size_t cheapest = FIREWORKS_LEVEL_COUNT - 1;
    sim.init(frame.target, rng, cheapest, busy_pacing());
    const size_t capacity = sim.spark_capacity();
    REQUIRE(capacity == static_cast<size_t>(FIREWORKS_LEVELS[cheapest].sparks_per_burst) *
                            FIREWORKS_LEVELS[cheapest].bursts_at_once);
    sim.request_level(0);

    std::vector<DirtyRect> dirty;
    size_t most = 0;
    size_t capacity_changes = 0;
    for (int i = 0; i < 1200; i++) {
        sim.step(16, frame.target, rng, dirty);
        capacity_changes += sim.spark_capacity() != capacity ? 1 : 0;
        most = std::max(most, sim.sparks_in_use());
    }
    CHECK(sim.level() == 0);
    CHECK(capacity_changes == 0);
    // Bursts after the pool filled found it full and dropped what did not fit.
    CHECK(most == capacity);
}

TEST_CASE("a fireworks level request waits for the next shell launch", "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(8);
    FireworksSim sim;
    SimFrame frame(PixelFormat::RGB565);
    FireworksPacing pacing;
    pacing.first_launch_ms = 300;
    pacing.min_gap_ms = 1000;
    pacing.max_gap_ms = 1000;
    pacing.finale_every_ms = 600000;
    sim.init(frame.target, rng, 0, pacing);
    std::vector<DirtyRect> dirty;
    const auto run_ms = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 20) {
            sim.step(20, frame.target, rng, dirty);
        }
    };

    run_ms(400); // the first shell launched at 300 ms
    REQUIRE(sim.level() == 0);
    sim.request_level(2);
    run_ms(800); // 1200 ms; the next launch is due at 1300 ms
    CHECK(sim.level() == 0);
    run_ms(200); // 1400 ms
    CHECK(sim.level() == 2);

    sim.request_level(9); // clamped to the last level, again at the next launch
    run_ms(800);          // 2200 ms
    CHECK(sim.level() == 2);
    run_ms(200); // 2400 ms
    CHECK(sim.level() == FIREWORKS_LEVEL_COUNT - 1);
}

// ============================================================================
// FireworksScreensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "the fireworks saver draws on a canvas in the build's format with the fireworks ladder",
                 "[screensaver][screensaver_fireworks]") {
    FireworksScreensaver ss;
    ScreensaverStopOnExit<FireworksScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss));
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == helix::ui::SAVER_BUILD_CANVAS_FORMAT);
    CHECK(ss.level_count() == FIREWORKS_LEVEL_COUNT);
    CHECK(FIREWORKS_LEVELS[0].period_ms == helix::ui::SAVER_FAST_PERIOD);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FireworksScreensaver level 0: configured period, else 16 ms, with the refresh equal",
                 "[screensaver][screensaver_fireworks]") {
    const uint32_t configured_ms = GENERATE(as<uint32_t>{}, 0, 20);
    INFO("configured period " << configured_ms << " ms");
    const LevelZeroPeriods periods = level_zero_periods<FireworksScreensaver>(configured_ms);
    CHECK(periods.timer_ms ==
          (configured_ms != 0 ? configured_ms : FIREWORKS_LEVELS[0].period_ms));
    CHECK(periods.refresh_ms == periods.timer_ms);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a fireworks level request changes the frame period when the next shell launches",
                 "[screensaver][screensaver_fireworks]") {
    FireworksScreensaver ss;
    ScreensaverStopOnExit<FireworksScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 4);
    ss.start();
    REQUIRE(ss.is_active());

    ss.request_level(3);
    CHECK(ss.level() == 0);
    int frames = 0;
    while (ss.level() != 3 && frames < 400) {
        lv_tick_inc(50);
        fire(SaverTestAccess::timer(ss));
        frames++;
    }
    REQUIRE(ss.level() == 3);
    CHECK(frames > 0);
    CHECK(SaverTestAccess::timer(ss)->period == FIREWORKS_LEVELS[3].period_ms);
}

TEST_CASE_METHOD(LVGLTestFixture, "every registered saver this build draws starts from the manager",
                 "[screensaver][screensaver_fireworks]") {
    helix::ScopedEnv budget{"HELIX_SCREENSAVER_BUDGET_PCT"};
    helix::ScopedEnv level{"HELIX_SCREENSAVER_LEVEL"};
    unsetenv("HELIX_SCREENSAVER_BUDGET_PCT");
    unsetenv("HELIX_SCREENSAVER_LEVEL");
    helix::Config* config = helix::Config::get_instance();
    if (config->try_get_json("/display/screensaver_levels") != nullptr) {
        config->get_json("/display").erase("screensaver_levels");
    }

    auto& mgr = ScreensaverManager::instance();
    size_t started = 0;
    for (const helix::ui::ScreensaverInfo& info : helix::ui::SCREENSAVERS) {
        CAPTURE(info.name);
        if ((info.depths & helix::ui::BUILD_SAVER_DEPTH) == 0) {
            continue;
        }
        mgr.start(info.type);
        CHECK(mgr.is_active());
        CHECK(helix::ScreensaverManagerTestAccess::active(mgr) != nullptr);
        started += helix::ScreensaverManagerTestAccess::active(mgr) != nullptr ? 1 : 0;
        mgr.stop();
    }
    CHECK(started > 0);
    const helix::ui::ScreensaverInfo* fireworks = helix::ui::find_screensaver_by_name("fireworks");
    REQUIRE(fireworks != nullptr);
    CHECK(fireworks->type == ScreensaverType::FIREWORKS);
    CHECK((fireworks->depths & helix::ui::SAVER_DEPTH_16) != 0);
    CHECK((fireworks->depths & helix::ui::SAVER_DEPTH_32) != 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
```

- [ ] **Step 2: Run the build and watch it fail**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"`
Expected: `exit 2`; the log names `screensaver_fireworks.h: No such file or directory`.

- [ ] **Step 3: Write the simulation header**

Create `include/screensaver_fireworks_sim.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_frame.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_fireworks_sim.h
 * @brief A fireworks show over hills under a night sky, stepped one frame at a time
 *
 * No LVGL, no clock, no shared random sequence and no allocation after init(): the saver
 * passes the frame time, its own random sequence and the frame, so a seed replays exactly and
 * the show runs in tests without a display.
 */

namespace helix::ui {

class PixelWriter;

/// One level of the fireworks ladder. Every tunable a level changes is here.
struct FireworksLevel {
    uint32_t period_ms;        ///< frame period in ms
    uint16_t sparks_per_burst;
    uint8_t trail;             ///< trail points behind a trailing spark's head
    uint8_t bursts_at_once;    ///< shells in flight plus bursts still burning
};

/// The ladder, most expensive level first. A requested level applies when the next shell launches.
inline constexpr FireworksLevel FIREWORKS_LEVELS[] = {
    {SAVER_FAST_PERIOD, 150, 5, 6},
    {33, 150, 5, 6},
    {33, 90, 3, 4},
    {50, 50, 0, 3},
};

inline constexpr size_t FIREWORKS_LEVEL_COUNT = std::size(FIREWORKS_LEVELS);

/// When shells launch. The defaults are the show's pacing; tests shorten them.
struct FireworksPacing {
    uint32_t first_launch_ms = 300;
    uint32_t min_gap_ms = 800;
    uint32_t max_gap_ms = 2500;
    uint32_t finale_every_ms = 180000;
    uint8_t finale_min_shells = 6;
    uint8_t finale_max_shells = 10;
    uint32_t finale_span_ms = 4000;
};

class FireworksSim {
  public:
    /// Most shells in flight plus bursts burning, at any level.
    static constexpr size_t MAX_BURSTS = 6;
    /// Faint fixed stars in the sky.
    static constexpr int STAR_COUNT = 80;

    enum class BurstKind : uint8_t { PEONY, CHRYSANTHEMUM, WILLOW, RING };

    /**
     * @brief Lays out the sky for `frame`, sizes the spark pool for `level`, and paints the sky
     *
     * The pool holds sparks_per_burst * bursts_at_once sparks of `level` and never grows; a
     * burst that finds it full drops the sparks that do not fit.
     */
    void init(FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const FireworksPacing& pacing = {});

    /**
     * @brief Advances the show by `dt_ms` and draws it into `frame`
     *
     * Replaces `dirty` with one box per rocket or burst whose pixels changed: what it erased
     * from the previous frame and what it drew in this one.
     */
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
              std::vector<DirtyRect>& dirty);

    /// Runs at `level` (clamped to the ladder) from the next shell launch on.
    void request_level(size_t level);

    /// Level the show runs at now.
    size_t level() const {
        return level_;
    }

    /// Sparks the pool holds, fixed from init() to clear().
    size_t spark_capacity() const {
        return sparks_.size();
    }

    /// Sparks alive now.
    size_t sparks_in_use() const;

    /// Frees the spark pool and the sky layout.
    void clear();

    /// Background at (x, y) inside the frame: the hills, a star, or the sky gradient.
    Rgb sky_at(int32_t x, int32_t y) const;

  private:
    enum SparkFlag : uint8_t {
        SPARK_ALIVE = 1u << 0,
        SPARK_DRAWN = 1u << 1,     ///< drawn in the last frame, so the next frame erases it
        SPARK_DRAWN_BIG = 1u << 2, ///< drawn 2x2 in the last frame
    };

    struct Spark {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f; ///< px per second
        float vy = 0.0f;
        uint16_t age_ms = 0;
        uint16_t life_ms = 0;
        Rgb color{};
        uint8_t flags = 0;
        int16_t drawn_x = 0; ///< head drawn in the last frame
        int16_t drawn_y = 0;
        int8_t drawn_dx = 0; ///< unit direction the trail runs in, times 100
        int8_t drawn_dy = 0;
        uint8_t burst = 0; ///< owning burst slot
    };
    // 900 sparks at level 0 keep the pool near 30 KB.
    static_assert(sizeof(Spark) <= 32, "a spark must stay within 32 bytes");

    struct Burst {
        bool active = false;
        BurstKind kind = BurstKind::PEONY;
        uint8_t trail = 0;
        uint16_t alive = 0;
        float drag = 0.0f;    ///< share of velocity lost per second
        float gravity = 0.0f; ///< px/s^2
    };

    struct Rocket {
        bool active = false;
        bool drawn = false;
        BurstKind kind = BurstKind::PEONY;
        uint8_t trail = 0;
        uint16_t sparks = 0;
        Rgb color{};
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        int16_t head_x = 0; ///< line drawn in the last frame
        int16_t head_y = 0;
        int16_t tail_x = 0;
        int16_t tail_y = 0;
    };

    void advance(uint32_t step_ms, std::minstd_rand& rng);
    void launch(std::minstd_rand& rng);
    void burst(const Rocket& rocket, std::minstd_rand& rng);
    Spark* take_spark();
    size_t in_flight() const;
    int32_t next_gap_ms(std::minstd_rand& rng) const;

    void draw_spark(PixelWriter& writer, Spark& spark, std::minstd_rand& rng, DirtyRect& box);
    void erase_spark(PixelWriter& writer, const Spark& spark, DirtyRect& box);
    void draw_rocket(PixelWriter& writer, Rocket& rocket, DirtyRect& box);
    void erase_rocket(PixelWriter& writer, const Rocket& rocket, DirtyRect& box);
    void light_point(PixelWriter& writer, int32_t x, int32_t y, Rgb color, DirtyRect& box);
    void erase_point(PixelWriter& writer, int32_t x, int32_t y, DirtyRect& box);

    /// Calls visit(x, y, brightness 0-255) for the head, the 2x2 block when drawn big, and the
    /// trail of `spark` as it was last drawn.
    template <typename Visit>
    void visit_spark_points(const Spark& spark, uint8_t trail, Visit&& visit) const;

    FireworksPacing pacing_{};
    size_t level_ = 0;
    size_t pending_level_ = 0;
    int32_t w_ = 0;
    int32_t h_ = 0;
    float scale_ = 1.0f;

    // The sky as layout, never as pixels: a colour per row, a hill top and at most one star per column.
    std::vector<Rgb> sky_rows_;
    std::vector<int16_t> hill_top_;
    std::vector<int16_t> star_y_; ///< -1 where a column has no star
    std::vector<uint8_t> star_level_;

    std::vector<Spark> sparks_;
    size_t next_spark_ = 0;
    Burst bursts_[MAX_BURSTS]{};
    Rocket rockets_[MAX_BURSTS]{};

    int32_t launch_in_ms_ = 0;
    int32_t finale_in_ms_ = 0;
    uint8_t finale_left_ = 0;
    int32_t finale_gap_ms_ = 0;
};

} // namespace helix::ui
```

- [ ] **Step 4: Write the simulation, part 1: constants, sky and pool**

Create `src/ui/screensaver_fireworks_sim.cpp` with this first half:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_fireworks_sim.h"

#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace helix::ui {

namespace {

using screensaver::random_below;
using screensaver::unit_random;

constexpr float PI = 3.14159265f;

constexpr Rgb SKY_TOP = {2, 3, 10};       // near black overhead
constexpr Rgb SKY_HORIZON = {12, 20, 52}; // deep navy where the sky meets the hills
constexpr Rgb HILLS = {1, 2, 4};
constexpr Rgb WHITE = {255, 255, 255};
constexpr Rgb EMBER = {255, 110, 30};
constexpr Rgb WILLOW_GOLD = {255, 190, 90};
constexpr Rgb ROCKET = {255, 214, 150};
constexpr Rgb SHELL_COLORS[] = {
    {255, 60, 60}, {80, 255, 90}, {90, 140, 255}, {255, 230, 80},
    {255, 90, 230}, {90, 240, 255}, {255, 160, 60},
};
constexpr int SHELL_COLOR_COUNT = static_cast<int>(std::size(SHELL_COLORS));

// Physics runs in steps no longer than this, so a late frame moves everything the same way.
constexpr uint32_t PHYSICS_STEP_MS = 20;

// Distances and accelerations are for a 480 px tall frame and scale with the frame's height.
constexpr float REFERENCE_HEIGHT = 480.0f;
constexpr float ROCKET_GRAVITY = 110.0f; // px/s^2
constexpr float BURST_SPEED = 30.0f;     // a shell bursts once it climbs slower than this, px/s
constexpr float ROCKET_TAIL_PX = 9.0f;
constexpr int32_t TRAIL_SPACING_TENTHS = 16; // trail points 1.6 px apart

// Where each colour phase of a spark's life ends, as a share of the life.
constexpr float FLASH_END = 0.08f;
constexpr float COLOR_END = 0.55f;
constexpr float EMBER_END = 0.80f;
constexpr float FLICKER_FROM = 0.70f;
constexpr float BIG_UNTIL = 0.25f;

constexpr int STAR_MIN_LEVEL = 40;
constexpr int STAR_MAX_LEVEL = 110;

struct KindParams {
    float speed_min; // px/s
    float speed_max;
    int life_min_ms;
    int life_max_ms;
    float drag;    // share of velocity lost per second
    float gravity; // px/s^2
    bool full_trail;
};

// Indexed by FireworksSim::BurstKind.
constexpr KindParams KINDS[] = {
    {70.0f, 110.0f, 1100, 1600, 1.1f, 40.0f, false}, // peony: an even ball
    {80.0f, 120.0f, 1400, 1900, 0.9f, 36.0f, true},  // chrysanthemum: sparks leave trails
    {55.0f, 85.0f, 2600, 3400, 1.8f, 60.0f, true},   // willow: long-lived gold, heavy drag, droops
    {95.0f, 115.0f, 1200, 1500, 1.0f, 38.0f, false}, // ring: a tilted flat circle
};

uint8_t mix_channel(uint8_t a, uint8_t b, float t) {
    const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
}

Rgb mix(Rgb a, Rgb b, float t) {
    return {mix_channel(a.r, b.r, t), mix_channel(a.g, b.g, t), mix_channel(a.b, b.b, t)};
}

/// `c` at `amount` of its brightness, 0 to 255.
Rgb dimmed(Rgb c, uint32_t amount) {
    return {static_cast<uint8_t>(c.r * amount / 255), static_cast<uint8_t>(c.g * amount / 255),
            static_cast<uint8_t>(c.b * amount / 255)};
}

int16_t to_pixel(float v) {
    return static_cast<int16_t>(std::clamp(std::lround(v), -16000L, 16000L));
}

} // namespace

void FireworksSim::init(FrameTarget& frame, std::minstd_rand& rng, size_t level,
                        const FireworksPacing& pacing) {
    pacing_ = pacing;
    level_ = std::min(level, FIREWORKS_LEVEL_COUNT - 1);
    pending_level_ = level_;
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    scale_ = std::max(0.4f, static_cast<float>(h_) / REFERENCE_HEIGHT);

    sky_rows_.resize(static_cast<size_t>(h_));
    for (int32_t y = 0; y < h_; y++) {
        const float t = h_ > 1 ? static_cast<float>(y) / static_cast<float>(h_ - 1) : 0.0f;
        sky_rows_[static_cast<size_t>(y)] = mix(SKY_TOP, SKY_HORIZON, t);
    }

    // Rolling hills: three waves with random phases give one height per column.
    const float phase_a = unit_random(rng) * 2.0f * PI;
    const float phase_b = unit_random(rng) * 2.0f * PI;
    const float phase_c = unit_random(rng) * 2.0f * PI;
    hill_top_.resize(static_cast<size_t>(w_));
    for (int32_t x = 0; x < w_; x++) {
        const float fx = static_cast<float>(x) / static_cast<float>(std::max(w_, 1));
        const float share = 0.14f + 0.05f * std::sin(fx * 2.0f * PI * 1.1f + phase_a) +
                            0.025f * std::sin(fx * 2.0f * PI * 3.0f + phase_b) +
                            0.01f * std::sin(fx * 2.0f * PI * 9.0f + phase_c);
        const int32_t top = h_ - static_cast<int32_t>(share * static_cast<float>(h_));
        hill_top_[static_cast<size_t>(x)] = static_cast<int16_t>(std::clamp<int32_t>(top, 0, h_));
    }

    // Faint fixed stars, at most one per column, in the upper 70% of the frame.
    star_y_.assign(static_cast<size_t>(w_), -1);
    star_level_.assign(static_cast<size_t>(w_), 0);
    for (int i = 0; i < STAR_COUNT && w_ > 0 && h_ > 0; i++) {
        int32_t x = random_below(rng, w_);
        const int32_t y = random_below(rng, std::max(1, h_ * 7 / 10));
        const auto star_level =
            static_cast<uint8_t>(STAR_MIN_LEVEL + random_below(rng, STAR_MAX_LEVEL - STAR_MIN_LEVEL + 1));
        for (int32_t tries = 0; tries < w_ && star_y_[static_cast<size_t>(x)] >= 0; tries++) {
            x = (x + 1) % w_;
        }
        if (star_y_[static_cast<size_t>(x)] >= 0 || y >= hill_top_[static_cast<size_t>(x)]) {
            continue;
        }
        star_y_[static_cast<size_t>(x)] = static_cast<int16_t>(y);
        star_level_[static_cast<size_t>(x)] = star_level;
    }

    const FireworksLevel& params = FIREWORKS_LEVELS[level_];
    sparks_.assign(static_cast<size_t>(params.sparks_per_burst) * params.bursts_at_once, Spark{});
    next_spark_ = 0;
    for (Burst& b : bursts_) {
        b = Burst{};
    }
    for (Rocket& r : rockets_) {
        r = Rocket{};
    }
    launch_in_ms_ = static_cast<int32_t>(pacing_.first_launch_ms);
    finale_in_ms_ = static_cast<int32_t>(std::max<uint32_t>(pacing_.finale_every_ms, 1));
    finale_left_ = 0;
    finale_gap_ms_ = 0;

    PixelWriter writer(frame);
    for (int32_t y = 0; y < h_; y++) {
        for (int32_t x = 0; x < w_; x++) {
            writer.put_dithered(x, y, sky_at(x, y));
        }
    }
}

Rgb FireworksSim::sky_at(int32_t x, int32_t y) const {
    const auto column = static_cast<size_t>(x);
    if (y >= hill_top_[column]) {
        return HILLS;
    }
    if (star_y_[column] == y) {
        const uint8_t v = star_level_[column];
        return {v, v, static_cast<uint8_t>(std::min(255, v + 25))};
    }
    return sky_rows_[static_cast<size_t>(y)];
}

void FireworksSim::request_level(size_t level) {
    pending_level_ = std::min(level, FIREWORKS_LEVEL_COUNT - 1);
}

size_t FireworksSim::sparks_in_use() const {
    return static_cast<size_t>(std::count_if(sparks_.begin(), sparks_.end(), [](const Spark& s) {
        return (s.flags & SPARK_ALIVE) != 0;
    }));
}

void FireworksSim::clear() {
    std::vector<Spark>().swap(sparks_);
    std::vector<Rgb>().swap(sky_rows_);
    std::vector<int16_t>().swap(hill_top_);
    std::vector<int16_t>().swap(star_y_);
    std::vector<uint8_t>().swap(star_level_);
    for (Burst& b : bursts_) {
        b = Burst{};
    }
    for (Rocket& r : rockets_) {
        r = Rocket{};
    }
    w_ = 0;
    h_ = 0;
}

FireworksSim::Spark* FireworksSim::take_spark() {
    const size_t count = sparks_.size();
    for (size_t k = 0; k < count; k++) {
        const size_t i = (next_spark_ + k) % count;
        if (sparks_[i].flags == 0) {
            next_spark_ = (i + 1) % count;
            return &sparks_[i];
        }
    }
    return nullptr;
}

size_t FireworksSim::in_flight() const {
    size_t count = 0;
    for (const Rocket& r : rockets_) {
        count += r.active ? 1 : 0;
    }
    for (const Burst& b : bursts_) {
        count += b.active ? 1 : 0;
    }
    return count;
}

int32_t FireworksSim::next_gap_ms(std::minstd_rand& rng) const {
    const uint32_t min_gap = pacing_.min_gap_ms;
    const uint32_t max_gap = std::max(pacing_.max_gap_ms, min_gap);
    return static_cast<int32_t>(
        min_gap + static_cast<uint32_t>(random_below(rng, static_cast<int>(max_gap - min_gap + 1))));
}
```

- [ ] **Step 5: Write the simulation, part 2: frames, shells, bursts and drawing**

Append to `src/ui/screensaver_fireworks_sim.cpp`:

```cpp
void FireworksSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                        std::vector<DirtyRect>& dirty) {
    dirty.clear();
    if (w_ <= 0 || h_ <= 0) {
        return;
    }
    PixelWriter writer(frame);
    DirtyRect rocket_boxes[MAX_BURSTS];
    DirtyRect burst_boxes[MAX_BURSTS];

    // Everything the previous frame drew goes back to sky first, so the frame shows only what
    // it draws itself.
    for (size_t i = 0; i < MAX_BURSTS; i++) {
        if (rockets_[i].drawn) {
            erase_rocket(writer, rockets_[i], rocket_boxes[i]);
            rockets_[i].drawn = false;
        }
    }
    for (Spark& spark : sparks_) {
        if ((spark.flags & SPARK_DRAWN) != 0) {
            erase_spark(writer, spark, burst_boxes[spark.burst]);
            spark.flags = static_cast<uint8_t>(spark.flags & ~(SPARK_DRAWN | SPARK_DRAWN_BIG));
        }
    }

    for (uint32_t remaining = dt_ms; remaining > 0;) {
        const uint32_t step_ms = std::min(remaining, PHYSICS_STEP_MS);
        advance(step_ms, rng);
        remaining -= step_ms;
    }

    for (size_t i = 0; i < MAX_BURSTS; i++) {
        if (rockets_[i].active) {
            draw_rocket(writer, rockets_[i], rocket_boxes[i]);
        }
    }
    for (Spark& spark : sparks_) {
        if ((spark.flags & SPARK_ALIVE) != 0) {
            draw_spark(writer, spark, rng, burst_boxes[spark.burst]);
        }
    }
    // A burst with no sparks left drew nothing this frame, and its last pixels are erased.
    for (Burst& b : bursts_) {
        if (b.active && b.alive == 0) {
            b.active = false;
        }
    }

    for (size_t i = 0; i < MAX_BURSTS; i++) {
        if (!rocket_boxes[i].empty()) {
            dirty.push_back(rocket_boxes[i]);
        }
        if (!burst_boxes[i].empty()) {
            dirty.push_back(burst_boxes[i]);
        }
    }
}

void FireworksSim::advance(uint32_t step_ms, std::minstd_rand& rng) {
    const float dt = static_cast<float>(step_ms) / 1000.0f;
    const auto elapsed = static_cast<int32_t>(step_ms);

    finale_in_ms_ -= elapsed;
    if (finale_in_ms_ <= 0) {
        finale_in_ms_ += static_cast<int32_t>(std::max<uint32_t>(pacing_.finale_every_ms, 1));
        if (finale_left_ == 0) {
            const int extra =
                std::max(0, static_cast<int>(pacing_.finale_max_shells) - pacing_.finale_min_shells);
            finale_left_ = static_cast<uint8_t>(
                std::max(1, pacing_.finale_min_shells + random_below(rng, extra + 1)));
            finale_gap_ms_ = static_cast<int32_t>(pacing_.finale_span_ms / finale_left_);
            launch_in_ms_ = 0;
        }
    }

    launch_in_ms_ -= elapsed;
    if (launch_in_ms_ <= 0) {
        if (in_flight() < FIREWORKS_LEVELS[pending_level_].bursts_at_once) {
            launch(rng);
            if (finale_left_ > 0) {
                finale_left_--;
            }
            launch_in_ms_ = finale_left_ > 0 ? finale_gap_ms_ : next_gap_ms(rng);
        } else {
            launch_in_ms_ = 0; // the sky is full; try again next step
        }
    }

    const float rocket_gravity = ROCKET_GRAVITY * scale_;
    for (Rocket& rocket : rockets_) {
        if (!rocket.active) {
            continue;
        }
        rocket.vy += rocket_gravity * dt;
        rocket.x += rocket.vx * dt;
        rocket.y += rocket.vy * dt;
        if (rocket.vy >= -BURST_SPEED * scale_) {
            burst(rocket, rng);
            rocket.active = false;
        }
    }

    for (Spark& spark : sparks_) {
        if ((spark.flags & SPARK_ALIVE) == 0) {
            continue;
        }
        Burst& owner = bursts_[spark.burst];
        const uint32_t age = static_cast<uint32_t>(spark.age_ms) + step_ms;
        if (age >= spark.life_ms) {
            spark.flags = static_cast<uint8_t>(spark.flags & ~SPARK_ALIVE);
            if (owner.alive > 0) {
                owner.alive--;
            }
            continue;
        }
        spark.age_ms = static_cast<uint16_t>(age);
        const float keep = std::max(0.0f, 1.0f - owner.drag * dt);
        spark.vx *= keep;
        spark.vy = spark.vy * keep + owner.gravity * dt;
        spark.x += spark.vx * dt;
        spark.y += spark.vy * dt;
    }
}

void FireworksSim::launch(std::minstd_rand& rng) {
    level_ = pending_level_;
    Rocket* slot = nullptr;
    for (Rocket& r : rockets_) {
        if (!r.active) {
            slot = &r;
            break;
        }
    }
    if (slot == nullptr) {
        return;
    }
    const FireworksLevel& params = FIREWORKS_LEVELS[level_];
    // Launched from anywhere along the hills, to burst in the upper 60% of the frame.
    const int32_t x = std::clamp<int32_t>(
        static_cast<int32_t>(static_cast<float>(w_) * (0.1f + 0.8f * unit_random(rng))), 0, w_ - 1);
    const float ground = static_cast<float>(hill_top_[static_cast<size_t>(x)]);
    const float apex = static_cast<float>(h_) * (0.08f + 0.52f * unit_random(rng));
    const float rise = std::max(ground - apex, 20.0f * scale_);

    Rocket& rocket = *slot;
    rocket = Rocket{};
    rocket.active = true;
    rocket.kind = static_cast<BurstKind>(random_below(rng, 4));
    rocket.color = rocket.kind == BurstKind::WILLOW ? WILLOW_GOLD
                                                   : SHELL_COLORS[random_below(rng, SHELL_COLOR_COUNT)];
    rocket.sparks = params.sparks_per_burst;
    rocket.trail = params.trail;
    rocket.x = static_cast<float>(x);
    rocket.y = ground;
    rocket.vx = (unit_random(rng) - 0.5f) * 30.0f * scale_;
    rocket.vy = -std::sqrt(2.0f * ROCKET_GRAVITY * scale_ * rise);
}

void FireworksSim::burst(const Rocket& rocket, std::minstd_rand& rng) {
    size_t slot = MAX_BURSTS;
    for (size_t i = 0; i < MAX_BURSTS; i++) {
        if (!bursts_[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == MAX_BURSTS) {
        return;
    }
    const KindParams& kind = KINDS[static_cast<size_t>(rocket.kind)];
    Burst& owner = bursts_[slot];
    owner = Burst{};
    owner.active = true;
    owner.kind = rocket.kind;
    owner.trail = kind.full_trail ? rocket.trail : static_cast<uint8_t>(rocket.trail / 2);
    owner.drag = kind.drag;
    owner.gravity = kind.gravity * scale_;

    const float speed = (kind.speed_min + (kind.speed_max - kind.speed_min) * unit_random(rng)) * scale_;
    const float tilt = std::cos(unit_random(rng) * 0.45f * PI);
    const float spin = unit_random(rng) * 2.0f * PI;
    for (uint16_t i = 0; i < rocket.sparks; i++) {
        Spark* spark = take_spark();
        if (spark == nullptr) {
            break; // the pool is full: the rest of this burst is dropped
        }
        float dx = 0.0f;
        float dy = 0.0f;
        float spark_speed = speed;
        if (rocket.kind == BurstKind::RING) {
            const float theta = 2.0f * PI * static_cast<float>(i) / static_cast<float>(rocket.sparks);
            const float cx = std::cos(theta);
            const float cy = std::sin(theta) * tilt;
            dx = cx * std::cos(spin) - cy * std::sin(spin);
            dy = cx * std::sin(spin) + cy * std::cos(spin);
        } else {
            // A direction uniform over a sphere, seen face on: an evenly filled ball.
            const float z = 2.0f * unit_random(rng) - 1.0f;
            const float phi = 2.0f * PI * unit_random(rng);
            const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
            dx = radius * std::cos(phi);
            dy = radius * std::sin(phi);
            spark_speed = speed * (0.85f + 0.15f * unit_random(rng));
        }
        *spark = Spark{};
        spark->x = rocket.x;
        spark->y = rocket.y;
        spark->vx = dx * spark_speed + rocket.vx * 0.3f;
        spark->vy = dy * spark_speed + rocket.vy * 0.3f;
        spark->life_ms = static_cast<uint16_t>(
            kind.life_min_ms + random_below(rng, kind.life_max_ms - kind.life_min_ms + 1));
        spark->color = rocket.color;
        spark->flags = SPARK_ALIVE;
        spark->burst = static_cast<uint8_t>(slot);
        owner.alive++;
    }
    if (owner.alive == 0) {
        owner.active = false;
    }
}

template <typename Visit>
void FireworksSim::visit_spark_points(const Spark& spark, uint8_t trail, Visit&& visit) const {
    visit(spark.drawn_x, spark.drawn_y, 255u);
    if ((spark.flags & SPARK_DRAWN_BIG) != 0) {
        visit(spark.drawn_x + 1, spark.drawn_y, 255u);
        visit(spark.drawn_x, spark.drawn_y + 1, 255u);
        visit(spark.drawn_x + 1, spark.drawn_y + 1, 255u);
    }
    for (int32_t k = 1; k <= trail; k++) {
        const int32_t tx = spark.drawn_x + spark.drawn_dx * k * TRAIL_SPACING_TENTHS / 1000;
        const int32_t ty = spark.drawn_y + spark.drawn_dy * k * TRAIL_SPACING_TENTHS / 1000;
        visit(tx, ty, static_cast<uint32_t>(220 - 200 * k / (trail + 1)));
    }
}

void FireworksSim::draw_spark(PixelWriter& writer, Spark& spark, std::minstd_rand& rng,
                              DirtyRect& box) {
    const float life = static_cast<float>(std::max<uint16_t>(spark.life_ms, 1));
    const float f = static_cast<float>(spark.age_ms) / life;
    // Late in its life a spark flickers: on some frames it is not drawn.
    if (f > FLICKER_FROM && random_below(rng, 4) == 0) {
        return;
    }

    // White flash, the shell colour, ember orange, then a fade into the sky.
    Rgb color = spark.color;
    if (f < FLASH_END) {
        color = mix(WHITE, spark.color, f / FLASH_END);
    } else if (f < COLOR_END) {
        color = spark.color;
    } else if (f < EMBER_END) {
        color = mix(spark.color, EMBER, (f - COLOR_END) / (EMBER_END - COLOR_END));
    } else {
        color = dimmed(EMBER, static_cast<uint32_t>(255.0f * (1.0f - (f - EMBER_END) / (1.0f - EMBER_END))));
    }

    const float speed = std::sqrt(spark.vx * spark.vx + spark.vy * spark.vy);
    spark.drawn_x = to_pixel(spark.x);
    spark.drawn_y = to_pixel(spark.y);
    spark.drawn_dx = speed > 1.0f ? static_cast<int8_t>(std::lround(-spark.vx / speed * 100.0f)) : 0;
    spark.drawn_dy = speed > 1.0f ? static_cast<int8_t>(std::lround(-spark.vy / speed * 100.0f)) : 0;
    spark.flags = static_cast<uint8_t>(spark.flags | SPARK_DRAWN | (f < BIG_UNTIL ? SPARK_DRAWN_BIG : 0));
    visit_spark_points(spark, bursts_[spark.burst].trail, [&](int32_t x, int32_t y, uint32_t amount) {
        light_point(writer, x, y, dimmed(color, amount), box);
    });
}

void FireworksSim::erase_spark(PixelWriter& writer, const Spark& spark, DirtyRect& box) {
    visit_spark_points(spark, bursts_[spark.burst].trail,
                       [&](int32_t x, int32_t y, uint32_t) { erase_point(writer, x, y, box); });
}

void FireworksSim::draw_rocket(PixelWriter& writer, Rocket& rocket, DirtyRect& box) {
    const float speed = std::sqrt(rocket.vx * rocket.vx + rocket.vy * rocket.vy);
    const float ux = speed > 1.0f ? rocket.vx / speed : 0.0f;
    const float uy = speed > 1.0f ? rocket.vy / speed : -1.0f;
    const float tail = ROCKET_TAIL_PX * scale_;
    rocket.head_x = to_pixel(rocket.x);
    rocket.head_y = to_pixel(rocket.y);
    rocket.tail_x = to_pixel(rocket.x - ux * tail);
    rocket.tail_y = to_pixel(rocket.y - uy * tail);
    rocket.drawn = true;

    // A short spark trail, brightest at the shell.
    const int32_t points = std::max(std::abs(rocket.tail_x - rocket.head_x),
                                    std::abs(rocket.tail_y - rocket.head_y)) + 1;
    int32_t index = 0;
    PixelWriter::line(rocket.head_x, rocket.head_y, rocket.tail_x, rocket.tail_y,
                      [&](int32_t x, int32_t y) {
                          light_point(writer, x, y,
                                      dimmed(ROCKET, static_cast<uint32_t>(255 - 195 * index / points)), box);
                          index++;
                      });
}

void FireworksSim::erase_rocket(PixelWriter& writer, const Rocket& rocket, DirtyRect& box) {
    PixelWriter::line(rocket.head_x, rocket.head_y, rocket.tail_x, rocket.tail_y,
                      [&](int32_t x, int32_t y) { erase_point(writer, x, y, box); });
}

void FireworksSim::light_point(PixelWriter& writer, int32_t x, int32_t y, Rgb color, DirtyRect& box) {
    if (!writer.contains(x, y)) {
        return;
    }
    writer.blend_max(x, y, color);
    box.add(x, y, x, y);
}

void FireworksSim::erase_point(PixelWriter& writer, int32_t x, int32_t y, DirtyRect& box) {
    if (!writer.contains(x, y)) {
        return;
    }
    // Erasing recomputes the sky at the pixel, so no background buffer exists.
    writer.put_dithered(x, y, sky_at(x, y));
    box.add(x, y, x, y);
}

} // namespace helix::ui
```

- [ ] **Step 6: Write the saver and register fireworks**

Create `include/screensaver_fireworks.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_canvas.h"
#include "screensaver_fireworks_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

/**
 * @brief Fireworks over hills under a night sky
 *
 * helix::ui::FireworksSim draws each frame straight into the canvas at the display's own
 * depth, RGB565 or XRGB8888, and returns one dirty box per rocket or burst. A level request
 * waits for the next shell launch, and so does the frame period that comes with it.
 */
class FireworksScreensaver : public helix::ui::SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::FIREWORKS;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::SAVER_BUILD_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return helix::ui::FIREWORKS_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return helix::ui::FIREWORKS_LEVELS[level].period_ms;
    }
    void on_level_request(size_t level) override {
        sim_.request_level(level);
    }

  private:
    helix::ui::FireworksSim sim_;
};

#endif // HELIX_ENABLE_SCREENSAVER
```

Create `src/ui/screensaver_fireworks.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_fireworks.h"

#include <spdlog/spdlog.h>

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;

bool FireworksScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting fireworks");
    FrameTarget frame = canvas().frame();
    sim_.init(frame, rng(), level());
    // Filling the canvas black at creation invalidated all of it and no refresh has run since,
    // so the first refresh shows the sky painted here.
    return true;
}

void FireworksScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    FrameTarget frame = canvas().frame();
    sim_.step(dt_ms, frame, rng(), dirty);
    if (sim_.level() != level()) {
        apply_level(sim_.level());
    }
}

void FireworksScreensaver::on_stop() {
    sim_.clear();
}

#endif // HELIX_ENABLE_SCREENSAVER
```

In `include/screensaver_registry.h`, add `FIREWORKS = 4,` after `PIPES_3D = 3,` in `ScreensaverType`, and add this row after the pipes row of `SCREENSAVERS`:

```cpp
    {ScreensaverType::FIREWORKS, "fireworks", "Fireworks", SAVER_DEPTH_16 | SAVER_DEPTH_32},
```

In `include/screensaver_canvas.h`, add `#include "screensaver_registry.h"` next to `#include "screensaver_frame.h"`, and insert after `pixel_format_for`:

```cpp
/// SaverDepth bit of this build's display.
inline constexpr uint8_t BUILD_SAVER_DEPTH = LV_COLOR_DEPTH == 16 ? SAVER_DEPTH_16 : SAVER_DEPTH_32;

```

In `src/ui/screensaver_manager.cpp`, add `#include "screensaver_fireworks.h"` to the includes and append to the constructor body, after the pipes line:

```cpp
    screensavers_.push_back(std::make_unique<FireworksScreensaver>());
```

In `include/display_settings_manager.h`, replace `/** @brief Get screensaver type (0=Off, 1=Flying Toasters, 2=Starfield, 3=3D Pipes) */` with `/** @brief Get screensaver type (a ScreensaverType value, include/screensaver_registry.h) */`, and `/** @brief Screensaver type subject (integer: 0=off, 1=toasters, 2=starfield, 3=pipes) */` with `/** @brief Screensaver type subject (a ScreensaverType value, include/screensaver_registry.h) */`.

In `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`, add after `src/ui/screensaver_cpu_clock.cpp  # not in the v1 Core+AMS cut`:

```
src/ui/screensaver_fireworks.cpp  # not in the v1 Core+AMS cut
src/ui/screensaver_fireworks_sim.cpp  # not in the v1 Core+AMS cut
```

In `scripts/screensaver-perf/perf_env.sh#perf_saver_type`, add `    fireworks) echo 4 ;;` after the `pipes) echo 3 ;;` line.

- [ ] **Step 7: Run the tests and watch the dropdown test fail**

Run: `python3 scripts/check_esp32_app_srcs.py; echo "exit $?"` then `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[fireworks_sim],[screensaver_fireworks]"` then `./build/bin/helix-tests "[screensaver_registry]"`.
Expected: `exit 0` twice; every fireworks case passes; `the settings dropdown lists Off then every registered screensaver in type order` fails because the XML lacks `Fireworks`.

- [ ] **Step 8: Add the dropdown option, translations and CJK glyphs**

In `ui_xml/settings_display_sound_overlay.xml`, in `row_screensaver`, change both `options="Off&#10;Flying Toasters&#10;Starfield&#10;3D Pipes"` and `options_tag="Off&#10;Flying Toasters&#10;Starfield&#10;3D Pipes"` to end in `&#10;3D Pipes&#10;Fireworks"`.

Run `make translation-sync > "$SS_SCRATCH/translation-sync.log" 2>&1; echo "exit $?"`, then check what it wrote. English gets the key as its value and every other language an empty string (`scripts/translations/yaml_manager.py#merge_new_keys`):

```bash
grep -c -x -F "  Fireworks: Fireworks" translations/en.yml
for lang in de es fr it ja pt ru zh; do printf '%s: ' "$lang"; grep -c -x -F "  Fireworks: ''" "translations/$lang.yml"; done
```

Expected: `exit 0`, or a non-zero exit whose log ends with the CJK staleness check naming the new characters (fixed below); `1` for `en.yml` and `1` for each of the eight others. A `0` means the sync wrote the entry in another form: stop and read that file rather than editing it blind.

Set the translations, matching the empty value, then check every file holds its exact text:

```bash
sed -i "s/^  Fireworks: ''\$/  Fireworks: Feuerwerk/" translations/de.yml
sed -i "s/^  Fireworks: ''\$/  Fireworks: Fuegos artificiales/" translations/es.yml
sed -i "s/^  Fireworks: ''\$/  Fireworks: Feux d'artifice/" translations/fr.yml
sed -i "s/^  Fireworks: ''\$/  Fireworks: Fuochi d'artificio/" translations/it.yml
sed -i "s/^  Fireworks: ''\$/  Fireworks: 花火/" translations/ja.yml
sed -i "s/^  Fireworks: ''\$/  Fireworks: Fogos de artifício/" translations/pt.yml
sed -i "s/^  Fireworks: ''\$/  Fireworks: Фейерверк/" translations/ru.yml
sed -i "s/^  Fireworks: ''\$/  Fireworks: 烟花/" translations/zh.yml
missing=0
while IFS='|' read -r lang text; do
    grep -q -x -F "  Fireworks: $text" "translations/$lang.yml" || { echo "MISSING $lang: $text"; missing=1; }
done <<'EOF'
en|Fireworks
de|Feuerwerk
es|Fuegos artificiales
fr|Feux d'artifice
it|Fuochi d'artificio
ja|花火
pt|Fogos de artifício
ru|Фейерверк
zh|烟花
EOF
echo "missing=$missing"
```

Expected: `missing=0` and no `MISSING` line; `sed` exits 0 whether or not it matched, so this check is what proves the values landed, and with them the CJK characters the font step below must add.

Regenerate the packs and the CJK runtime fonts, then check them:

```bash
make translations > "$SS_SCRATCH/translations.log" 2>&1; echo "exit $?"
make regen-text-fonts > "$SS_SCRATCH/regen-text-fonts.log" 2>&1; echo "exit $?"
bash scripts/check_cjk_font_staleness.sh && python3 scripts/check_cjk_font_coverage.py; echo "cjk exit $?"
git status --short translations ui_xml/translations assets/fonts/cjk
```

Expected: both makes `exit 0` (`regen-text-fonts` downloads the Noto CJK sources when absent and needs `lv_font_conv` from `node_modules`); `cjk exit 0`; the status lists the nine YAML files, the regenerated `ui_xml/translations/*.xml` packs and at least one changed file under `assets/fonts/cjk/` (none there means the new 花火 and 烟花 glyphs were not baked: stop).

Run: `./build/bin/helix-tests "[screensaver_registry]"`
Expected: every case passes.

- [ ] **Step 9: Update the docs**

In `docs/devel/ENVIRONMENT_VARIABLES.md` (section `HELIX_SCREENSAVER_NOW`), change the **Values** row's name list from `(\`toasters\`, \`starfield\`, \`pipes\`)` to `(\`toasters\`, \`starfield\`, \`pipes\`, \`fireworks\`)`.

In `docs/user/CONFIGURATION.md` (section `screensaver_type`), replace `**Values:** \`0\` = Off, \`1\` = Flying Toasters, \`2\` = Starfield, \`3\` = 3D Pipes` with `**Values:** \`0\` = Off, \`1\` = Flying Toasters, \`2\` = Starfield, \`3\` = 3D Pipes, \`4\` = Fireworks`.

In `docs/user/guide/settings/display-sound.md`, add this row after the `**3D Pipes**` row of the screensaver table:

```markdown
| **Fireworks** | Fireworks bursting over hills under a night sky |
```

In `config/settings.json.template`, change `3=3D Pipes. Replaces` in `_screensaver_type_comment` to `3=3D Pipes, 4=Fireworks. Replaces`, then run `python3 -c 'import json; json.load(open("config/settings.json.template")); print("template ok")'` (expected `template ok`).

Create `docs/devel/SCREENSAVERS.md`:

````markdown
# Screensavers

A screensaver runs when the display dims (`DisplayManager::check_display_sleep`), covers the
panel with a full-screen overlay, and measures its own CPU cost so it never slows the printer.
This doc is the map: which file owns what, how the gate decides, and how to add a saver.

## Key files

| File | Owns |
|---|---|
| `include/screensaver_registry.h` | `ScreensaverType`, the `SCREENSAVERS` rows (type, stable name, label key, color depths), clamp, `HELIX_SCREENSAVER_NOW` names, the fresh-install default. Always compiled |
| `include/screensaver_base.h` | `SaverBase`: start, frame and stop sequence, levels, level requests |
| `include/screensaver_overlay.h` | `SaverOverlay`: the black touch-absorbing overlay on `lv_layer_top()` |
| `include/screensaver_canvas.h` | `SaverCanvas`: stride-sized buffer, format per depth, hide-before-free, dirty areas merged to 32, the dirty-box layer session |
| `include/screensaver_frame_timer.h` | `SaverFrameTimer`: frame timer with a `MotionClock`, period changes without a motion jump |
| `include/refresh_period_hold.h` | The display refresh while a saver runs, at the saver's current period; `period()` holds `HELIX_SCREENSAVER_REFR_PERIOD_MS` |
| `include/screensaver_frame.h`, `include/screensaver_pixel_writer.h` | `DirtyRect`, `FrameTarget`, `PixelWriter` (RGB565 and XRGB8888, ordered dither, max blend, lines). No LVGL |
| `include/screensaver_gate.h`, `include/screensaver_cpu_clock.h` | Budget, decision, idle baseline, windows, environment switches, the CPU clock seam |
| `include/screensaver_level_store.h` | Board fingerprint and the stored level per saver |
| `include/screensaver.h`, `src/ui/screensaver_manager.cpp` | `ScreensaverManager`: factories, holds, the gate, the black-screen fallback |
| `include/ui_screensaver.h`, `include/screensaver_starfield.h`, `include/screensaver_pipes.h`, `include/screensaver_fireworks.h` | The savers |
| `scripts/screensaver-perf/` | Measuring a saver on a real board |

## The foundation

Every saver derives from `SaverBase` and implements `canvas_format()`, `on_start()`,
`on_frame()`, `on_stop()` and its ladder (`ladder_size()`, `ladder_period_ms()`). The base
creates the overlay, the canvas when the saver has one, seeds the saver's own random sequence,
starts the frame timer at the level's period, and invalidates the dirty areas each frame
returns. A saver that writes pixels itself returns boxes; a saver drawing with `lv_draw_*`
opens `SaverCanvas::begin_layer()`, marks what it draws with `mark_dirty()` and finishes with
`finish_layer()`. Toasters use no canvas: their sprites are LVGL images.

Every ladder starts at 16 ms (`SAVER_FAST_PERIOD`, one refresh of a 60 Hz panel), or at
`HELIX_SCREENSAVER_REFR_PERIOD_MS` when that is set for manual testing
(`SaverBase::level_period_ms`); levels 1 and up keep their declared periods either way. Starfield
and pipes then run at 33 ms; toasters at 33 ms with every sprite, then 33 ms with ten; fireworks
per `FIREWORKS_LEVELS`. While a saver runs, `helix::RefreshPeriodHold` refreshes the display at
the saver's current period (`follow`), so the display and the saver stay equal at every level. Device measurements assume the pacing that keeps 16 ms even: EGL vsync and a 1 ms
main-loop floor, which every device run sets explicitly (`scripts/screensaver-perf/arm_measure.sh`).

Motion is a function of elapsed time (`include/screensaver_motion.h`), so a level that changes
the frame period changes nothing on screen but the frame rate, and at the toasters' lowest rung
the sprite count.

## The gate

`ScreensaverManager::on_idle_check_tick` runs on the display manager's idle-check tick and
samples `CLOCK_PROCESS_CPUTIME_ID` (every thread, LVGL's draw thread included) at most every
250 ms.

- While no saver runs, samples feed `IdleBaseline`: the CPU rate over the last idle stretch of
  up to 10 s, and 0 under 3 s of samples.
- A run ignores its first second, then closes a 5 s window after another for as long as it runs:
  `(cpu delta - baseline * wall) / wall` as a share of one core.
- The budget is `saver_budget_share`: 4 or more cores 50%, 3 cores 37%, 2 cores 25%, 1 core 10%,
  halved while `SaverHost::is_printing` says a print holds the machine.
- `decide_saver_level`: within budget the level stays (a run never steps up); over budget the
  saver steps down one level at its next natural break; over budget at the bottom level the run
  becomes a static black `SaverOverlay` with no frame timer.

Each step down and each too-heavy mark is stored at `/display/screensaver_levels/<name>` as
`{"level", "too_heavy", "version", "board"}`. The next run of that saver starts there, unless
`helix_version_full()` or the board fingerprint (display path, cores, bogomips to the nearest
100, resolution, color depth) differs, which starts again at level 0.

`HELIX_SCREENSAVER_BUDGET_PCT` replaces the budget and `HELIX_SCREENSAVER_LEVEL` forces a level
with the gate off (`docs/devel/ENVIRONMENT_VARIABLES.md`).

## Color depth and builds

The registry row lists the depths a saver draws at. Toasters, starfield and pipes are 32 bpp
only; fireworks draws RGB565 or XRGB8888 through `PixelWriter`, with `SAVER_BUILD_CANVAS_FORMAT`
picking the display's depth at compile time.

## Adding a saver

1. Add the type to `ScreensaverType` and a row to `SCREENSAVERS` with a stable name and depths.
2. Add the label to `row_screensaver` in `ui_xml/settings_display_sound_overlay.xml` (a test
   compares it with the registry), run `make translation-sync`, translate the new key in every
   `translations/*.yml`, then `make translations` (and `make regen-text-fonts` for new CJK text).
3. Derive from `SaverBase` in `include/screensaver_<name>.h` and `src/ui/screensaver_<name>.cpp`;
   keep the scene in a pure simulation class when it writes pixels, so it is testable without a
   display. Register the factory in `ScreensaverManager::ScreensaverManager`.
4. Add the new sources to `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`
   (the Makefile screensaver filter picks up `src/ui/screensaver_*.cpp` by itself).
5. Test determinism per seed, that dirty areas cover every changed pixel, and the ladder, then
   measure the saver on a board with `scripts/screensaver-perf/`.
````

In `docs/devel/CLAUDE.md`, add this row directly after the `SOUND_SYSTEM.md` row of the Feature Systems table:

```markdown
| `SCREENSAVERS.md` | Screensavers: the registry, shared overlay/canvas/timer/pixel writer, the CPU gate and stored levels, color depth in builds, adding a saver |
```

In `docs/README.md`, add this row directly after the `[**Sound System**](devel/SOUND_SYSTEM.md)` row:

```markdown
| [**Screensavers**](devel/SCREENSAVERS.md) | Registry, shared parts, CPU gate and stored levels, adding a saver |
```

In `docs/CLAUDE.md`, add this row to the Quick Routing table directly after `| Understand modal patterns | \`devel/MODAL_SYSTEM.md\` |`:

```markdown
| Work on a screensaver | `devel/SCREENSAVERS.md` |
```

Run: `python3 scripts/check_doc_refs.py; echo "exit $?"`
Expected: `exit 0`.

- [ ] **Step 10: Run everything fireworks touches and look at it**

Run: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` then `./build/bin/helix-tests "[screensaver]"` then `make -j"$JOBS" > "$SS_SCRATCH/app-build.log" 2>&1; echo "exit $?"`.
Expected: `exit 0`; every `[screensaver]` case passes; the app builds.

Then run it locally and capture a frame:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy HELIX_SCREENSAVER_NOW=fireworks ./build/bin/helix-screen --test -vv \
  --remote-socket "$HELIX_SOCK" > "$SS_SCRATCH/mock-fireworks.log" 2>&1 &
MOCK_PID=$!
```

When `grep -c 'Type 4 running at level 0' "$SS_SCRATCH/mock-fireworks.log"` prints `1`, wait until the log is at least 4 s older than that line (a Monitor until-loop on the log's timestamps), then run `./build/bin/helix-screen ctl -s "$HELIX_SOCK" screenshot "$SS_SCRATCH/fireworks.png"` and `kill "$MOCK_PID"`. Show Preston `$SS_SCRATCH/fireworks.png` as an artifact or file path (he cannot see images read into this session) and ask whether the sky, hills, shells and bursts look right. Delete `$HELIX_CONFIG_DIR`.

- [ ] **Step 11: Mutate, then commit**

Mutation: in `FireworksSim::erase_point`, delete `box.add(x, y, x, y);`, rebuild, and confirm `every pixel a fireworks frame changes lies in one of its dirty boxes` fails. Restore.

```bash
.venv/bin/clang-format -i include/screensaver_fireworks_sim.h src/ui/screensaver_fireworks_sim.cpp include/screensaver_fireworks.h src/ui/screensaver_fireworks.cpp include/screensaver_registry.h include/screensaver_canvas.h src/ui/screensaver_manager.cpp include/display_settings_manager.h tests/unit/test_screensaver_fireworks.cpp
git add -- include/screensaver_fireworks_sim.h src/ui/screensaver_fireworks_sim.cpp include/screensaver_fireworks.h src/ui/screensaver_fireworks.cpp include/screensaver_registry.h include/screensaver_canvas.h src/ui/screensaver_manager.cpp include/display_settings_manager.h tests/unit/test_screensaver_fireworks.cpp ui_xml/settings_display_sound_overlay.xml translations ui_xml/translations assets/fonts/cjk firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt scripts/screensaver-perf/perf_env.sh docs/devel/ENVIRONMENT_VARIABLES.md docs/user/CONFIGURATION.md docs/user/guide/settings/display-sound.md config/settings.json.template docs/devel/SCREENSAVERS.md docs/devel/CLAUDE.md docs/README.md docs/CLAUDE.md
git commit -m "feat(screensaver): fireworks, drawn at 16 or 32 bpp on a four-level ladder" -m "Shells rise from rolling hills under a computed night sky and burst as peonies, chrysanthemums, willows and rings, with a finale every few minutes. The simulation writes through PixelWriter at the display's depth, keeps a fixed spark pool, and returns one dirty box per rocket or burst; level requests apply at the next launch. Fireworks is type 4 with translations, CJK glyphs and a developer doc." -m "Mutation: erase_point stopped adding erased pixels to the box; the dirty-box coverage test went red"
```

---
### Task 11: 16 bpp measurement builds, device verification and completion gates

Builds that do not ship screensavers get a way to compile only the 16-bit-capable savers, so the CC1 and AD5M can answer whether a screensaver fits at all. Then fireworks is measured per level on the Pi 3B, the CC1 and the AD5M under the load gate, with Preston approving each printer and judging the looks. The branch closes with the full suite, mutation, ASAN and removal of the plan documents.

**How a 16 bpp build leaves out the 32-bit savers:** `Makefile` lists the platform targets whose `lv_conf.h` sets `LV_COLOR_DEPTH 16` in `SCREENSAVER_16BPP_TARGETS` and filters `SCREENSAVER_32BPP_ONLY_SRCS` (toasters, starfield and its simulation, pipes) out of `APP_SRCS` for them. `src/ui/screensaver_manager.cpp` creates those three savers only under `#if LV_COLOR_DEPTH == 32`. The two lists cannot drift silently: a 16 bpp target missing from the Makefile list compiles the 32-bit sources and stops on their `static_assert(LV_COLOR_DEPTH == 32)`, and a 32 bpp target wrongly in it fails to link the manager.

**What the dropdown shows on those builds:** all five options (`Off`, `Flying Toasters`, `Starfield`, `3D Pipes`, `Fireworks`), because `ui_xml/settings_display_sound_overlay.xml` is shared. Choosing a 32-bit saver logs `No screensaver registered for type N` and no saver starts; the screen dims and sleeps as it does with no saver. These builds are measurement builds and are never shipped.

**Files:**
- Modify: `docs/devel/SCREENSAVERS.md` (`## Color depth and builds`), `Makefile` (16 bpp filter), `mk/cross.mk` (`DOCKER_SCREENSAVER`, the `ad5m-docker` and `cc1-docker` recipes), `src/ui/screensaver_manager.cpp` (factories and includes under `LV_COLOR_DEPTH == 32`), `src/ui/ui_screensaver.cpp` (`static_assert`)
- Delete at ship: `docs/devel/plans/2026-09-14-screensaver-gating-fireworks-design.md`, `docs/devel/plans/2026-09-14-screensaver-gating-fireworks.md`
- Test: CC1 Docker build and binary strings, `[screensaver]`, device runs

**Interfaces:**
- Consumes: Task 0 harness (`arm_measure.sh`, `pi3b_service.sh`, `pi3b_measure.sh`, `embedded_gate.sh`, `wakeup_probe.c`, `summarize.py`), Task 10 fireworks, `scripts/device-env-set.sh <ssh-target> <env-file> <KEY> <VALUE>`, `make cc1-docker`, `make deploy-cc1 CC1_HOST=...`, `make ad5m-docker`, `make deploy-ad5m AD5M_HOST=...`, `scripts/device-profile.sh`, Task 0 `perf_app_log`, `perf_pi3b_take`, `perf_pi3b_release`, `PERF_PACING_ENV`.
- Produces: make variables `SCREENSAVER_16BPP_TARGETS`, `SCREENSAVER_32BPP_ONLY_SRCS`, `DOCKER_SCREENSAVER`; measurement results under `$HELIX_PERF_SCRATCH/results/`.

- [ ] **Step 1: Compile only 16-bit-capable savers on 16 bpp targets**

In `Makefile`, replace the block Task 3 wrote

```make
# Screensaver sources: every saver, the parts they share and the manager
SCREENSAVER_SRCS := $(SRC_DIR)/ui/ui_screensaver.cpp $(wildcard $(SRC_DIR)/ui/screensaver_*.cpp)
# Exclude screensaver when not enabled
ifneq ($(ENABLE_SCREENSAVER),yes)
    APP_SRCS := $(filter-out $(SCREENSAVER_SRCS),$(APP_SRCS))
endif
```

with

```make
# Screensaver sources: every saver, the parts they share and the manager
SCREENSAVER_SRCS := $(SRC_DIR)/ui/ui_screensaver.cpp $(wildcard $(SRC_DIR)/ui/screensaver_*.cpp)
# Targets whose lv_conf.h LV_COLOR_DEPTH is 16. The savers below static_assert 32 bpp, so a
# 16 bpp target missing here fails to compile rather than shipping them.
SCREENSAVER_16BPP_TARGETS := ad5m ad5m-br cc1 mips k1 k1-dynamic ad5x k2 snapmaker-u1
# Savers that draw only at 32 bpp; ScreensaverManager registers them under LV_COLOR_DEPTH == 32.
SCREENSAVER_32BPP_ONLY_SRCS := $(SRC_DIR)/ui/ui_screensaver.cpp \
    $(SRC_DIR)/ui/screensaver_starfield.cpp $(SRC_DIR)/ui/screensaver_starfield_sim.cpp \
    $(SRC_DIR)/ui/screensaver_pipes.cpp
ifneq ($(ENABLE_SCREENSAVER),yes)
    APP_SRCS := $(filter-out $(SCREENSAVER_SRCS),$(APP_SRCS))
else ifneq ($(filter $(PLATFORM_TARGET),$(SCREENSAVER_16BPP_TARGETS)),)
    APP_SRCS := $(filter-out $(SCREENSAVER_32BPP_ONLY_SRCS),$(APP_SRCS))
endif
```

In `src/ui/screensaver_manager.cpp`, replace the three includes `#include "ui_screensaver.h"`, `#include "screensaver_pipes.h"` and `#include "screensaver_starfield.h"` with this block placed after `#include "screensaver.h"`:

```cpp
#if LV_COLOR_DEPTH == 32
#include "screensaver_pipes.h"
#include "screensaver_starfield.h"
#include "ui_screensaver.h"
#endif
```

and replace the constructor with:

```cpp
ScreensaverManager::ScreensaverManager() : cpu_clock_(helix::ui::read_process_cpu_clock) {
#if LV_COLOR_DEPTH == 32
    // These draw at 32 bpp only; a 16 bpp build compiles and registers fireworks alone.
    screensavers_.push_back(std::make_unique<FlyingToasterScreensaver>());
    screensavers_.push_back(std::make_unique<StarfieldScreensaver>());
    screensavers_.push_back(std::make_unique<PipesScreensaver>());
#endif
    screensavers_.push_back(std::make_unique<FireworksScreensaver>());
}
```

In `src/ui/ui_screensaver.cpp`, directly after `using helix::ui::DirtyRect;`, insert:

```cpp

// The sprites are decoded for and tuned on a 32 bpp display; 16 bpp builds leave this saver out
// (Makefile SCREENSAVER_32BPP_ONLY_SRCS).
static_assert(LV_COLOR_DEPTH == 32, "flying toasters draw on a 32 bpp display");
```

In `mk/cross.mk`, directly after the `DOCKER_REMOTE_CONTROL = ...` line, insert:

```make

# Forwards ENABLE_SCREENSAVER into a Docker build when it was given on the command line, for
# measurement builds on boards that do not ship screensavers.
DOCKER_SCREENSAVER = $(if $(filter command line,$(origin ENABLE_SCREENSAVER)),ENABLE_SCREENSAVER=$(ENABLE_SCREENSAVER))
```

and in the `ad5m-docker` and `cc1-docker` recipes change `make PLATFORM_TARGET=ad5m SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) -j$(NPROC_DOCKER_RUN)` and `make PLATFORM_TARGET=cc1 SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) -j$(NPROC_DOCKER_RUN)` to end in `$(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_SCREENSAVER) -j$(NPROC_DOCKER_RUN)`.

- [ ] **Step 2: Prove the CC1 measurement build carries fireworks and nothing else**

```bash
pgrep -x -d' ' 'make|cc1plus'
make cc1-docker ENABLE_SCREENSAVER=yes NPROC_DOCKER_RUN="$JOBS" > "$SS_SCRATCH/cc1-docker.log" 2>&1; echo "exit $?"
for s in 'Starting fireworks' 'Starting flying toasters' 'Starting starfield' 'Starting 3D pipes' 'list_callbacks'; do
  printf '%s: ' "$s"; strings -a build/cc1/bin/helix-screen | grep -c "$s"
done
```

Expected: `exit 0`; `Starting fireworks: 1`, the three other savers `0`, `list_callbacks` at least `1` (the control server is compiled in). Then prove the host build still registers all four: `make test -j"$JOBS" > "$SS_SCRATCH/test-build.log" 2>&1; echo "exit $?"` and `./build/bin/helix-tests "[screensaver]"` (every case passes, including `every registered saver this build draws starts from the manager`).

Mutation (by hand, not reverted by make): remove `cc1` from `SCREENSAVER_16BPP_TARGETS`, rerun the Docker build, confirm it stops on a 32 bpp `static_assert` (whichever 32-bit-only saver the build reaches first); restore `cc1`.

- [ ] **Step 3: Document the build switch, then commit it**

In `docs/devel/SCREENSAVERS.md`, section `## Color depth and builds`, replace `picking the display's depth at compile time.` with:

```markdown
picking the display's depth at compile time. A 16 bpp build with `ENABLE_SCREENSAVER=yes`
compiles only the savers that draw at 16 bpp (Makefile `SCREENSAVER_32BPP_ONLY_SRCS`); the
settings dropdown still lists every row, and choosing one that is not compiled starts nothing.
```

```bash
.venv/bin/clang-format -i src/ui/screensaver_manager.cpp src/ui/ui_screensaver.cpp
git add -- Makefile mk/cross.mk src/ui/screensaver_manager.cpp src/ui/ui_screensaver.cpp docs/devel/SCREENSAVERS.md
git commit -m "build(screensaver): 16 bpp builds compile only the savers that draw at 16 bpp" -m "With ENABLE_SCREENSAVER=yes a 16 bpp target leaves toasters, starfield and pipes out of the sources and the manager, keeping fireworks, and the ad5m and cc1 Docker builds pass the switch through. This is for measurement builds on the CC1 and AD5M, which do not ship screensavers." -m "Mutation: cc1 dropped from SCREENSAVER_16BPP_TARGETS; the CC1 Docker build stopped on a 32 bpp static_assert"
```

- [ ] **Step 4: Measure fireworks per level on the Pi 3B under the load gate**

Preston approved device use this session (Task 3 Step 0)? If not, ask him first. With the Task 3 Step 0 settings, run one arm per level. Level 0 runs at 16 ms by itself, so no refresh override is set; each arm claims `device:pi3b`, checks the print state and runs with `HELIX_EGL_VSYNC=1 HELIX_LOOP_MIN_SLEEP_MS=1`. Wait for each `.done` before the next:

```bash
ARM=fireworks-l0 BUILD=1 HELIX_PERF_BUILD_TREE="$PWD" ENV_EXTRA="HELIX_SCREENSAVER_LEVEL=0" \
  WORKLOADS="idle fireworks" SAVERS="off fireworks" \
  setsid nohup scripts/screensaver-perf/arm_measure.sh > "$SS_SCRATCH/fireworks-l0.log" 2>&1 < /dev/null &
```

Then, reusing the binaries, `fireworks-l1`, `fireworks-l2` and `fireworks-l3` with `BUILD=0 BIN_FROM=fireworks-l0` and `ENV_EXTRA="HELIX_SCREENSAVER_LEVEL=1"`, `2` and `3`, and finally a gated arm `fireworks-gated` with `BUILD=0 BIN_FROM=fireworks-l0 ENV_EXTRA=""` after clearing the stored levels with the Pi held: `perf_pi3b_take "clear stored saver levels" && perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=unset_display KEY=screensaver_levels; perf_pi3b_release`. Summarize them together:

```bash
python3 scripts/screensaver-perf/summarize.py "$HELIX_PERF_SCRATCH"/results/fireworks-*.txt > "$HELIX_PERF_SCRATCH/fireworks_pi3b_summary.txt"
. scripts/screensaver-perf/perf_env.sh
perf_pi3b_take "read stored saver levels" || exit 1
perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=display_value KEY=screensaver_levels
cores=$(perf_ssh 'grep -c ^processor /proc/cpuinfo')
perf_pi3b_release
echo "cores: $cores"
```

The design has the load gate confirm or correct the budget, so compare every level's cost with it. Save as `$SS_SCRATCH/budget_vs_gate.py` (used again on the CC1 and the AD5M):

```python
"""Compares each fireworks level's net CPU with the gate budget and the load-gate verdict.

usage: budget_vs_gate.py pi <cores> <results/fireworks-l0.txt> ... <results/fireworks-l3.txt>
       budget_vs_gate.py embedded <cores> <arm> <results/<arm>.txt>

Net CPU is the fireworks workload minus the idle one, as a percent of one core: the pass CPU
lines on the Pi, the helix_cpu of the gate runs on a BusyBox board. The budget is
helix::ui::saver_budget_share for a board that is not printing.
"""
import math
import statistics
import sys

sys.path.insert(0, "scripts/screensaver-perf")
import summarize  # noqa: E402

LEVELS = 4


def budget_pct(cores):
    return 50.0 if cores >= 4 else {3: 37.0, 2: 25.0}.get(cores, 10.0)


def mean(values):
    return statistics.mean(values) if values else float("nan")


def passes_easily(runs):
    """Passes the load gate with every run under half of both latency limits."""
    return (summarize.gate_passes(runs)
            and all(wakeup < summarize.MAX_WAKEUP_US / 2 for wakeup in runs["max_us"])
            and all(p99 < summarize.MAX_P99_US / 2 for p99 in runs["p99_us"]))


mode, cores = sys.argv[1], int(sys.argv[2])
rows = []
if mode == "pi":
    cpu, _flips, gate, _flags = summarize.load(sys.argv[3:])
    for level in range(LEVELS):
        arm = f"fireworks-l{level}"
        net = mean(cpu[(arm, "fireworks")]["total"]) - mean(cpu[(arm, "idle")]["total"])
        rows.append((level, net, gate[(arm, "fireworks")]))
else:
    arm = sys.argv[3]
    _cpu, _flips, gate, _flags = summarize.load(sys.argv[4:])
    idle = mean(gate[(arm, "off")]["helix_cpu"])
    for level in range(LEVELS):
        runs = gate[(arm, f"level{level}")]
        rows.append((level, mean(runs["helix_cpu"]) - idle, runs))

limit = budget_pct(cores)
print(f"cores={cores} budget={limit:.0f}% of a core")
for level, net, runs in rows:
    if math.isnan(net):
        print(f"level {level}: no CPU figures")
        continue
    verdict = "FAIL"
    if summarize.gate_passes(runs):
        verdict = "PASS easily" if passes_easily(runs) else "PASS"
    side = "within" if net <= limit else "over"
    print(f"level {level}: net CPU {net:.1f}%, {side} budget, gate {verdict}")
    if side == "within" and verdict == "FAIL":
        print(f"FLAG level {level}: within the budget but fails the load gate; the budget is too generous here")
    if side == "over" and verdict == "PASS easily":
        print(f"FLAG level {level}: over the budget yet passes the load gate easily; the budget is too strict here")
```

```bash
python3 "$SS_SCRATCH/budget_vs_gate.py" pi "$cores" "$HELIX_PERF_SCRATCH"/results/fireworks-l[0-3].txt | tee "$HELIX_PERF_SCRATCH/fireworks_pi3b_budget.txt"
```

Expected: `cores=4 budget=50% of a core` and one line per level. A `FLAG` line is evidence for Step 9, not a failure of this step.

Pass criteria: no crash lines in any arm log; `APP_ENV ok` in every arm log; every load-gate row `PASS`; `fireworks-l0` fireworks mean fps at least 57.0 (the 16 ms arms of 2026-09-14 presented 58.6); the gated arm's stored `fireworks` entry (if any) names the level the gate settled on. Put the per-level CPU, fps and gate verdict table, `fireworks_pi3b_budget.txt` with any `FLAG` lines, and each arm's hottest `THERMAL` reading and throttled count in the report to Preston. If level 0 fails the gate or misses 57 fps, stop and report the rows before touching the CC1.

Afterwards clear what the gated arm stored, holding the Pi: `perf_pi3b_take "clear stored saver levels" && perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=unset_display KEY=screensaver_levels; perf_pi3b_release` (each arm already removed its drop-in).

- [ ] **Step 5: Preston's eye check on the Pi 3B**

Tell Preston: "Fireworks is about to run on the Pi 3B panel at level 0 for 35 seconds; please watch it: shells should rise from the hills with a short trail, burst as even balls, trailing chrysanthemums, drooping gold willows and tilted rings, fade through orange with a flicker, and the screen should never flash or tear." Then, holding the Pi:

```bash
. scripts/screensaver-perf/perf_env.sh
perf_pi3b_take "fireworks eye check" || exit 1
perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=dropin "ENV_EXTRA=$PERF_PACING_ENV HELIX_SCREENSAVER_LEVEL=0"
perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=stop
perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=start
perf_run_remote scripts/screensaver-perf/pi3b_measure.sh TYPE=4
```

Record his verdict, then put the Pi back and release it:

```bash
perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=dropin ENV_EXTRA=
perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=stop
perf_run_remote scripts/screensaver-perf/pi3b_service.sh ACTION=start
perf_pi3b_release
```

- [ ] **Step 6: CC1 checkpoint: ask Preston before stressing it**

Ask Preston, with the facts he needs to decide: "I want to run fireworks measurements on the CC1 at <address>: deploy a measurement build (ENABLE_SCREENSAVER=yes, fireworks only), then for about 20 minutes run busy loops on half its cores plus a 1 ms wake-up probe, at the idle panel and at each fireworks level. It must not be printing. OK to go ahead?" Do nothing on the CC1 until he says yes. Then:

```bash
scripts/helix-claim check device:cc1 && scripts/helix-claim take device:cc1 "fireworks measurement" --note "measurement build, busy loops and wake-up probe"
export HELIX_PERF_HOST=<CC1 address from the device roster> HELIX_PERF_USER=root HELIX_PERF_INSTALL=/user-resource/helixscreen
unset HELIX_PERF_PASSWORD   # key auth
curl -s "http://$HELIX_PERF_HOST:80/printer/objects/query?print_stats=state"
```

Expected: the claim succeeds; the Moonraker reply's `print_stats.state` is not `printing` or `paused` (the CC1's Moonraker answers on port 80). If it is, stop and tell Preston.

- [ ] **Step 7: Measure the CC1**

Record what is installed and snapshot it on the host before anything is deployed. The snapshot is the whole install but `config` and `logs`: the XML and assets load at runtime, so they must go back with the binary they belong to.

```bash
. scripts/screensaver-perf/perf_env.sh
ENV_FILE=$HELIX_PERF_INSTALL/config/helixscreen.env
perf_ssh "$HELIX_PERF_INSTALL/bin/helix-screen --version 2>&1 | head -n 1" > "$HELIX_PERF_SCRATCH/cc1-version-before.txt"
cat "$HELIX_PERF_SCRATCH/cc1-version-before.txt"
perf_ssh "cd $HELIX_PERF_INSTALL && tar -cf - \$(ls -A | grep -v -x -E 'config|logs')" > "$HELIX_PERF_SCRATCH/cc1-install-before.tar"; echo "snapshot exit $?"
tar -tf "$HELIX_PERF_SCRATCH/cc1-install-before.tar" | grep -c -x 'bin/helix-screen'
perf_ssh "cat $ENV_FILE" > "$HELIX_PERF_SCRATCH/cc1-env-before.txt"; echo "env exit $?"
```

Expected: a version line, `snapshot exit 0`, `1` and `env exit 0`. Anything else: stop before deploying and tell Preston.

Deploy the measurement build and build the probe:

```bash
make deploy-cc1 CC1_HOST="$HELIX_PERF_HOST" CC1_DEPLOY_DIR="$HELIX_PERF_INSTALL" > "$SS_SCRATCH/deploy-cc1.log" 2>&1; echo "exit $?"
mkdir -p build/screensaver-perf
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src -w /src helixscreen/toolchain-cc1 \
  arm-none-linux-gnueabihf-gcc -std=c99 -O2 -static \
  -o build/screensaver-perf/wakeup_probe-armv7 scripts/screensaver-perf/wakeup_probe.c -lrt; echo "probe exit $?"
restart_cc1() { perf_ssh "/etc/init.d/helixscreen stop; /etc/init.d/helixscreen start"; }
export HELIX_PERF_PROBE=build/screensaver-perf/wakeup_probe-armv7 DUR=60
```

Expected: `exit 0` and `probe exit 0`.

Set the pacing switches every device run uses (EGL vsync does nothing on fbdev, and is set with the floor so every board runs the same switches) and the log level, then restart:

```bash
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_EGL_VSYNC 1
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_LOOP_MIN_SLEEP_MS 1
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_LOG_LEVEL info
restart_cc1
```

Wait with a Monitor until-loop on `perf_ssh 'pidof helix-screen'` printing a pid (60 s at most), then check the app carries the switches and find its log from the file it holds open rather than assuming a path:

```bash
perf_ssh 'tr "\000" "\n" < /proc/$(pidof helix-screen | cut -d" " -f1)/environ | grep -E "^HELIX_(EGL_VSYNC|LOOP_MIN_SLEEP_MS)="'
LOG=$(perf_app_log); echo "log: ${LOG:-none}"
perf_ssh "tail -n 3 $LOG"
```

Expected: `HELIX_EGL_VSYNC=1` and `HELIX_LOOP_MIN_SLEEP_MS=1`; a log path (`docs/devel/LOGGING.md` lists `/user-resource/helixscreen/logs/helix.log` for the CC1, but the open file is what counts); three recent app lines. A missing switch or `log: none`: stop and report.

Idle baseline (no saver forced), three runs:

```bash
for run in 1 2 3; do scripts/screensaver-perf/embedded_gate.sh cc1-fireworks "$run" off; done
```

Then each fireworks level, one level at a time:

```bash
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_SCREENSAVER_NOW fireworks
level=0   # then 1, 2 and 3
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_SCREENSAVER_LEVEL "$level"
restart_cc1
```

Wait with a Monitor until-loop on `perf_ssh "grep -c 'Type 4 running at level $level' $LOG"` becoming non-zero; zero after 60 s means the build or env did not take: stop and report. Then:

```bash
for run in 1 2 3; do scripts/screensaver-perf/embedded_gate.sh cc1-fireworks "$run" "level$level"; done
```

After level 3, summarize and compare with the budget (`budget_vs_gate.py` from Step 4):

```bash
python3 scripts/screensaver-perf/summarize.py "$HELIX_PERF_SCRATCH/results/cc1-fireworks.txt" > "$HELIX_PERF_SCRATCH/cc1-fireworks_summary.txt"
cores=$(perf_ssh 'grep -c ^processor /proc/cpuinfo')
python3 "$SS_SCRATCH/budget_vs_gate.py" embedded "$cores" cc1-fireworks "$HELIX_PERF_SCRATCH/results/cc1-fireworks.txt" | tee "$HELIX_PERF_SCRATCH/cc1-fireworks_budget.txt"
```

Each gate prints its RESULT line; `summarize.py` gives PASS or FAIL per level under the load-gate rule, and the budget file one line per level with any `FLAG`.

Eye check on the CC1 during the level-0 runs: before starting them, tell Preston "The CC1 display now shows fireworks at level 0; please look for banding in the sky gradient and in fading sparks. The 16-bit dither should show a fine even grain, not stripes."

Restore the installed build and its env file, confirm the version is back, and release:

```bash
tar -tf "$HELIX_PERF_SCRATCH/cc1-install-before.tar" | grep -q -x 'bin/helix-screen' || exit 1
perf_ssh "/etc/init.d/helixscreen stop"
perf_ssh "cd $HELIX_PERF_INSTALL && for d in \$(ls -A | grep -v -x -E 'config|logs'); do rm -rf \"\$d\"; done && tar -xf -" < "$HELIX_PERF_SCRATCH/cc1-install-before.tar"; echo "restore exit $?"
perf_ssh "cat > $ENV_FILE" < "$HELIX_PERF_SCRATCH/cc1-env-before.txt"
perf_ssh "/etc/init.d/helixscreen start"
perf_ssh "$HELIX_PERF_INSTALL/bin/helix-screen --version 2>&1 | head -n 1" | diff - "$HELIX_PERF_SCRATCH/cc1-version-before.txt"; echo "version diff exit $?"
```

Expected: `restore exit 0` and `version diff exit 0`; then `scripts/helix-claim release device:cc1`. Anything else: keep the claim, tell Preston the recorded version, and redeploy that release before releasing.

Report to Preston: the summary table (helix CPU and load-gate p99 and max per level against idle), which levels pass, `cc1-fireworks_budget.txt` with any `FLAG` lines, and his eye verdict.

- [ ] **Step 8: AD5M checkpoint and measurement**

Ask Preston the same question for the AD5M (its address, the same load, about 20 minutes, not printing), and wait for his yes. Then claim it, check it is idle, and find the install the deploy overwrites. The AD5M can hold two installs and the running process may come from the other one, so the directory comes from the device profile the deploy itself uses:

```bash
scripts/helix-claim check device:ad5m && scripts/helix-claim take device:ad5m "fireworks measurement" --note "measurement build, busy loops and wake-up probe"
export HELIX_PERF_HOST=<AD5M address from the device roster> HELIX_PERF_USER=root
unset HELIX_PERF_PASSWORD   # key auth
curl -s "http://$HELIX_PERF_HOST:7125/printer/objects/query?print_stats=state"
export HELIX_PERF_INSTALL=$(scripts/device-profile.sh "root@$HELIX_PERF_HOST" -o ConnectTimeout=5 2>/dev/null | sed -n 's/^INSTALL_DIR=//p' | head -n 1)
echo "install: ${HELIX_PERF_INSTALL:-none}"
. scripts/screensaver-perf/perf_env.sh
ENV_FILE=$(perf_ssh "for f in $HELIX_PERF_INSTALL/config/helixscreen.env /etc/helixscreen/helixscreen.env; do [ -f \$f ] && { echo \$f; break; }; done")
ENV_EXISTED=yes
if [ -z "$ENV_FILE" ]; then
    ENV_EXISTED=no
    ENV_FILE=$HELIX_PERF_INSTALL/config/helixscreen.env
    perf_ssh "mkdir -p $HELIX_PERF_INSTALL/config && : > $ENV_FILE"
fi
INIT_SCRIPT=$(perf_ssh "for f in /etc/init.d/S80helixscreen /etc/init.d/S90helixscreen; do [ -f \$f ] && { echo \$f; break; }; done")
echo "env file: $ENV_FILE (existed: $ENV_EXISTED); init script: ${INIT_SCRIPT:-none}"
```

Expected: the claim succeeds; `print_stats.state` is not `printing` or `paused` (otherwise stop and tell Preston); an install path (on the Forge-X rig, `/opt/config/mod/.bin/helixscreen`; `none` stops the step). The launcher reads the first of `<install>/config/helixscreen.env` and `/etc/helixscreen/helixscreen.env` that exists (`scripts/helix-launcher.sh`), so that file is the one changed and restored. `deploy-ad5m` can rewrite the init script from the build, so it is snapshotted too.

Record and snapshot the install, the env file and the init script:

```bash
perf_ssh "$HELIX_PERF_INSTALL/bin/helix-screen --version 2>&1 | head -n 1" > "$HELIX_PERF_SCRATCH/ad5m-version-before.txt"
cat "$HELIX_PERF_SCRATCH/ad5m-version-before.txt"
perf_ssh "cd $HELIX_PERF_INSTALL && tar -cf - \$(ls -A | grep -v -x -E 'config|logs')" > "$HELIX_PERF_SCRATCH/ad5m-install-before.tar"; echo "snapshot exit $?"
tar -tf "$HELIX_PERF_SCRATCH/ad5m-install-before.tar" | grep -c -x 'bin/helix-screen'
perf_ssh "cat $ENV_FILE" > "$HELIX_PERF_SCRATCH/ad5m-env-before.txt"
[ -z "$INIT_SCRIPT" ] || perf_ssh "cat $INIT_SCRIPT" > "$HELIX_PERF_SCRATCH/ad5m-init-before.txt"
```

Expected: a version line, `snapshot exit 0` and `1`. Anything else: stop before deploying.

Build, check the build carries fireworks alone, and deploy to that directory:

```bash
make ad5m-docker ENABLE_SCREENSAVER=yes NPROC_DOCKER_RUN="$JOBS" > "$SS_SCRATCH/ad5m-docker.log" 2>&1; echo "exit $?"
for s in 'Starting fireworks' 'Starting flying toasters' 'Starting starfield' 'Starting 3D pipes'; do
  printf '%s: ' "$s"; strings -a build/ad5m/bin/helix-screen | grep -c "$s"
done
make deploy-ad5m AD5M_HOST="$HELIX_PERF_HOST" AD5M_SSH_TARGET="root@$HELIX_PERF_HOST" AD5M_DEPLOY_DIR="$HELIX_PERF_INSTALL" > "$SS_SCRATCH/deploy-ad5m.log" 2>&1; echo "exit $?"
restart_ad5m() { perf_ssh "killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 1; cd $HELIX_PERF_INSTALL && ./bin/helix-launcher.sh >/dev/null 2>&1 &"; }
export HELIX_PERF_PROBE=build/screensaver-perf/wakeup_probe-armv7 DUR=60
```

Expected: both makes `exit 0`; fireworks `1` and the others `0`. The probe is the static armv7 build from Step 7; `restart_ad5m` is the launcher command `deploy-ad5m` itself uses.

Set the pacing switches and the log level, restart, and find the log:

```bash
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_EGL_VSYNC 1
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_LOOP_MIN_SLEEP_MS 1
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_LOG_LEVEL info
restart_ad5m
```

Wait with a Monitor until-loop on `perf_ssh 'pidof helix-screen'` printing a pid (60 s at most), then:

```bash
perf_ssh 'tr "\000" "\n" < /proc/$(pidof helix-screen | cut -d" " -f1)/environ | grep -E "^HELIX_(EGL_VSYNC|LOOP_MIN_SLEEP_MS)="'
LOG=$(perf_app_log); echo "log: ${LOG:-none}"
perf_ssh "tail -n 3 $LOG"
```

Expected: `HELIX_EGL_VSYNC=1` and `HELIX_LOOP_MIN_SLEEP_MS=1`; a log path, found from the file the app holds open (`docs/devel/LOGGING.md` lists `/data/helixscreen/logs/helix.log` for Forge-X, and `perf_app_log` falls back to `/var/log/messages` only when the app writes there); three recent app lines. A missing switch or `log: none`: stop and report.

Idle baseline, three runs:

```bash
for run in 1 2 3; do scripts/screensaver-perf/embedded_gate.sh ad5m-fireworks "$run" off; done
```

Each fireworks level, one at a time:

```bash
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_SCREENSAVER_NOW fireworks
level=0   # then 1, 2 and 3
scripts/device-env-set.sh "root@$HELIX_PERF_HOST" "$ENV_FILE" HELIX_SCREENSAVER_LEVEL "$level"
restart_ad5m
```

Wait with a Monitor until-loop on `perf_ssh "grep -c 'Type 4 running at level $level' $LOG"` becoming non-zero; zero after 60 s: stop and report. Then:

```bash
for run in 1 2 3; do scripts/screensaver-perf/embedded_gate.sh ad5m-fireworks "$run" "level$level"; done
```

After level 3:

```bash
python3 scripts/screensaver-perf/summarize.py "$HELIX_PERF_SCRATCH/results/ad5m-fireworks.txt" > "$HELIX_PERF_SCRATCH/ad5m-fireworks_summary.txt"
cores=$(perf_ssh 'grep -c ^processor /proc/cpuinfo')
python3 "$SS_SCRATCH/budget_vs_gate.py" embedded "$cores" ad5m-fireworks "$HELIX_PERF_SCRATCH/results/ad5m-fireworks.txt" | tee "$HELIX_PERF_SCRATCH/ad5m-fireworks_budget.txt"
```

Restore the install, env file and init script, confirm the version, and release:

```bash
tar -tf "$HELIX_PERF_SCRATCH/ad5m-install-before.tar" | grep -q -x 'bin/helix-screen' || exit 1
perf_ssh "killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 1"
perf_ssh "cd $HELIX_PERF_INSTALL && for d in \$(ls -A | grep -v -x -E 'config|logs'); do rm -rf \"\$d\"; done && tar -xf -" < "$HELIX_PERF_SCRATCH/ad5m-install-before.tar"; echo "restore exit $?"
if [ "$ENV_EXISTED" = yes ]; then perf_ssh "cat > $ENV_FILE" < "$HELIX_PERF_SCRATCH/ad5m-env-before.txt"; else perf_ssh "rm -f $ENV_FILE"; fi
[ -z "$INIT_SCRIPT" ] || perf_ssh "cat > $INIT_SCRIPT" < "$HELIX_PERF_SCRATCH/ad5m-init-before.txt"
restart_ad5m
perf_ssh "$HELIX_PERF_INSTALL/bin/helix-screen --version 2>&1 | head -n 1" | diff - "$HELIX_PERF_SCRATCH/ad5m-version-before.txt"; echo "version diff exit $?"
```

Expected: `restore exit 0` and `version diff exit 0`; then `scripts/helix-claim release device:ad5m`. Anything else: keep the claim, tell Preston the recorded version, and redeploy that release before releasing. Report the summary table, `ad5m-fireworks_budget.txt` and the passing levels as for the CC1.

- [ ] **Step 9: Put the budget numbers to Preston**

The design has the load gate on the Pi 3B and the CC1 confirm or correct the budget (`saver_budget_share`: 4 or more cores 50%, 3 cores 37%, 2 cores 25%, 1 core 10%). Collect the comparisons:

```bash
cat "$HELIX_PERF_SCRATCH/fireworks_pi3b_budget.txt" "$HELIX_PERF_SCRATCH/cc1-fireworks_budget.txt" "$HELIX_PERF_SCRATCH/ad5m-fireworks_budget.txt"
grep -h '^FLAG' "$HELIX_PERF_SCRATCH"/*_budget.txt
```

For each board, the evidence for its core count is the net CPU of its most expensive level that passes the gate (the budget may sit at or above it) and of its cheapest level that fails (the budget must sit below it). With no `FLAG` line and every pair on the right side of the current number, tell Preston the measurements confirm the budget and name the pairs. Otherwise propose, for each core count a board measured, a whole percent between its two figures, with the rows behind it and the `THERMAL` summaries (a throttled Pi run understates what the board affords), and ask him to choose. A core count no board measured keeps its number.

Change nothing he has not approved. If he approves new numbers, set them in `src/ui/screensaver_gate.cpp#saver_budget_share`, in the test `the saver budget follows the core count and halves while printing` (`tests/unit/test_screensaver_gate.cpp`), and in `docs/devel/SCREENSAVERS.md` where it states the budget (`grep -n '37%' docs/devel/SCREENSAVERS.md`); build the tests, run `./build/bin/helix-tests "[screensaver_gate]"`, then commit with `BUDGETS` set to the approved numbers as text:

```bash
.venv/bin/clang-format -i src/ui/screensaver_gate.cpp tests/unit/test_screensaver_gate.cpp
git add -- src/ui/screensaver_gate.cpp tests/unit/test_screensaver_gate.cpp docs/devel/SCREENSAVERS.md
git commit -m "fix(screensaver): gate budget per core count from the load-gate measurements" -m "The Pi 3B, CC1 and AD5M fireworks runs under the load gate put the saver budget at $BUDGETS: the most expensive level each board runs within the gate stays inside it, and the cheapest level it cannot run stays outside." -m "Mutation: the previous number restored for one changed core count; the saver budget test went red"
```

- [ ] **Step 10: Completion gates**

Run the full suite and mutation gate in the foreground, one at a time:

```bash
make test-run > "$SS_SCRATCH/test-run.log" 2>&1; echo "exit $?"
make mutate-diff > "$SS_SCRATCH/mutate-diff.log" 2>&1; echo "exit $?"
grep -n -E '^base |^diff |SURVIVED|NOT COVERED|CLEAN|INCOMPLETE' "$SS_SCRATCH/mutate-diff.log"
```

Expected: `make test-run` exits 0. For `make mutate-diff`, first check the `base` line names the branch this work was cut from; for every `SURVIVED` hunk either add the test that kills it (its own commit, with a `Mutation:` line) or confirm it is one of the four survivors this plan expects on a STANDARD test host: the tier expression in `DisplaySettingsManager::init_subjects` (named in the Task 7 commit), the removal of the toasters' tier input in Task 6 (the lowest ladder rung keeps the cap), the `set_host` call in `DisplayManager::init` (proven on the Pi 3B in Task 6 Step 13), and the hide-before-free order in `SaverCanvas::release` (Task 3 Step 13). List every survivor, expected or not, in the report to Preston. `NOT COVERED` paths under `lib/` or `tests/` are expected.

ASAN runs on zeus against a pushed commit. Ask Preston before pushing the branch; after he agrees:

```bash
git push -u origin HEAD
scripts/zeus-run.sh asan '[screensaver_parts],[pixel_writer],[fireworks_sim],[screensaver_fireworks],[starfield_sim],[screensaver_canvas],[screensaver_gate]' > "$SS_SCRATCH/zeus-asan.log" 2>&1; echo "exit $?"
grep -n -E 'ERROR: AddressSanitizer|test cases|assertions' "$SS_SCRATCH/zeus-asan.log"
```

Expected: `exit 0`, no `ERROR: AddressSanitizer`, and a Catch2 totals line with a non-zero test-case count (an ASAN run that prints no totals ran nothing). No TSAN run: the work adds no threads.

- [ ] **Step 11: Remove the plan documents in the shipping change**

The durable knowledge now lives in `docs/devel/SCREENSAVERS.md`, `docs/devel/ENVIRONMENT_VARIABLES.md`, the user docs and the code. In the change that ships the work (the merge of this branch), delete the spec and this plan:

```bash
grep -rn '2026-09-14-screensaver-gating-fireworks' docs --include=*.md; echo "references exit $?"
rm docs/devel/plans/2026-09-14-screensaver-gating-fireworks-design.md docs/devel/plans/2026-09-14-screensaver-gating-fireworks.md
git add -- docs/devel/plans/2026-09-14-screensaver-gating-fireworks-design.md docs/devel/plans/2026-09-14-screensaver-gating-fireworks.md
git commit -m "docs(screensaver): drop the gating and fireworks spec and plan now the work has shipped" -m "The registry, foundation, gate and fireworks are described in docs/devel/SCREENSAVERS.md, the switches in ENVIRONMENT_VARIABLES.md and the settings in the user docs; the Pi 3B, CC1 and AD5M measurements are in the branch's commit messages and the report to Preston." -m "Mutation: none; documentation only"
```

Expected: the reference grep prints only lines inside the two files being deleted (`references exit 0` with those two paths), and the commit removes both files. If any other doc cites them, fix that citation in the same commit.
