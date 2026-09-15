# Screensaver gating and fireworks

Status: design and spec approved by Preston 2026-09-14. Project 1 of the four from the
2026-09-14 screensaver brainstorm (gating + fireworks, then plotter, then plasma demo). Builds on
`feature/screensaver-smoothness` (results in `docs/devel/GPU_ACCELERATION.md`, "Pi 3B screensaver pacing") and starts
after that branch lands. Implementation plan:
`docs/devel/plans/2026-09-14-screensaver-gating-fireworks.md`.

## Goal

Every board that can afford a screensaver runs one, at the best quality it can sustain without hurting
the printer, decided by measuring the saver on that board rather than by a RAM tier. The first new saver,
fireworks, proves the design and runs on 16-bit boards, with the CC1 as the floor reference and the AD5M
as the second data point.

Success means all of:

- Each saver measures its own CPU cost, steps down its quality ladder when over budget, and remembers the
  level per board and app version.
- Screensaver code shares one foundation; no saver rolls its own overlay, canvas, timer, pixel format,
  dirty-area handling or registration.
- The three existing savers run on that foundation with pixel-identical output and no measured
  regression on the Pi 3B.
- Fireworks runs on the Pi 3B at 60 fps within the load gate, and a CC1 and an AD5M measurement build
  answer whether 16-bit embedded boards can run a screensaver at all.

## Facts the design rests on

- `PlatformCapabilities::classify_tier` (`src/system/platform_capabilities.cpp`): EMBEDDED under 512 MB
  or one core, STANDARD at 2048 MB and 4 cores, BASIC otherwise. The Pi 3B reports 856 MB and is BASIC.
- Fresh installs default the saver to Off on BASIC and EMBEDDED
  (`src/system/display_settings_manager.cpp#init_subjects`). Config migration v16
  (`src/system/config.cpp#migrate_v15_to_v16`) turned toasters off once on those tiers and showed a notice.
- AD5M, AD5X, CC1, K1, K2 and U1 builds set `ENABLE_SCREENSAVER=no` and render RGB565 (`lv_conf.h`).
  Starfield and pipes `static_assert` 32 bpp, and `StarfieldSim` writes 4-byte pixels.
- `sleep_while_printing` defaults to true, so savers run during prints; no saver checks print state.
- Nothing in the app measures CPU time. The only frame metric is main-thread wall time of
  `lv_timer_handler` (telemetry). LVGL's software draw thread is unnamed and its handle is private.
- The saver type upper bound 3 is hardcoded in `ScreensaverManager::configured_type` and two clamps in
  `DisplaySettingsManager`, and a test expects 99 to clamp to 3.
- Pi 3B, 2026-09-14: changed-area size dominates cost; LVGL invalidates the whole screen once more than
  32 areas are pending (`LV_INV_BUF_SIZE`); the dumb-DRM binary draws 2D savers at about a third of the
  EGL binary's CPU.

## Decisions (approved)

| Topic | Decision |
|---|---|
| Gating | Self-measuring. Budget is a share of one core, calibrated with the load gate. Step down only within a session. |
| Tiers | The Pi 3B becomes the STANDARD floor. Savers ignore tiers entirely once gated. |
| Default saver | Flying toasters on every board that builds savers, for fresh installs. |
| Existing users | Saved choices are untouched, including configs v16 turned off. |
| Type count | No hardcoded count anywhere; one registry. |
| DRY | Shared foundation; the existing savers are refactored onto it before fireworks. |
| Color depth | Fireworks and the shared parts support RGB565 and XRGB8888. The existing savers stay 32-bit until a later port. |
| Embedded check | CC1 (floor) and AD5M measurement builds with fireworks only. Preston approves each device run. |
| Fireworks look | Realistic night sky, hills horizon, four burst types, no logo bursts. |

## Design

### 1. Shared foundation

Small parts with one job each, usable and testable alone:

| Part | Owns |
|---|---|
| Registry | One row per saver: type, stable name (`toasters`, `starfield`, `pipes`, `fireworks`), label translation key, supported color depths. Type, names, count and label keys live in an always-compiled header so `DisplaySettingsManager` can clamp without saver sources; factories live in the manager. |
| `SaverOverlay` | The black, touch-absorbing overlay on `lv_layer_top()`, the switch to transparent once a canvas covers it, and deferred deletion. |
| `SaverCanvas` | Buffer allocation at LVGL's stride, canvas format per color depth, opaque fill, hide-before-free on stop, invalidation of a list of dirty areas merged to at most 32, and the dirty-box `finish_layer` for scenes drawn with `lv_draw_*` (the body pipes owns today). |
| `SaverFrameTimer` | The frame timer at the current level's period plus the `MotionClock`, safe cancel from stop and the destructor, and applying a new period without resetting motion. |
| `PixelWriter` | Writes a colour into an RGB565 or XRGB8888 frame (X byte 0xFF), a 4x4 ordered dither for fades on RGB565, per-channel max blending, and a line rasteriser for the plotter. |
| Saver base | Sequences the parts: `start` (overlay, canvas, seeded random sequence, timer, level), frame (`on_frame(dt) -> dirty areas`), `stop` (timer, canvas hide, buffer, overlay), and level requests. A saver implements only `on_start`, `on_frame`, `on_stop` and its ladder. |
| Test access | One generic access class through the base (overlay, canvas, timer, seed, level). Per-saver access classes remain only for simulation state the base does not own (pipes grid, stars, sprites). |

The settings dropdown keeps its option list in `ui_xml/settings_display_sound_overlay.xml` (UI rule); a
test parses that list and fails if its order or length differs from the registry.

### 2. The gate

1. **CPU clock seam.** Reads `CLOCK_PROCESS_CPUTIME_ID` and the steady clock. Process time covers the
   LVGL draw thread without touching LVGL internals. Tests inject samples.
2. **Idle baseline.** While no saver runs, `ScreensaverManager` samples the clock from the display
   manager's idle check, which runs every main-loop iteration, at most every 250 ms. The baseline is the
   CPU rate over the last idle stretch of up to 10 s. Under 3 s of samples, the baseline is 0, which can
   only make the gate step down early, never late.
3. **Window.** After a saver starts, the first second is ignored. Then each 5 s window, for as long as the
   saver runs, computes `(cpu delta - baseline * wall) / wall` as a share of one core. Evaluating every
   window catches a print that starts mid-run. Process time also counts other app threads, so a burst of
   background work can cause an early step down; that errs toward protecting the printer.
4. **Budget.** By core count: 4 or more cores 50%, 3 cores 37%, 2 cores 25%, 1 core 10%. Halved while a
   print is running. The display manager passes an `is_printing` callback so the manager does not depend
   on printer code. The load gate on the Pi 3B and the CC1 confirms or corrects these numbers.
5. **Decision (pure function).** Over budget: step one level down at the saver's next natural break. Over
   budget at the bottom level: mark the board too heavy. A session is one run of a saver from start to
   stop; within it the level never steps up, and the next run starts at the stored level.
6. **Ladders.** Every saver declares its levels, most expensive first; each level carries a frame period.
   Level 0 runs at 16 ms (a 60 Hz panel) by default, and `HELIX_SCREENSAVER_REFR_PERIOD_MS` overrides it
   for manual testing; the smoothness branch ships the pacing defaults (EGL vsync, main-loop floor) that
   keep 16 ms even. Starfield and pipes get two levels: 16 ms, then 33 ms. Toasters get
   three: 16 ms with every sprite, 33 ms with every sprite, 33 ms with 10 sprites, which replaces today's
   tier-based sprite cap. Motion is time-based, so a period change mid-run is seamless.
7. **Too heavy.** The session falls back to a static black `SaverOverlay` with no frame timer, logs once
   why, and the store remembers it until the app version or board fingerprint changes.

### 3. Level store

