# Screensaver smoothness on low-tier boards (Pi 3B)

## Context

Flying toasters are visibly jerky on the Pi 3B. Measured on the device (helix-screen-egl,
800x480, no rotation, tier=basic), 2026-09-14:

| Workload | CPU (one core) | Presented frames/s | Frame interval |
|---|---|---|---|
| Idle home | 2.5% | - | - |
| Flying toasters | 5.8% | 6.6 | 90-222 ms, p50 149 |
| Starfield | 78.6% (draw thread 52, main 25) | 16 (target 30) | min 50, p50 58 |

Toaster jank is self-inflicted: BASIC tier (RAM < 2 GB) pins the tick to 150 ms, motion is a
fixed step per tick, and `lv_timer` re-arms from its actual run time. Starfield and pipes hit
a real ceiling: full-screen invalidation, transparent canvases, and the whole panel UI redrawn
under the top-layer overlay. The user asked for all five fixes, and for screensaver work on a
separate thread/core where possible. The plan was reviewed adversarially and the amendments are
folded in below.

Outcome: toasters at display refresh rate with even intervals; starfield and pipes at or near
30 fps on the 3B; no measurable harm to a Klipper-like task sharing the CPU; pipeline changes
kept only where an A/B on the 3B shows a win.

## Order

1. **P0** GL debug off A/B (5c), plus the load-gate baseline on the unchanged binary.
2. **P3** hide the panel under the saver, with the gcode viewer watchdog guard.
3. **P1+P2** time-based motion at refresh rate, RNG owned by each sim. Load gate.
4. **P4a** transparent overlay, XRGB canvases, pipes bbox invalidation. Measure + load gate.
5. **P4b** starfield worker behind `HELIX_SCREENSAVER_WORKER`, A/B gated.
6. **P5** 5a, 5b, 5d, 5e, 5f A/B.

Per phase: failing tests, implementation (delegated), `/code-review`, fixes, commit, 3B
measurement. Worktree branch `feature/screensaver-smoothness`; copy this plan into
`docs/devel/plans/` on day one and delete it in the shipping change.

## Design

### P0 GL debug

`LV_USE_OPENGLES_DEBUG 0` inside the existing `#ifdef HELIX_ENABLE_OPENGLES` block in
`lv_conf.h` (defaults to 1: `glGetError` before and after every `GL_CALL`). Confirm first that
it gates only the `GL_CALL` macro, so shared objects cannot disagree. Winner becomes the
baseline.

### P3 Hide the panel under the saver

- LVGL's cover search walks only the active screen, and the top layer is drawn unconditionally
  (`lib/lvgl/src/core/lv_refr.c#refr_configured_layer`). A hidden screen is skipped, and every
  invalidate from its descendants is dropped (`lv_obj_pos.c#lv_obj_invalidate_area` via
  `lv_obj_area_is_visible`), so live bindings stop producing dirty rects too.
- **One reference-counted helper keyed on the screen pointer**, in an always-compiled file
  (`src/application/display_manager.cpp`; `screensaver_manager.cpp` is filtered out when
  `ENABLE_SCREENSAVER=no`). It leaves an already-hidden screen hidden, never unhides a screen
  it did not hide, and a screen swapped mid-hold is left alone. Annotate the flag write
  `// DECLARATIVE_OK: screen hidden under an opaque top-layer overlay`
  (`scripts/check_imperative_ui.py` counts it).
- **Callers**: `ScreensaverManager::start`/`stop` (every path reaches the saver through it:
  idle, preview, `HELIX_SCREENSAVER_NOW`, Z key, wake, `enter_sleep`). Hold only when
  `ss->is_active()` after start (today `active_` is set even when the saver bailed); a type
  switch must not release between stop and start. Also
  `DisplayManager::create_sleep_overlay`/`destroy_sleep_overlay`: that path serves every
  SoftwareOverlay device, not just the Pi.
- **Gcode viewer watchdog guard**: `suspend_active` deactivates only the topmost overlay, and
  the forced and Z-key paths do not suspend, so print status can sit under a hidden screen with
  its `DRAW_POST`-driven cache frozen; the watchdog would give up after ~60 s and raise
  "Failed to load G-code preview". Add `bool visible` to `WatchdogObservation`
  (`include/gcode_viewer_watchdog.h`); `watchdog_evaluate` resets the streak with no kick and no
  give-up when false; `gcode_viewer_watchdog_cb` fills it from `lv_obj_is_visible(obj)`. Audit
  other draw-driven progress (`ui_bed_mesh.cpp`, `ui_temp_graph.cpp`,
  `ui_frequency_response_chart.cpp`) for the same coupling.
- Stays hidden until wake (were already under the opaque overlay): modals, notification panel,
  temp-graph overlay, software cursor, all children of the active screen. On the top layer and
  unaffected: toasts, lock screen. `lv_snapshot_take` redraws a hidden root and ctl filters
  only children, so screenshots and `describe_screen` keep working.

### P1+P2 Time-based motion at refresh rate

Header-only `include/screensaver_motion.h` (namespace `helix::ui::screensaver`; avoids the
Makefile screensaver filter and ESP32 `app_srcs_excluded.txt`), precedent
`src/ui/ui_confetti.cpp`:

```cpp
struct MotionClock { void reset(uint32_t now_ms); uint32_t advance(uint32_t now_ms); };  // wrap-safe, clamps one gap
struct FlightPos { int32_t x, y; bool started; };
constexpr FlightPos flight_pos_at(uint32_t elapsed_ms, int32_t sx, int32_t sy, int32_t fly_ms, int32_t delay_ms, int32_t distance);
constexpr uint8_t flap_frame_at(uint32_t elapsed_ms, int32_t delay_ms, uint32_t frame_step_ms, uint8_t initial_frame);
struct StepAccumulator { uint32_t steps_due(uint32_t dt_ms, uint32_t step_ms, uint32_t cap); };
```

- **Toasters** (`src/ui/ui_screensaver.cpp`): period = `lv_display_get_refr_timer(disp)->period`
  via `misc/lv_timer_private.h` (9.5 has no getter); tier periods deleted; flap becomes a clean
  0,1,2,3,2,1 ping-pong at 50 ms x `ticks_per_flap`; sprite cap re-decided from a 10-vs-49
  measurement on the 3B; the "7 fps" comments in `include/ui_screensaver.h` and the .cpp go.
- **Starfield**: `z -= speed * dt_ms / 33.0f`; the sim owns a `std::minstd_rand`, replacing
  `srand(time)`/`rand()`, so tests can seed it.
- **Pipes**: grow per alive pipe when `StepAccumulator` says a 100 ms step is due, capped per
  call; steps still due at a grid reset are dropped; RNG owned as above.
- **Cadence**: the saver timer sits ahead of `lv_display_refr_timer` (`lv_timer.c` inserts at
  the head), and its invalidate resumes the refresh timer, which pauses itself every refresh,
  so the next refresh takes each update. They are not phase-locked (each re-arms from its own
  run time; the main loop sleeps 5-33 ms), so cadence is judged by flip-interval spread.
- start -> stop -> start on the same instance resets clock, accumulator, RNG and dirty state
  (the manager reuses instances).

### Load gate: a Klipper-like task sharing the 3B

Migration v16 (`src/system/config.cpp`) turned toasters off on BASIC/EMBEDDED because they
"cause Klipper print failures" (no issue or detail on record), `sleep_while_printing` defaults
on, and refresh-rate toasters are ~4.5x the timer work. Klipper's host-side failures come from
the klippy process waking late. So measure exactly that on the 3B, with no printer involved:

- **Probe**: `cyclictest --policy=other -i 1000 -D 60 -q -h 500` at nice 0 (klippy's class).
  Records max and p99 wake-up latency. Neither tool is on the 3B yet; both are in its apt
  (`sudo apt install rt-tests stress-ng`: rt-tests 2.6, stress-ng 0.19.02). helixscreen already
  runs there at nice 10 under the launcher, which is the production setting to keep.
- **Background load**: `stress-ng --cpu 2` (klippy step compression + moonraker + a webcam
  streamer), with helixscreen running under its launcher as in production.
- **Arms**: saver off (baseline); current binary's 150 ms toasters; then each saver on the
  phase's build (toasters, starfield, pipes; worker on and off in P4b). Three interleaved 60 s
  runs per arm; also record helixscreen CPU and flip rate in the same window.
- **Pass**: probe max and p99 stay within the saver-off baseline's run-to-run spread.
- **Fail**: on BASIC/EMBEDDED, fall back to a slower saver cadence while a print is active
  (`print_state` printing), and re-run the gate. Migration v16 stays as is either way; the
  commit body says why.

### P4a Transparent overlay, opaque canvases, pipes bbox

- Starfield and pipes canvases use `LV_COLOR_FORMAT_XRGB8888` (`static_assert` on 32 bpp;
  `screensaver_canvas_stride_bytes` in `include/screensaver.h` takes the format). That makes
  the canvas draw a straight copy. It does not skip the overlay fill, because top-layer
  children are never cover-culled, so after the buffer is attached the overlay's `bg_opa`
  becomes `LV_OPA_TRANSP`: the canvas covers every pixel.
- **The X byte must stay 0xFF** (production 32 bpp displays are ARGB8888 and XRGB->ARGB is a
  raw 4-byte copy; alpha trap in `docs/devel/GPU_ACCELERATION.md`). `lv_canvas_fill_bg` and
  solid fills write 0xFF, AA mixes write only RGB, so fill before any drawing, including the
  pipes reset.
- **Pipes bbox**: `lv_display_enable_invalidation(false)`, `lv_canvas_finish_layer`,
  `lv_display_enable_invalidation(true)`, then `lv_obj_invalidate_area(canvas, &bbox)`. The
  enable flag is a counter, so it nests with Android and sleep suppression, and no LVGL
  internals are copied. bbox = union of the segment box (`+/- thickness`) and joint box
  (`+/- ball_r`); the grid reset keeps a full invalidate. Pipes keep LVGL's AA raster, which
  already runs on LVGL's draw thread (not the main thread).
- **stop()**: hide the canvas (or point it at a static 1x1 buffer) before freeing its buffer;
  wake's `lv_refr_now` and top-layer screenshots can draw it before the async delete runs.

### P4b Starfield worker, A/B gated

- Pure `StarfieldSim::step(dt_ms, FrameTarget&, std::minstd_rand&) -> DirtyRect`, no LVGL /
  `lv_malloc` / `lv_tick_get` inside (threading rule 1). The inline path calls the same step, so
  the two can only differ in where they run.
- Spawn only when `cpu_cores >= 4` and tier != EMBEDDED; `SCHED_IDLE` on the worker
  (`PWMSoundBackend::apply_render_thread_priority()`, `src/system/pwm_sound_backend.cpp`); spawn in
  `try/catch(std::system_error)` with inline fallback. `HELIX_SCREENSAVER_WORKER=0|1` forces
  either path for the A/B.
- **Main never blocks on the idle-priority worker** (no priority inheritance on `std::mutex`,
  and wake runs through `wake_display`): the worker keeps one persistent frame; the main timer
  `try_lock`s it and skips a pickup when busy; dirty rects accumulate until picked up, and only
  those rows are copied into the canvas buffer. `stop()` sets an atomic `stop_` and notifies,
  and a one-shot timer joins once the worker sets `exited_`; only the destructor joins blocking
  (the manager is a function-static singleton, so a joinable member at static teardown calls
  `std::terminate`). The worker checks `stop_` inside a step. Skeleton:
  `BedMeshRenderThread` (`src/rendering/bed_mesh_render_thread.cpp`).
- Worker buffers are `std::vector`, never `lv_malloc`; only the main thread writes the canvas
  buffer, between refreshes (#1102 class). Two extra 1.5 MB buffers at 800x480.
- **Default**: on only if the A/B shows the main thread's share drops at equal or better
  starfield frame rate and the load gate passes; otherwise the switch defaults off and the
  numbers go in the commit body.

### P5 Pipeline experiments (A/B on the 3B, keep only wins)

One knob per arm, three interleaved runs, workloads = toasters, starfield, pipes, idle home.
**Keep rule**: lower CPU at equal frames/s, or more frames/s at equal CPU, beyond run-to-run
spread, no visual defect, and one spot-check run on the Pi 5 (192.168.1.113), since the `pi`
binary ships there too.

| Knob | Change | Rebuild / binaries | Watch |
|---|---|---|---|
| 5a EGL partial upload | patch `lv_linux_drm_egl.c`: `glTexSubImage2D` per flushed area; full `glTexImage2D` when the texture id or size changes, after `set_color_transform`, and when a no-op flush cb (splash, panel sleep) is swapped back out | EGL objects | vc4 may stall on a sub-upload into a live texture; measure |
| 5b XRGB8888 on EGL | skip the ARGB force in `display_backend_drm.cpp#create_display`; shader ignores X via `u_ColorDepth = 24` (v100 and v300es branches, no new uniform) | EGL objects | alpha-trap path: eye check mandatory; keep `u_SwapRB` for `make verify-egl`. The 3D viewer has its own context and shaders |
| 5d refresh period | runtime `HELIX_REFR_PERIOD_MS`: `lv_timer_set_period` on refresh, anim, indev read and UpdateQueue timers, and lower the main loop's hardcoded 33 ms cap; try 16 | one app file, runtime-gated | uneven cadence when frames miss 16 ms: judge by interval spread; re-run the load gate if kept |
| 5e draw threads | `LV_DRAW_SW_DRAW_UNIT_CNT 2`, sub-arm `lv_display_set_tile_cnt(disp, 1)` | full rebuild, own `BUILD_SUBDIR` | widens the #1102/#1338 layer UAF window; the patched 1 ms `wait_for_finish` poll likely eats the gain. If promising, replace the poll with an `lv_thread_sync_t` wait, then ASAN + TSAN |
| 5f libc string | `LV_USE_STDLIB_STRING LV_STDLIB_CLIB` | full rebuild | smaller gain than it looks (builtin already copies words when aligned); glibc `memcpy` rejects forward overlap, so ASAN run first |

Kept `lv_conf.h` knobs are scoped to the Pi builds. The aarch64 `pi`, `pi-fbdev` and
`pi-both` targets have no platform define today, so add `-DHELIX_PLATFORM_PI` to their
`TARGET_CFLAGS` in `mk/cross.mk` (that reaches LVGL, splash and watchdog objects uniformly) and
scope with `HELIX_PLATFORM_PI || HELIX_PLATFORM_PI32`. Never with `HELIX_ENABLE_OPENGLES`,
which only the EGL variant objects get. Kept LVGL changes become `patches/` entries via the
pristine-file method, wired in `mk/patches.mk` after the blocks that guard on a clean file,
then `make reapply-patches` from clean. Results go in `docs/devel/GPU_ACCELERATION.md`.

### Eye checks on the panel (Preston)

`ctl` drives to each state; only a person at the 3B's panel can confirm: toasters smooth with
correct flap; starfield and pipes look as before; no black icons, borders or AA text after 5b;
no stale regions after waking from sleep with 5a.

## Tests, red first

Harness facts: `lv_timer_handler_safe` pauses every timer, including the refresh timer, so
tests call the saver's `timer_cb` by hand after `lv_tick_inc` (`tests/unit/test_ui_carousel.cpp`)
and every render assertion uses `lv_refr_now`. `LV_EVENT_INVALIDATE_AREA` on the display
yields invalidated areas. Canvas pixels are read raw from `lv_canvas_get_draw_buf(c)->data`
(`lv_canvas_get_px` forces alpha). Access via friend `*TestAccess` in `tests/test_helpers/`.

| Phase | Must go red first | File |
|---|---|---|
| P0 | none (A/B only) | - |
| P3 | hide on start per type and restore on stop; pre-hidden screen stays hidden; type switch never unhides; a saver whose start bails leaves the screen visible; a modal shown during the saver is visible after stop; sleep overlay pair; a `DRAW_MAIN` counter on a screen child stays 0 across invalidate + `lv_refr_now`; 40 invisible stalled watchdog samples never give up | `test_screensaver.cpp`, `application/test_display_idle_poweroff.cpp`, `test_gcode_viewer_watchdog.cpp` |
| P1 | `MotionClock` wrap and gap clamp; `flight_pos_at` before, at and past delay, wrap; `flap_frame_at` sequence and offset; toaster period follows the refresh timer (test sets it to 20 first, so a hardcoded 33 goes red); one `lv_tick_inc(110)` tick vs eleven 10 ms ticks land sprites identically (150 vs 3x50 would pass on a BASIC host today); 128 px sprites above 800 px width; start -> stop -> start resets | new `test_screensaver_motion.cpp`, `test_screensaver.cpp` |
| P2 | starfield depth advance scales with dt; sim equality only after `REQUIRE`-ing no recycle in the window; seeded RNG determinism; pipes: 300 ms grows 3 steps per alive pipe, capped, remainder dropped at reset | `test_screensaver_motion.cpp` |
| P4a | canvas format is XRGB8888; X byte proven by mutating a fill to X=0 and watching the test fail; overlay issues no fill draw task (`LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS`); one pipes grow invalidates an area smaller than the canvas that contains every changed pixel (buffer diff); stop hides the canvas before its buffer is freed | `test_screensaver.cpp`, `test_screensaver_canvas_stride.cpp` |
| P4b | `StarfieldSim::step` deterministic per seed and its `DirtyRect` covers every changed pixel; skipped pickups accumulate dirty rects; worker start/stop leaves no extra thread (`live_thread_count.h`, `join_on_exit.h`); stop mid-step never blocks the main thread; destroying a running saver joins; cores < 4, EMBEDDED and env=0 take the inline path | new `test_screensaver_sim.cpp` (`[slow]` for thread tests) |
| P5 | any kept knob with observable logic gets a test (e.g. partial-upload rect math as a pure function) | per knob |

Gates after P4b and at the end: `make test-run` (not a tag filter),
`make test-tsan-one TEST="[screensaver]"` under `setarch "$(uname -m)" -R`, `make mutate-diff`
with the surviving mutation named in each commit body.

## Docs

`docs/devel/ENVIRONMENT_VARIABLES.md` (new env vars, next to `HELIX_SCREENSAVER_NOW`);
`docs/devel/GPU_ACCELERATION.md` (P5 results, alpha-trap section if 5b lands);
`docs/devel/THREADING.md` and `docs/devel/architecture/03-threading-lifetime.md` (both cite the
saver destructor as an exemplar); `docs/user/CONFIGURATION.md` (default screensaver type is
tier-dependent, not 1).

## Verification

### Build and deploy loop (thelio -> Pi 3B)

- Worktree via `scripts/setup-worktree.sh`; preflight `grep -c 'realpath lib/lvgl' mk/cross.mk`
  = 0 and `scripts/helix-claim jobs`.
- Build `make -C <wt> pi-docker NPROC_DOCKER_RUN=<jobs> > <scratch>/pi-<arm>.log 2>&1` (no
  pipe); confirm `EGL binary carries the GPU path` and `remote_control=yes` in
  `build/pi/bin/.build-features`. First pi build per tree is cold (~16 min); never two first
  builds at once (shared `lib/wpa_supplicant`/`lib/libnl` get `make clean`ed). A/B arms with
  different `lv_conf.h` use separate `BUILD_SUBDIR` trees.
- Keep each arm's `helix-screen` + `helix-screen-egl` under `<scratch>/arms/<name>/`.
- Deploy (never `make deploy-pi`, never glob `bin/*`): `systemctl stop helixscreen` and wait on
  `pidof`, `scp` the two binaries (`rsync ui_xml/` only if XML differs), `systemctl start`, then
  `--version` SHA and `ps` rung check before and after each run. `HELIX_LOG_LEVEL=debug`
  identical across arms.

### Measurement harness

Session `pi3b_measure.sh`: clean per-thread CPU window from `/proc/<pid>/task/*/stat`, then a
separate `strace -f -ttt -e trace=ioctl` window counting `DRM_IOCTL_MODE_PAGE_FLIP`, parsed
on thelio by `flips.py` (frames/s, interval percentiles, histogram). Workloads toasters,
starfield, pipes, idle home; three interleaved runs per arm; restore `screensaver_type` and
navigate home after each run. The load gate runs its probe and `stress-ng` in the same session,
started and killed by PID.