Config-only setting at `/display/screensaver_levels/<name>`:
`{"level": n, "too_heavy": bool, "version": "...", "board": "..."}`.

- `version` is `helix_version_full()`.
- `board` is a pure function of the running display backend (drm, egl, fbdev, sdl), CPU core count,
  bogomips rounded to 100, resolution and colour depth.
- A mismatch in either starts again from level 0. Malformed entries are ignored.

### 4. Defaults, tiers and registration

- `STANDARD_RAM_THRESHOLD_MB` drops from 2048 to 768 (a 1 GB Pi reports about 856 MB); 4 cores stays
  required. This also turns on UI animations and fuller charts for 1 GB quad-core boards, so it lands as
  its own commit after a Pi 3B UI check under the load gate.
- Fresh installs default `screensaver_type` to toasters wherever savers are built. Existing configs are not
  migrated. The user docs (`docs/user/CONFIGURATION.md`, `docs/user/guide/settings/display-sound.md`,
  `config/settings.json.template`) are corrected to match.
- Fireworks is type 4: registry row, dropdown option, `Fireworks` in all 9 translation files (plus a CJK font rebake for the new glyphs),
  `HELIX_SCREENSAVER_NOW` name from the registry, Makefile screensaver filter and ESP32
  `app_srcs_excluded.txt` entries for new sources.

### 5. Fireworks

- **Pure simulation.** `FireworksSim` has no LVGL, clock or shared random sequence; it draws through
  `PixelWriter` into a frame of either depth and returns dirty areas.
- **Sky, computed not stored.** Vertical gradient from deep navy to near black, about 80 faint fixed stars,
  and a hills silhouette held as one height per column. Erasing a spark recomputes the sky colour at its
  old pixels, so no background buffer is allocated.
- **Shells.** Launch from random positions near the bottom, rise and slow with a short spark trail, burst
  near the apex in the upper 60% of the screen.
- **Bursts.** Peony (even ball), chrysanthemum (sparks leave trails), willow (long-lived gold, heavy drag,
  droops), ring (tilted flat circle). Sparks have gravity, drag and a lifetime; colour runs white flash,
  shell colour, ember orange, fade, with random flicker late in life.
- **Drawing.** A spark is 1 or 2 pixels plus a short fading trail, blended by per-channel max against the
  sky. RGB565 fades use the ordered dither.
- **Pacing.** One shell every 0.8 to 2.5 s; every 3 minutes or so a finale of 6 to 10 shells over about 4 s.
- **Cost.** One dirty box per active rocket or burst. Sparks come from a pool allocated at start (about
  30 KB at level 0); a full pool drops new sparks rather than allocating.
- **Ladder** (applied when the next shell launches; all tunables in one table):

| Level | Frame period | Sparks per burst | Trail | Bursts at once |
|---|---|---|---|---|
| 0 | 16 ms | 150 | 5 | 6 |
| 1 | 33 ms | 150 | 5 | 6 |
| 2 | 33 ms | 90 | 3 | 4 |
| 3 | 50 ms | 50 | 0 | 3 |

### 6. Environment switches

Documented in `docs/devel/ENVIRONMENT_VARIABLES.md`, parsed strictly (malformed values warn and are ignored):

- `HELIX_SCREENSAVER_BUDGET_PCT` overrides the budget, to prove step-down on a device.
- `HELIX_SCREENSAVER_LEVEL` forces a level and disables the gate for that run.

## Order of work

Each step is its own commit, test-first, reviewed; existing tests stay green throughout.

1. Registry and type count; remove the hardcoded bound.
2. Pixel fingerprint tests for pipes and starfield (fixed seed, N frames), captured on the current code.
3. Shared parts and base; refactor pipes; Pi 3B re-measure.
4. Refactor starfield; Pi 3B re-measure.
5. Refactor toasters (overlay, timer, levels; no canvas); Pi 3B re-measure.
6. Gate, CPU clock seam, level store, manager integration, env switches, too-heavy fallback.
7. Default saver for fresh installs and the user doc fixes.
8. STANDARD floor change, after the Pi 3B UI check.
9. `PixelWriter` RGB565 path and dither; `SaverCanvas` 16-bit format.
10. Fireworks saver and simulation, registration, translations, docs, including a durable
    `docs/devel/SCREENSAVERS.md` so this spec and its plan can be deleted when the work ships.
11. Device verification (below).

## Testing

Unit tests go red first:

- **Gate:** budget per core count and while printing; warm-up ignored; baseline subtracted, and 0 under
  3 s of samples; over budget steps down; bottom level over budget marks too heavy; no step up within a
  session.
- **Level store:** version or any fingerprint component change restarts at level 0; malformed entries
  ignored; round trip.
- **Registry:** count from the registry; XML option list matches it; out-of-range types clamp to the last
  type; `HELIX_SCREENSAVER_NOW` names resolve.
- **Shared parts:** stride and format per depth; hide before free; merged dirty areas number at most 32 and
  cover every input; `PixelWriter` exact bits for both formats including the X byte; dither pattern.
- **Refactors:** the fingerprints from step 2 are unchanged after each refactor. They are recorded for the
  x86-64 Linux test build and skip elsewhere, since antialiased float math can move a pixel.
- **Fireworks:** deterministic per seed; dirty areas cover every changed pixel (buffer diff); the spark pool
  never grows; a level request waits for the next launch; the simulation runs on a 32-bit and a 16-bit frame.

Completion gates: `make test-run`, `make mutate-diff` with the surviving mutation named per commit, and
ASAN on zeus (`scripts/zeus-run.sh asan`) for the canvas and pixel code. No new threads, so no TSAN run.

## Device verification

- **Pi 3B:** re-measure after each refactor against the 2026-09-14 numbers; fireworks at each level under
  the load gate (`cyclictest --policy=other` plus `stress-ng --cpu 2`, every run max under 20 ms and p99
  under 5 ms); prove the gate end to end by forcing a small `HELIX_SCREENSAVER_BUDGET_PCT`, watching the
  level drop, restarting, and confirming the stored level is used.
- **CC1 (floor), then AD5M:** measurement builds with `ENABLE_SCREENSAVER=yes` and only 16-bit-capable
  savers compiled (fireworks). These builds are not shipped: their settings dropdown still lists the
  32-bit savers. Neither board has `cyclictest` or `stress-ng`, so the load gate uses a statically built
  wake-up latency probe (1 ms `clock_nanosleep` loop recording overshoot) and busy-loop processes. No print
  runs during measurement, and Preston approves each device before it is stressed.
- **Eye checks (Preston):** fireworks on the Pi 3B panel, and the 16-bit dither on the CC1 display.

## Out of scope

- Porting toasters, starfield and pipes to RGB565 and shipping savers on embedded boards: a follow-up,
  decided by the CC1 and AD5M measurements.
- Shipping-default changes to the Pi 3B display binary (dumb DRM versus EGL): separate decision.
- Projects 3 and 4, recorded here so their decisions travel with the repo:
  - **Plotter:** a model printed layer by layer while the camera moves. Ladder: orbit with vector-display
    glow, orbit with blueprint lines, glide-and-hold camera, fixed camera. Victory lap then a top-down
    fade per model. Movie-style HUD: scrolling simulated G-code column (shed first), instrument corner with
    simulated temperatures, real printer name and clock, frame dressing. Seven models derived offline from
    G-code into a compact format: Voron Design Cube, Cali Cat, spiral vase, Utah teapot, HelixScreen logo,
    3DBenchy, planetary gear set.
  - **Plasma demo:** software renderer first (low-resolution sine and lookup tables, 4x4 blocks, palette
    cycling) behind a renderer interface, GPU shader path for EGL boards later. v1 plasma, palette themes
    and a sine scroller; tunnel, rotozoomer and copper bars later. Scroller interleaves demoscene greetings
    (no Bambu) with live printer status; status phrases are translated.
