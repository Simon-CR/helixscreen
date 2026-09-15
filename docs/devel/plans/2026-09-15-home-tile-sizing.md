# Home Tile Sizing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the 18 centred-icon home tiles resize from half a cell to the whole grid, with icon and text scaling to match, and never offer a size at which the tile cannot show both its icon and its current value.

**Architecture:** The nozzle-temps pattern, one instance deeper. A pure function in `src/ui/panel_widgets/tile_layout.h` turns measured pixel widths into a verdict (icon step, value font, label rung, direction). Each tile owns a sizing helper that measures in `on_size_changed()`, calls the function, and publishes the verdict as **per-instance** subjects registered in the widget's constructor — before `lv_xml_create()` parses the component, because the parser permanently skips a binding whose subject does not exist. The manager passes each instance's subject names to its component as XML attrs. Every appearance is bound in XML off those subjects. Edit mode's resize clamp consults the live tile through a new `fits_at()` virtual, and the load path grows a saved span that is too small for this panel without persisting the growth.

**Tech Stack:** C++17, LVGL 9.5, helix-xml (our fork), Catch2 (amalgamated), nlohmann/json via `hv/json.hpp`, spdlog, pure Makefile.

**Spec:** `docs/devel/plans/2026-09-15-home-tile-sizing-design.md` (approved 2026-09-15, commit 8839ebd4d). Read it alongside this plan; this plan argues from it and records where the code contradicted it.

## Global Constraints

Every task's requirements implicitly include this section.

- spdlog only — never `printf`/`cout`/`LV_LOG_*`.
- SPDX header `// SPDX-License-Identifier: GPL-3.0-or-later` on new files.
- Design tokens, never raw values: `theme_manager_get_color("card_bg")`, `theme_manager_get_spacing("space_md")`, `#space_md` in XML.
- Declarative UI rules (`.claude/rules/declarative-ui.md`): no `lv_obj_add_event_cb`, no imperative visibility, no `lv_label_set_text`, no C++ styling. Measured layout and computed fonts are an explicit permanent exception — that is what the sizing helper is.
- Rule 7: a compound condition is `<subject_expr>` or an inline `cond=`, never a hand-written C++ derived subject.
- Widget lookup by `lv_obj_find_by_name()`, never `lv_obj_get_child()` with a fixed index.
- Comments describe the code as it is now. No SHAs, no "used to", no bug or mutation stories — in source, tests, gates and Makefiles alike.
- Conventional commits, subject plus roughly four lines. One line names the mutation `make mutate-diff` used.
- **Shared main tree.** Never `git add`; commit by pathspec: `git commit -m "..." -- <paths>`. For a brand-new file, `git add -N -- <file>` first. Never `--no-verify`. Never autostash.
- `make -j"$(scripts/helix-claim jobs)"` builds only the app; `make test` builds only tests. Inner loop is `make t F='[tag]'`. `make mutate-diff` and `make full-test-run` are completion gates, never dev loops.
- Work in `.worktrees/1559-tile-sizing` via `scripts/setup-worktree.sh feature/1559-tile-sizing`.
- Do not touch `active_spool_widget.cpp`'s `handle_clicked` / `commit_slot_edit` block — a peer session owns it.

---

## Owner rulings on the spec-versus-code conflicts

Thirteen places where the approved design and the current code disagree. These are the decisions; the tasks below are written to match.

**1. There is no `content_fits` virtual, and that name is already taken. RENAMED.** The design says "`PanelWidget` gains `virtual bool content_fits(...)`". Nothing of that name exists on `PanelWidget`. Worse, `content_fits` is already the name of an *external* test harness with opposite semantics — `tests/unit/test_widget_content_fits.cpp` measures overflow from outside the widget and asks nothing. Two things named `content_fits` that mean different things is a trap for every later reader. **The virtual is `fits_at(int width_px, int height_px) const`**, defaulting to `true`, slotted beside `supports_reuse()` (`include/panel_widget.h`), which is the existing default-true const-query shape.

**2. Per-instance subjects register in the CONSTRUCTOR, not `attach()`. MUST FIX.** The design says only "registered before the tile's XML is created". The obvious home, `attach()`, is too late: the manager's order is factory → `set_panel_id` → `set_config` → `get_component_name` → **`lv_xml_create`** → `attach` → `notify_size_changed`. A binding whose subject does not exist at parse time is skipped permanently, so subjects registered in `attach()` never bind. `PanelWidget::init_subjects()` looks like the right hook but is dead code — `PanelWidgetManager` never calls it and no widget overrides it. Register in the widget's constructor, which runs in the manager's first pass.

**3. `bypass` is not an unwrappable-widget tile. REGROUPED.** The design groups "temperature, filament and bypass" as having content the shared parts cannot wrap. `ui_xml/components/panel_widget_bypass.xml` uses none of `heater_icon` / `nozzle_icon` / `temp_display` / `filament_sensor_indicator`. It is two plain `<icon>` elements swapped on `ams_bypass_active`. The real grouping, which the tasks follow:

| Family | Count | Tiles |
|---|---|---|
| Single icon + label | 7 | shutdown, firmware_restart, led_controls, macros, motion, gcode_console, led |
| State-swapped icon set | 3 | lock (2 icons), network (6), bypass (2) |
| Icon + overlay badge | 2 | notifications, power_device |
| Icon + live value, nested row | 2 | fan, thermistor |
| Unwrappable composite | 4 | filament, temperature, bed_temperature, chamber_temperature |

**4. The seven action tiles are not interchangeable. `home_action_tile` takes a condition prop.** `shutdown` has no connection-state bind; `led_controls`, `macros`, `motion`, `gcode_console` each have one; `led` has two (`printer_connection_state` and `led_command_in_flight`). `header_bar.xml:169` proves a whole condition passes as a prop (`<bind_state_if cond="$action_button_disabled_cond" state="disabled"/>`), and `nozzle_icon.xml`'s `badge_subject=""` establishes that an empty prop installs no binding. So one `disabled_cond` prop covers zero, one or two reasons.

**5. `firmware_restart` changes appearance. CALLED OUT, NOT SMUGGLED.** It alone hardcodes `size="sm" variant="text"` instead of `size="#icon_size" variant="secondary"`, so it does not scale with breakpoint today. Collapsing it into `home_action_tile` normalises it. That is a deliberate, visible fix; it goes in the commit body and gets a screenshot in the visual sign-off.

**6. The icon ladder is NOT capped at 64px. The design's growth ceiling is reachable.** `nozzle_icon.xml`'s comment says "the icon ladder does NOT move with breakpoint — xs/sm/md/lg/xl are always 16/24/32/48/64". That comment is stale. `src/ui/ui_icon.cpp` maps the five rungs to *token names* (`icon_font_xs`…`icon_font_xl`), and `ui_xml/globals.xml` re-points every rung per tier: `icon_font_lg_xxlarge` is `mdi_icons_80`, `icon_font_xl_xxlarge` is `mdi_icons_96`, `icon_font_hero_xxlarge` is `mdi_icons_128`. Fix the stale comment in Task 1.

**7. `is_icon_font()` does not recognise faces above 64px. PRE-EXISTING DEFECT, FIXED HERE.** `src/ui/theme_manager.cpp#is_icon_font` lists `mdi_icons_{14,16,24,32,48,64}` only, while globals.xml already ships tokens resolving to 80, 96 and 128. At both call sites the predicate is a *guard*: the one at `theme_manager.cpp` "skip icons, they use the variant system" fails open for those faces, and the code then writes inline colours that the comment itself says would override variant styles and `HeatingIconAnimator`'s tint. This is not introduced by this change, but this change puts every home tile on those faces, so it is fixed in Task 1.

**8. `shared_font_style()` is `static` in `ui_text.cpp`. EXTRACT IT.** Both engine fixes need the per-face added-style cache, and a third hand-written copy is the duplication the house rules forbid. Promote it to a shared helper before either fix uses it.

**9. `temp_display` already hides the target by breakpoint. THE TILE WINS.** `hide_target_below_bp` plus `hide_target_when_off` drive `apply_target_visibility()` imperatively (`src/ui/ui_temp_display.cpp`). Design §2.5 puts the same decision in the tile's sizing helper. Two mechanisms for one rule drift silently. **Ruling: `temp_display` gains a `hide_target_cond` prop; when set it supersedes `hide_target_below_bp`, and the temperature tiles pass their per-instance verdict.** `hide_target_when_off` is orthogonal (it is about the heater being off, not about size) and stays.

**10. `test_grid_half_cell_placement.cpp` breaks, and swapping the id alone makes it VACUOUS.** It asserts `REQUIRE_FALSE(macros->supports_half_col)` and builds two placement scenarios on macros being whole-cell-only. `macros` is one of the 18, so three assertions fail: the `snap_step_for("macros") == {kCell, kCell}` pair check, `CHECK(it->col % kCell == 0)` in the auto-place case (it seats at `hole`), and the same check in the saved-origin case (col stays 7).

The obvious substitution is a trap. That scenario's free run is exactly `kCell` (2 tracks) wide — `tips` spans `hole = grid.cols - 5` and `clock` takes the last 3. `control_buttons` is 4 tracks wide, so it can never seat there under any step: it is honestly refused, `col` comes back -1, the `if (it->enabled && it->col >= 0)` guard skips every assertion, and the case goes **green while testing nothing**. `macros` worked as the fixed point precisely because its def is `kCell x kCell`, matching the hole.

**Ruling: widen the scenario, do not just rename the widget.** Use `control_buttons` (registry row `false, 4, 2, 4, 2, 4, 2`, both half flags false, confirmed not among the 18) *and* widen the hole to an odd-aligned 4-track run — `tips` spanning `grid.cols - 7`. Keep the existing `REQUIRE(hole % kCell != 0)` premise check and **add one asserting the run is wide enough for the def**, so a future reader cannot rebuild the vacuous version by shrinking it back. `temperature` is not available as the fixed point despite being one in three other test files: it is one of the 18.

The saved-origin case survives — a 2x2 saved span at col 7 still gets `floor_to(7, kCell) = 6`. But that span is now *below* `control_buttons`' minimum, which is exactly the condition Task 5's load-path grow targets, so that case starts exercising the grow. Say so in a comment there.

**11. `test_registry_span_bands.cpp` needs NO change. CHANGED FROM THE SPEC.** The design says it "drops the whole-cell minimum assertion for half-capable tiles". It already does — the assertion is inside `if (!def.supports_half_col)`. Setting the flags makes it skip those tiles automatically. But this is why both flags are load-bearing: a one-track minimum with `supports_half_row` still false would fail the row half.

**12. `on_size_changed()` receives pre-grid arithmetic, not laid-out geometry.** The manager passes `grid_track_extent(metrics.cell_w, metrics.gutter, colspan)`; the grid layout is activated after every widget is created and attached. The sizing helper must not read `lv_obj_get_width()` and expect truth. Edit mode's clamp must use the same arithmetic or clamp and render disagree.

**13. The exemplar re-decides only on target set/clear. The spec needs more.** `NozzleTempsWidget` re-decides when `had_target != (target > 0)` — two width classes. A reading growing `95°` → `100°` triggers nothing. The design requires the font and label to follow the live value, so the tile's predicate compares a **width-class key** (digit count of the formatted halves), not a boolean.

**14. The half-cell flags are a PLACEMENT-ENGINE change, not a registry-table edit. SCOPE RAISED.** `GridEditMode::snap_step_for` is the single source for the per-axis step, and it feeds `find_available`, `find_available_bottom`, `find_available_bottom_min`, `grow_once`, `clamp_to_grid`, `place_widget_from_catalog`, `layout_port.cpp#steps_for`, the edit-mode lattice, and the card-merge pass. Flipping 18 widgets from step 2 to step 1 makes odd-aligned gaps legal seats — which changes the free-run map for every *later* widget in the pass, shifts origins during `grow_once`, and halves the lattice pitch and the resize pixel minimum. Task 9 is split accordingly: the flags land with their own test pass, separately from the maxima.

**15. Origins persist unconditionally; only spans are guarded.** The write-back guard withholds a span unless `p.colspan == p.want_colspan`, but `entry_it->col` / `row` are written whenever they changed. A placement shift caused by the new step persists on the next save with no guard at all. Growth must therefore not nudge the origin, and Task 9a must re-derive the pinned layouts rather than assume they hold.

**16. The growth ceiling is platform-dependent, and literal face names are unsafe in a shared style.** `mk/fonts.mk` gives ad5m, ad5x, cc1, k1 and snapmaker-u1 a top face of 64px or below; only pi/pi32/x86/yocto link 96 and 128. A tile therefore grows to *its platform's* largest linked face, not to 128 everywhere. And the two spellings are not equivalent: `#icon_font_*` constants are guarded (`theme_manager.cpp` falls back to the `_large` rung and warns when a tier's face is missing), while a literal `mdi_icons_80` in `styles.xml` gets no such protection — `lv_xml_get_font` misses, warns through the LVGL log only, and silently substitutes `LV_FONT_DEFAULT` (`noto_sans_14`), which renders MDI codepoints as tofu. **Every tile style names `#icon_font_<rung>`, never a literal face.**

**17. `clamp_to_grid` already ceils spans up to the step; the real gap is the registry minimum.** A saved span below `effective_min_colspan()` that happens to be step-aligned passes through untouched today. Task 5's growth is specifically growth to the registry minimum or the smallest fitting size above it, not a re-implementation of what `ceil_to` already does.

---

## What could silently break

This codebase's characteristic failure is a lookup or predicate that fails with no crash, no log, and no test. Each is paired with the assertion that catches it.

| # | Silent failure | Assertion that catches it | Task |
|---|---|---|---|
| 1 | **A per-instance subject registered too late.** Registered in `attach()`, the component has already been parsed and every binding referencing it was skipped permanently. The tile renders at its default appearance forever, with no warning. | Test: create two `FanWidget` instances, drive them to different sizes, assert their icons resolve to *different* fonts. A skipped binding makes both read the XML default and the test fails. | 3, 6 |
| 2 | **A positional aggregate initialiser shifts `merges_into_card`.** In `s_widget_defs`, `merges_into_card` sits *after* `supports_half_col`/`supports_half_row`. A row that gains half flags without also spelling out `multi_instance` shifts the card flag and the tile loses (or gains) its background with no error. | The existing `[widget_def]` sweep plus an explicit check that every one of the 18 still has `merges_into_card == true`. | 7 |
| 3 | **`bind_flag_if_ne` silently does nothing.** The not-equal element is spelled `_not_eq`; `_ne` is the *expression* operator. `bind_flag_if_ne` does not exist and is not recognised — no error, the binding just never fires. | Bound-outcome test asserting the hidden flag actually toggles, not merely that the XML parsed. | 8, 9 |
| 4 | **`<subject_expr>` never registers.** It resolves operands when the *component is registered*; per-instance subjects do not exist until the manager constructs the widget. The derived subject is silently never created and every binding on it is inert. `cond=` resolves at *view creation* and is the only construct that works here. | Any bound-outcome test on a per-instance verdict; a `subject_expr` version reads the XML default. | 8 |
| 5 | **A grown span gets persisted.** `serialize_pages()` writes `item["colspan"] = entry.colspan` unconditionally on every save. If load-time growth writes into `entry.colspan`, the grown span is permanent and the widget is stranded at the larger size after rotating to a panel where it does not fit. | Test: load a config with a too-small span, populate, assert the widget renders grown **and** that the serialised JSON still carries the original span. | 5 |
| 6 | **The clamp and the renderer disagree about pixels.** Edit mode must convert span to pixels with `grid_track_extent(cell, gutter, span)` exactly as `populate_widgets` does. Any other arithmetic makes a size that clamps green render clipped. | Test driving `fits_at` through both paths at the same span and asserting identical pixel inputs. | 5 |
| 7 | **An icon above 64px loses its variant colour.** `is_icon_font()` does not list `mdi_icons_{80,96,128}`, so the theme-change guard fails open and inline colours overwrite the variant style. | Unit test asserting `is_icon_font` is true for all of 80, 96 and 128. | 1 |
| 8 | **A bound font loses to the local style.** `<icon>`'s face is written as a local style, which outranks every added style, so a bound `text_font` is silently ignored — exactly the #1614 shape. | `test_semantic_font_binding.cpp`'s pattern applied to `<icon>`: bound, plain and inline labels, asserting the resolved font pointer. | 2 |
| 9 | **A tile sized while idle draws a wider value later.** The fit query must budget the widest value the tile can ever show (`888°`, `100%`), or a size that fits at `95°` clips at `100°`. | Decision-function test asserting `fits` uses the worst-case value while the font/label choice follows the live one. | 4 |
| 10 | **`fits` is not monotonic and the clamp walks off.** The clamp assumes a tile that fits at a size fits at every larger size. If the decision function ever returns false for a strictly larger box, the walk-back loop never terminates or stops at the wrong size. | Sweep asserting monotonicity in both width and height across `kMeasured`. | 4 |
| 11 | **Four buttons gain resize handles they never had.** `is_selected_widget_resizable()` is `def->is_scalable()`, and `effective_max_*` collapses onto the authored span when max is 0. shutdown, lock, firmware_restart and led_controls have `max == min` today, so they cannot be resized at all. Raising their max makes handles appear — correct, but a visible change. | Named in the commit body; covered by the visual sign-off at three geometries. | 7 |
| 12 | **The re-decide fires on every temperature tick.** `lv_subject_set_int` notifies unconditionally and the observer runs per poll. Without a width-class guard the tile re-measures and republishes several times a second. | Test: drive a value change *within* one width class and assert the verdict subjects do not change. | 4, 9 |
| 13 | **A repointed test fixture goes green while testing nothing.** `test_grid_half_cell_placement.cpp`'s auto-place case guards its assertions behind `if (it->enabled && it->col >= 0)`. Substitute a widget too wide for the scenario's hole and it is refused, `col` is -1, every assertion is skipped, and the case passes having exercised no step rule at all. | The premise checks: keep `REQUIRE(hole % kCell != 0)` and **add** an assertion that the free run is wide enough for the substituted def, so the vacuous form cannot be rebuilt. | 9a |
| 14 | **A saved layout silently moves on next load.** Origins write back whenever they changed (`any_written`), with none of the guarding spans get. A widget reseated at an odd origin by the new step persists that position, so the user's dashboard rearranges itself after an upgrade. | Re-derive the pinned positions in `test_default_layout.cpp` rather than assuming they hold; assert the shipped layout is unchanged where it should be. | 9a |
| 15 | **A literal face name in a shared style renders tofu.** `lv_xml_get_font` misses, warns through the LVGL log (not spdlog), and falls back to `LV_FONT_DEFAULT` = `noto_sans_14`, which has no MDI glyphs. Baked once at style registration, so it is permanent and silent on that platform. | Grep gate: no `mdi_icons_` literal in `ui_xml/styles.xml`. Every tile style names `#icon_font_<rung>`. | 6 |
| 16 | **`<icon>`'s create path writes the face even with no `size=` attribute.** `ui_icon_xml_create` calls `apply_size(obj, IconSize::XL)` unconditionally, and `apply_size` also writes width/height as local styles. A fix that only covers the attribute path leaves the default path writing locally. | The bound-font test uses an `<icon>` with an explicit `size=` *and* one without; both must accept a bound face. | 2 |

---

## Reference data

**Grid constants** (`include/grid_layout.h`): `TRACKS_PER_CELL = 2`, `MIN_TRACKS = 4`, `MAX_TRACKS = 64`, `GRID_CELL[] = {34, 40, 40, 60, 60, 72, 96}` indexed by `UiBreakpoint`. `grid_track_extent(cell, gutter, span) = span * cell + (span - 1) * gutter`. `BAND_COLSPAN` in the registry is `GridLayout::MAX_TRACKS`.

**The icon ladder is per-tier.** `<icon size=>` accepts only the five names, and each resolves to a *token* that re-points per breakpoint (`ui_xml/globals.xml`, applied by `theme_manager.cpp#ui_theme_register_responsive_fonts`). Face px per tier:

| rung | micro | tiny | small | medium | large | xlarge | xxlarge |
|---|---|---|---|---|---|---|---|
| `icon_font_xs` | 16 | 16 | 16 | 16 | 16 | 24 | 32 |
| `icon_font_sm` | 16 | 24 | 24 | 24 | 24 | 32 | 48 |
| `icon_font_md` | 24 | 32 | 32 | 32 | 32 | 48 | 64 |
| `icon_font_lg` | 24 | 32 | 48 | 48 | 48 | 64 | 80 |
| `icon_font_xl` | 32 | 48 | 64 | 64 | 64 | 80 | 96 |
| `icon_font_hero` | 64 | 64 | 64 | 64 | 64 | 80 | 128 |

`#icon_size` resolves to the *string* `md` below medium and `lg` at medium and above, chosen so the glyph lands at 40-50% of the cell edge (`2 * GRID_CELL[tier]`). Micro is a deliberate exception at 35%: raising it clips seven widgets on both micro geometries.

**Platform reach** (`mk/fonts.mk` `FONT_TIERS`): ad5m, ad5x, cc1, k1 and snapmaker-u1 top out at 64px or below; k2 links 80; pi, pi32, x86 and yocto build `all` and link 96 and 128. A tile grows to *its platform's* largest linked face. `FONTS_CORE` always links 14/16/24/32/48/64, so those literals are safe everywhere and larger ones are not.

**The 18 tiles today**, spans in tracks (2 = one cell), from `src/ui/panel_widget_registry.cpp#s_widget_defs`:

| Tile | col,row | min c,r | max c,r | multi | half_col |
|---|---|---|---|---|---|
| shutdown, lock, firmware_restart, led_controls | 2,2 | 2,2 | 2,2 | — | yes |
| macros, motion, gcode_console | 2,2 | 2,2 | 2,2 | — | no |
| power_device | 2,2 | 2,2 | 2,2 | yes | no |
| network, led, filament, notifications | 2,2 | 2,2 | 4,2 | — | no |
| fan, thermistor | 2,2 | 2,2 | 4,2 | yes | yes |
| bypass | 2,2 | 2,2 | 4,2 | — | yes |
| temperature, bed_temperature, chamber_temperature | 2,2 | 2,2 | 4,4 | — | no |

All 18 have `supports_half_row = false`. Eight are non-scalable (`max == min`): shutdown, lock, power_device, firmware_restart, led_controls, macros, motion, gcode_console.

**Geometries**: `tests/unit/test_grid_square_cells.cpp#kMeasured` carries **16** entries, not the five the design quotes — including `ultrawide 1920x440` and `ultratall 440x1920`. The ultrawide row is where prestonbrown/helixscreen#1559 was originally reported, so the fit sweep covers all 16.

**Manager order** (`src/ui/panel_widget_manager.cpp#populate_widgets`): pass one constructs each instance (`def->factory(entry.id)` → `set_panel_id` → `set_config` → `get_component_name`); pass two creates and places (`lv_xml_create(container, name, attrs)` → `lv_obj_set_grid_cell` → `lv_obj_set_name` → `PANEL_WIDGET_TILE_FLAG` → `attach(widget, lv_scr_act())` → `notify_size_changed(...)`); then `lv_obj_set_grid_dsc_array` + `lv_obj_set_layout` + `lv_obj_update_layout` once, after the loop.

**Attrs contract**: `lv_xml_create(parent, name, attrs)` takes a flat `const char*` array of key/value pairs terminated by a single `nullptr`. Values are copied at parse time, so temporaries are safe. Working example at `src/ui/ui_fan_control_overlay.cpp` (`fan_status_card`).

**Binding rules** (`lib/helix-xml/docs/BINDINGS.md`): several bindings on one property **OR** together via `lv_xml_bind_compose` — each holds its own mask bit and the property applies while `held != 0`, in any notify order. Two bindings never AND; a conjunction is one `cond=` expression. The not-equal element is `_not_eq`. `invert="true"` flips an expression binding. Expression operators, loosest first: `or`, `and`, `eq`/`ne`/`lt`/`le`/`gt`/`ge`, `+ -`, `* / %`, `not`.

**Edit-mode resize** (`src/ui/grid_edit_mode.cpp`): `handle_resize_move` and `handle_resize_end` both run `compute_resize_result` → `clamp_span` → re-anchor for Top/Left. `at_limit = (pre_clamp != clamped)` on the dragged axis only; `valid = page_occupancy(id, Occupants::AllPlaced).can_place(...)`; the pixel preview gets `valid && !at_limit`, the snap preview gets `valid` alone. `detect_resize_edge` already caps inward reach at `min(band, width/3)` specifically so half-cell tiles stay grabbable.

---

## File Structure

**New**

| File | Responsibility |
|---|---|
| `src/ui/panel_widgets/tile_layout.h` | Pure decision function: measured widths in, verdict out. No LVGL. |
| `include/helix/ui/shared_font_style.h` | The per-face added-style cache, extracted from `ui_text.cpp` so all three call sites share one table. |
| `src/ui/panel_widgets/tile_sizing.h` / `.cpp` | `TileSizing`: per-instance subjects, measurement, publish. Owned by each tile. |
| `ui_xml/components/home_action_tile.xml` | The seven single-icon action tiles, with props. |
| `ui_xml/components/tile_icon.xml`, `tile_value.xml`, `tile_label.xml` | The bound parts. |
| `tests/unit/test_tile_layout.cpp` | Decision function, `[tile][layout]`. |
| `tests/unit/test_widget_size_tiles.cpp` | Bound outcomes, `[widget_size][tile]`. |
| `tests/unit/test_icon_font_binding.cpp` | Engine fix, `[xml][icon][font]`. |

**Modified**

`include/panel_widget.h` (the `fits_at` virtual), `src/ui/ui_icon.cpp`, `src/ui/ui_text.cpp`, `src/ui/ui_temp_display.cpp`, `src/ui/theme_manager.cpp` (`is_icon_font`), `include/panel_widget_registry.h` (flag comment), `src/ui/panel_widget_registry.cpp` (the 18 rows), `src/ui/panel_widget_manager.cpp` (attrs + load growth), `src/ui/grid_edit_mode.cpp` (fit clamp), `ui_xml/styles.xml`, the 19 tile XML files, `ui_xml/components/nozzle_icon.xml` (stale comment).

**Modified — tests**: `test_grid_layout.cpp` (the classification map — first red build), `test_grid_half_cell_placement.cpp` (fixed point repaired and the scenario widened), `test_widget_content_fits.cpp` (extra sizes swept), `test_grid_edit_mode.cpp` (the macros clamp pin), and the pinned-outcome set Task 9a re-derives: `test_panel_widget_grid_full.cpp`, `test_default_layout.cpp`, `test_panel_widget_manager_cell_px.cpp`, `test_widget_catalog_placement.cpp`, `test_home_edit_page_swipe.cpp`, `test_grid_edit_drag_path.cpp`, `test_layout_port.cpp`, `test_panel_widget_card_merge.cpp`. Per-widget registry pins that break if a maximum moves: `test_nozzle_temps_widget.cpp`, `test_tool_switcher_widget.cpp`, `test_camera_widget.cpp`, `test_panel_widget_temp_graph.cpp`, `test_ui_ams_mini_status.cpp`, `test_widget_size_clog_detection.cpp`.

**Modified — docs**: `docs/devel/LAYOUT_SYSTEM.md`, `docs/devel/PANEL_WIDGET_GUIDE.md`, `docs/devel/HOME_EDIT_MODE.md`, `docs/devel/UI_CONTRIBUTOR_GUIDE.md`, `docs/devel/LVGL9_XML_GUIDE.md`, `include/panel_widget_registry.h`, `docs/devel/plans/2026-08-12-home-widgets-design.md`. This file is deleted in the change that ships the work.

---

## Sequencing and the point of no return

Tasks 1 and 2 are engine fixes that change nothing visible — a bound font simply becomes *possible*. Task 3 adds machinery no tile uses yet. Tasks 4 and 5 add a decision function and a clamp that no registry entry can yet reach, because the spans are still capped.

**Task 9a is the first behavioural break, and it is wider than it looks.** The flags alone change which origins are legal, so widgets reseat and — because origins persist unconditionally — a saved dashboard can rearrange itself on the next load. Land it only once Tasks 7 and 8 have migrated the tiles, and expect to re-derive the pinned layouts rather than to find them green.

**Task 9b is the visible break for the user.** The moment the maxima open up, every one of the 18 becomes resizable and four buttons grow resize handles for the first time. Do not land 9b before the tiles can scale, or a user can drag a tile to a size whose content has not been taught to fill it.

**Task 9a is the point of no return for saved layouts** in one narrow sense: once a tile is seated at an odd origin and that position is written back, reverting the code leaves an origin the old whole-cell step will floor to the previous boundary. The load path handles it safely (origins floor, spans ceil), so this is recoverable, but it is the step after which a downgrade is no longer invisible.

Smoke-test intermediate tasks against a scratch config so a half-migrated dashboard never touches the real one:

```bash
export HELIX_CONFIG_DIR=/tmp/helix-config-1559 && mkdir -p "$HELIX_CONFIG_DIR"
```

---

### Task 1: Shared font-style cache, and an icon-font predicate that knows the big faces

**Files:**
- Create: `include/helix/ui/shared_font_style.h`
- Modify: `src/ui/ui_text.cpp` (delete the static copy, include the header)
- Modify: `src/ui/theme_manager.cpp` (`is_icon_font`)
- Modify: `ui_xml/components/nozzle_icon.xml` (stale ladder comment)
- Test: `tests/unit/test_icon_font_binding.cpp` (new, first case only)

**Interfaces:**
- Produces: `lv_style_t* helix::ui::shared_font_style(const lv_font_t* font)` — returns a process-lifetime style carrying exactly `text_font`, one per distinct face, created on first request.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_icon_font_binding.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_icon_font_binding.cpp
 * @brief An icon's face must be overridable by a bound style, and every
 *        compiled MDI face must be recognised as an icon font.
 *
 * theme_manager's is_icon_font() gates the "leave icons alone" branch of the
 * theme-change walk. A face it does not recognise falls through into code that
 * writes inline colours, which override the icon variant styles.
 */

#include "../test_fixtures.h"
#include "theme_manager.h"
#include "ui_fonts.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("every compiled MDI face is recognised as an icon font", "[icon][font][theme]") {
    // globals.xml resolves icon_font_lg_xxlarge to mdi_icons_80 and
    // icon_font_xl_xxlarge to mdi_icons_96, so a predicate that stops at 64
    // mis-classifies icons that ship today.
    struct Face { const char* name; const lv_font_t* font; };
    const Face faces[] = {
        {"mdi_icons_16", &mdi_icons_16}, {"mdi_icons_24", &mdi_icons_24},
        {"mdi_icons_32", &mdi_icons_32}, {"mdi_icons_48", &mdi_icons_48},
        {"mdi_icons_64", &mdi_icons_64}, {"mdi_icons_80", &mdi_icons_80},
    };
    for (const auto& f : faces) {
        INFO("face " << f.name);
        CHECK(theme_manager_is_icon_font(f.font));
    }
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
make test -j"$(scripts/helix-claim jobs)" && ./build/bin/helix-tests "[icon][font]"
```

Expected: compile error, `theme_manager_is_icon_font` not declared. `is_icon_font` is file-static today; the test forces it into the header.

- [ ] **Step 3: Extract the shared cache**

Create `include/helix/ui/shared_font_style.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

namespace helix::ui {

/// One shared ADDED style per font face, carrying only `text_font`.
///
/// A font written as a LOCAL style outranks every added style, and the XML
/// engine applies bind_style / bind_style_if_* through lv_obj_add_style — so a
/// widget whose face is local can never have it overridden from XML. Applying
/// the face as a shared added style at create time puts it below the nested
/// bind elements the parser applies afterwards, because same-precedence styles
/// resolve in addition order. An inline style_text_font attribute stays local
/// and keeps outranking both.
///
/// Append-only: one entry per compiled face any caller has ever asked for, so a
/// fixed table is enough and nothing here needs the heap.
lv_style_t* shared_font_style(const lv_font_t* font);

} // namespace helix::ui
```

With the body in a new `src/ui/shared_font_style.cpp`, moved verbatim from `ui_text.cpp`'s static function (32-entry table, `lv_style_init` + `lv_style_set_text_font` on first sight, `spdlog::critical` + `std::exit` when full).

- [ ] **Step 4: Point `ui_text.cpp` at it**

Delete the static `shared_font_style` from `src/ui/ui_text.cpp`, add `#include "helix/ui/shared_font_style.h"`, and change the one call site in `apply_semantic_font` to `helix::ui::shared_font_style(font)`.

- [ ] **Step 5: Widen and publish the icon-font predicate**

In `src/ui/theme_manager.cpp`, extend `is_icon_font` to the full set and expose it:

```cpp
bool theme_manager_is_icon_font(const lv_font_t* font) {
    if (!font)
        return false;
    return font == &mdi_icons_14 || font == &mdi_icons_16 || font == &mdi_icons_24 ||
           font == &mdi_icons_32 || font == &mdi_icons_48 || font == &mdi_icons_64 ||
           font == &mdi_icons_80;
}
```

Keep the file-static `is_icon_font` as a one-line forwarder so the two existing call sites are untouched. Declare `theme_manager_is_icon_font` in `include/theme_manager.h`. Guard `mdi_icons_96` / `mdi_icons_128` behind the same `LV_FONT_DECLARE` availability the xxlarge build uses, and add them to the list wherever they are declared — a face the current build did not link must not be named unconditionally.

- [ ] **Step 6: Fix the stale ladder comment**

In `ui_xml/components/nozzle_icon.xml`, replace "The icon ladder does NOT move with breakpoint - xs/sm/md/lg/xl are always 16/24/32/48/64" with a statement of what is true now:

```
The icon rungs are TOKENS, not fixed sizes: icon_font_xs..icon_font_xl each
resolve to a different face per breakpoint tier (globals.xml), so "lg" is 48px
at medium and 80px at xxlarge. A call site using a small icon must pass
badge_size and badge_font together, or the badge swallows the glyph.
```

- [ ] **Step 7: Run the test and watch it pass**

```bash
make t F='[icon][font]'
```

Expected: `All tests passed`.

- [ ] **Step 8: Prove the extraction changed no text behaviour**

```bash
./build/bin/helix-tests "[1614]"
```

Expected: PASS — `test_semantic_font_binding.cpp` still green, proving the moved cache behaves identically.

- [ ] **Step 9: Commit**

```bash
git add -N -- include/helix/ui/shared_font_style.h src/ui/shared_font_style.cpp tests/unit/test_icon_font_binding.cpp
git commit -m "refactor(ui): share one added-style-per-face cache, and recognise the large MDI faces

The per-face text_font style cache moves out of ui_text.cpp so the icon and
temp_display widgets can apply a face the same way, as an added style a bound
style can override rather than a local one that outranks it. is_icon_font()
also now recognises the 80px face, which globals.xml already resolves to at the
xxlarge tier; without it the theme walk treated those icons as text and wrote
inline colours over their variant styles." -- include/helix/ui/shared_font_style.h src/ui/shared_font_style.cpp src/ui/ui_text.cpp src/ui/theme_manager.cpp include/theme_manager.h ui_xml/components/nozzle_icon.xml tests/unit/test_icon_font_binding.cpp
```

---

### Task 2: A bound style can set an icon's face

**Files:**
- Modify: `src/ui/ui_icon.cpp` (`apply_size`)
- Test: `tests/unit/test_icon_font_binding.cpp` (append)

**Interfaces:**
- Consumes: `helix::ui::shared_font_style` from Task 1.
- Produces: no API change. `<icon>`'s face becomes overridable by `bind_style` / `bind_style_if_*`.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_icon_font_binding.cpp`, following `test_semantic_font_binding.cpp`:

```cpp
namespace {

constexpr const char* ICON_MODE_SUBJECT = "icon_font_test_mode";

/// One icon with a face bind, one without, one with an inline font attribute —
/// the three ways an icon's face can be decided.
constexpr const char* ICON_FIXTURE_XML = R"(<component>
  <styles>
    <style name="big_icon" text_font="#icon_font_xl"/>
  </styles>
  <view name="root" extends="lv_obj">
    <icon name="bound_icon" src="power" size="sm">
      <bind_style_if_eq name="big_icon" subject="icon_font_test_mode" ref_value="1"/>
    </icon>
    <icon name="plain_icon" src="power" size="sm"/>
  </view>
</component>)";

lv_subject_t& icon_mode_subject() {
    static lv_subject_t subject;
    static bool registered = false;
    if (!registered) {
        lv_subject_init_int(&subject, 0);
        lv_xml_register_subject(nullptr, ICON_MODE_SUBJECT, &subject);
        registered = true;
    }
    return subject;
}

} // namespace

TEST_CASE("a bound style overrides an icon's face", "[xml][icon][font]") {
    XMLTestFixture fixture;
    auto& mode = icon_mode_subject();

    const lv_font_t* sm = theme_manager_get_font("icon_font_sm");
    const lv_font_t* xl = theme_manager_get_font("icon_font_xl");
    REQUIRE(sm != nullptr);
    REQUIRE(xl != nullptr);
    // The two rungs must resolve to different faces or the assertions below
    // cannot tell the size attribute's face from the bound one.
    REQUIRE(sm != xl);

    REQUIRE(lv_xml_register_component_from_data("icon_font_fixture", ICON_FIXTURE_XML) ==
            LV_RESULT_OK);
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(fixture.test_screen(), "icon_font_fixture", nullptr));
    REQUIRE(root != nullptr);
    lv_obj_t* bound = lv_obj_find_by_name(root, "bound_icon");
    lv_obj_t* plain = lv_obj_find_by_name(root, "plain_icon");
    REQUIRE(bound != nullptr);
    REQUIRE(plain != nullptr);

    lv_subject_set_int(&mode, 1);
    CHECK(lv_obj_get_style_text_font(bound, LV_PART_MAIN) == xl);
    CHECK(lv_obj_get_style_text_font(plain, LV_PART_MAIN) == sm);

    lv_subject_set_int(&mode, 0);
    CHECK(lv_obj_get_style_text_font(bound, LV_PART_MAIN) == sm);
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
make t F='[xml][icon][font]'
```

Expected: FAIL — `bound` reports `sm` with the subject set to 1, because `apply_size` wrote the face as a local style.

- [ ] **Step 3: Apply the face as an added style**

In `src/ui/ui_icon.cpp`, `apply_size` currently resolves the rung to a font and writes it locally. Two paths reach it: the `size=` attribute, and `ui_icon_xml_create`, which calls `apply_size(obj, IconSize::XL)` **unconditionally** even when no `size=` is given. Both must stop writing locally, so the fix belongs in `apply_size` itself rather than at the attribute site. Note also that `apply_size` writes `lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT)` — width and height as local styles — so the same precedence problem would apply to any future attempt to bind an icon's geometry; leave those alone here.

Replace the local font write with the shared added style:

```cpp
    // The face rides a shared ADDED style so a style bound from XML can
    // override it; a local style would outrank every bound style. An inline
    // style_text_font attribute is still local and still wins, which is the
    // documented precedence.
    lv_obj_add_style(icon, helix::ui::shared_font_style(font), LV_PART_MAIN);
```

Add `#include "helix/ui/shared_font_style.h"`. Leave `set_size()`'s imperative path working: it must *replace* the previously added style rather than stack a second one, so remove the prior face style first with `lv_obj_remove_style(icon, helix::ui::shared_font_style(previous_font), LV_PART_MAIN)` when the widget already carries one. Track the current face in the icon's existing user-data struct.

- [ ] **Step 4: Run the test and watch it pass**

```bash
make t F='[xml][icon][font]'
```

Expected: `All tests passed`.

- [ ] **Step 5: Prove nothing else regressed**

```bash
./build/bin/helix-tests "[icon]" && ./build/bin/helix-tests "[1614]"
```

Expected: PASS both.

- [ ] **Step 6: Commit**

```bash
git commit -m "fix(ui): an icon's face can be set by a bound style

apply_size wrote the resolved MDI face as a local style, which outranks every
added style, so a style carrying text_font bound onto an <icon> was silently
ignored and the glyph kept the size attribute's face. The face now rides the
shared added style, below the bind elements the parser applies afterwards.
Inline style_text_font stays local and keeps outranking both." -- src/ui/ui_icon.cpp tests/unit/test_icon_font_binding.cpp
```

---

### Task 3: `fits_at` on `PanelWidget`, and a bindable target-hide on `temp_display`

**Files:**
- Modify: `include/panel_widget.h`
- Modify: `src/ui/ui_temp_display.cpp`, `ui_xml/temp_display.xml`
- Test: `tests/unit/test_widget_size_tiles.cpp` (new, first case)

**Interfaces:**
- Produces: `virtual bool PanelWidget::fits_at(int width_px, int height_px) const { return true; }`
- Produces: `temp_display` accepts `hide_target_cond="<expression>"`; when non-empty it supersedes `hide_target_below_bp`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_widget_size_tiles.cpp` with one case:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_tiles.cpp
 * @brief Centred-icon tiles resolve their icon face, value font, label
 *        visibility and direction from the per-instance subjects their sizing
 *        helper publishes.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "panel_widget.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("PanelWidget::fits_at defaults to true", "[widget_size][tile]") {
    // Every widget that has not opted in keeps its registry limits, so the
    // clamp must not narrow anything it was not taught about.
    for (const auto& def : get_all_widget_defs()) {
        INFO("widget " << def.id);
        if (!def.factory) {
            continue;
        }
        auto instance = def.factory(def.id);
        REQUIRE(instance != nullptr);
        CHECK(instance->fits_at(1, 1));
    }
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
make t F='[widget_size][tile]'
```

Expected: compile error, `fits_at` is not a member of `PanelWidget`.

- [ ] **Step 3: Add the virtual**

In `include/panel_widget.h`, immediately after `has_overlay_open()`:

```cpp
    /// Whether this widget can render its identifying content in a box of this
    /// size. Edit mode's resize clamp and the load path both ask before
    /// offering a size; a widget that returns false at a size is never given
    /// it. The default is true, so a widget that has not opted in keeps
    /// exactly its registry limits.
    ///
    /// Pixels are the grid's arithmetic extent for the span
    /// (grid_track_extent), not laid-out geometry — the same numbers
    /// notify_size_changed() carries, and available before any layout pass.
    ///
    /// Must be MONOTONIC: a widget that fits at a size fits at every larger
    /// size on both axes. The clamp walks sizes assuming that.
    virtual bool fits_at(int width_px, int height_px) const {
        (void)width_px;
        (void)height_px;
        return true;
    }
```

- [ ] **Step 4: Make `temp_display`'s font bindable — the design's second engine fix**

`ui_temp_display_create_cb` resolves one font via `get_font_for_size(size)` and writes it as a local style onto **four** labels: `current_label` and `unit_label` always, plus `separator_label` and `target_label` when `show_target="true"`. All four must ride `helix::ui::shared_font_style(font)` instead, or a bound style cannot retier any of them. Note this is the **text** ladder (`theme_manager_size_to_font_token` maps sm to `font_small`, md to `font_body`), not the `icon_font_*` ladder `<icon>` uses — the two are separate and the plan keeps them separate. Unlike `<icon>`, `temp_display` does not re-resolve on breakpoint change at all; that stays true here and is not this change's problem to fix.

- [ ] **Step 5: Add the bindable target-hide to `temp_display`**

In `src/ui/ui_temp_display.cpp`, parse a `hide_target_cond` attribute alongside `hide_target_below_bp`. When present, install an expression binding on the separator and target labels through the engine's `cond` path instead of computing visibility in `apply_target_visibility()`. Gate the imperative branch:

```cpp
    // A caller that supplies its own condition owns target visibility
    // entirely; the breakpoint rule is the fallback for callers that do not.
    // Two mechanisms writing one flag would fight, and the bound one wins by
    // construction because the imperative path stops running.
    if (data->has_target_cond) {
        return;
    }
```

Declare the prop in `ui_xml/temp_display.xml`'s `<api>` and forward it on the `<view>`.

- [ ] **Step 6: Run the test and watch it pass**

```bash
make t F='[widget_size][tile]'
```

Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add -N -- tests/unit/test_widget_size_tiles.cpp
git commit -m "feat(widgets): a panel widget can decline a size, and temp_display takes a target condition

PanelWidget::fits_at lets a widget answer whether it can draw its identifying
content in a given box, defaulting to true so nothing that has not opted in
changes. temp_display gains hide_target_cond, which supersedes the breakpoint
rule, so a caller that decides target visibility by measurement owns it outright
rather than racing the imperative path." -- include/panel_widget.h src/ui/ui_temp_display.cpp ui_xml/temp_display.xml tests/unit/test_widget_size_tiles.cpp
```

---

### Task 4: The decision function

**Files:**
- Create: `src/ui/panel_widgets/tile_layout.h`
- Test: `tests/unit/test_tile_layout.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace helix {
  struct TileContentWidths { int icon_px, value_px, value_current_px, label_px; };
  enum class TileDirection { Column = 0, Row = 1 };
  enum class TileLabelRung { None = 0, Label = 1 };
  struct TileStep { int icon_rung; int value_rung; };   // 0..4 indices into the ladders
  struct TileVerdict {
      int icon_rung = 2;
      int value_rung = 1;
      TileLabelRung label = TileLabelRung::Label;
      TileDirection direction = TileDirection::Column;
      bool show_target = true;
      bool fits = true;
  };
  TileVerdict decide_tile_layout(int avail_w, int avail_h, int gap_px,
                                 const TileContentWidths steps[5], const int line_h[5],
                                 bool has_value, bool labels_enabled);
  int tile_width_class(const char* value_text);
  }
  ```

The five-entry arrays are the five icon rungs (`xs`…`xl`), each carrying the content widths and line height at that rung — so the function never needs a font, only measurements. `tile_width_class` returns the digit count of the widest field, which is what the re-decide guard compares.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_tile_layout.cpp`, fixtures measured on an 800x480 panel, following `test_nozzle_temps_layout.cpp`'s plain-`TEST_CASE` shape (no fixture class — the function touches no LVGL):

```cpp
TEST_CASE("a one-cell tile keeps today's rung", "[tile][layout]") {
    // 110x108 is one cell at medium (55px track, 5px gutter, span 2).
    TileVerdict v = decide_tile_layout(110, 108, 4, kMediumSteps, kMediumLines,
                                       /*has_value=*/true, /*labels_enabled=*/true);
    CHECK(v.icon_rung == 3);                       // lg, which #icon_size resolves to
    CHECK(v.direction == TileDirection::Column);
    CHECK(v.fits);
}

TEST_CASE("a half-height strip becomes a row", "[tile][layout]") {
    // Half a cell tall cannot stack icon over value at any rung; the row form
    // fits the width, so direction is decided by fit rather than aspect.
    TileVerdict v = decide_tile_layout(110, 52, 4, kMediumSteps, kMediumLines, true, true);
    CHECK(v.direction == TileDirection::Row);
    CHECK(v.fits);
}

TEST_CASE("the target half goes before the label", "[tile][layout]") {
    TileVerdict v = decide_tile_layout(78, 108, 4, kMediumSteps, kMediumLines, true, true);
    CHECK_FALSE(v.show_target);
    CHECK(v.label == TileLabelRung::Label);
}

TEST_CASE("fits is monotonic in both axes", "[tile][layout]") {
    // The clamp walks sizes assuming a tile that fits stays fitting as it
    // grows; a non-monotonic answer makes that walk stop at the wrong size.
    for (int w = 20; w <= 400; w += 4) {
        for (int h = 20; h <= 400; h += 4) {
            const bool here = decide_tile_layout(w, h, 4, kMediumSteps, kMediumLines,
                                                 true, true).fits;
            if (!here) continue;
            INFO("fits at " << w << "x" << h << " must still fit larger");
            CHECK(decide_tile_layout(w + 4, h, 4, kMediumSteps, kMediumLines, true, true).fits);
            CHECK(decide_tile_layout(w, h + 4, 4, kMediumSteps, kMediumLines, true, true).fits);
        }
    }
}

TEST_CASE("fits budgets the widest value, not the current one", "[tile][layout]") {
    // A size accepted while the tile reads 95 must still fit at 888.
    TileVerdict v = decide_tile_layout(96, 108, 4, kMediumSteps, kMediumLines, true, true);
    if (v.fits) {
        CHECK(kMediumSteps[v.icon_rung].icon_px + kMediumSteps[v.icon_rung].value_px <= 96);
    }
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
make t F='[tile][layout]'
```

Expected: compile error, no such header.

- [ ] **Step 3: Write the decision function**

Create `src/ui/panel_widgets/tile_layout.h`: header-only, `inline`, namespace `helix`, zero LVGL includes. The ladder, in the order the spec fixes:

1. Choose direction by fit: Column when the icon stacked over the value fits the height at some rung; Row when only the side-by-side form fits the width.
2. Walk icon rungs from the largest whose glyph stays within the share of the short edge it holds at one cell, downward.
3. The value rung follows the icon rung, one step below the label's.
4. Degrade in order: target half, then label, then value rung, then icon rung, stopping at the 16px rung.
5. `fits` is true when the chosen rung draws the icon and the widest value. A tile with no value needs only the icon.
6. `labels_enabled == false` forces `TileLabelRung::None` — size can hide a label, never show one the setting hides.

- [ ] **Step 4: Run the tests and watch them pass**

```bash
make t F='[tile][layout]'
```

Expected: `All tests passed`.

- [ ] **Step 5: Prove the tests can fail**

```bash
make mutate-diff MUTATE_ARGS="--limit 5 --tests '[tile][layout]'"
```

Expected: every hunk dies. Record which mutation the commit body names.

- [ ] **Step 6: Commit**

```bash
git add -N -- src/ui/panel_widgets/tile_layout.h tests/unit/test_tile_layout.cpp
git commit -m "feat(widgets): a pure function decides a centred tile's rung and direction

decide_tile_layout takes measured content widths at each icon rung and returns
the rung, value font, label visibility and whether the tile stacks or sits in a
row. Direction follows fit rather than aspect ratio, so a half-height strip
becomes a row. fits budgets the widest value the tile can ever show, which is
what makes it monotonic and safe for the resize clamp to walk.

Mutation: inverting the height check in the direction choice makes the
half-height strip case report Column." -- src/ui/panel_widgets/tile_layout.h tests/unit/test_tile_layout.cpp
```

---

### Task 5: The resize clamp and load-time growth consult the tile

**Files:**
- Modify: `src/ui/grid_edit_mode.cpp`, `include/grid_edit_mode.h`
- Modify: `src/ui/panel_widget_manager.cpp`
- Test: `tests/unit/test_grid_edit_mode.cpp`, `tests/unit/test_panel_widget_load_growth.cpp` (new)

**Interfaces:**
- Consumes: `PanelWidget::fits_at` (Task 3).
- Produces: `std::pair<int,int> GridEditMode::clamp_span_to_fit(const std::string& id, PanelWidget* live, int c, int r, const helix::CellMetrics& m)` — a **member**, leaving the four existing tests on the static `clamp_span` untouched.

- [ ] **Step 1: Write the failing tests**

Two cases. The clamp refuses a non-fitting size and stops at the nearest fitting one; and a saved span too small for this panel is grown at populate **without** being persisted:

```cpp
TEST_CASE("a grown span is not written back", "[panel_widget][load][1559]") {
    // serialize_pages() writes colspan unconditionally on every save, so a
    // growth recorded on the config entry would outlive the panel that caused
    // it and strand the widget after a rotation.
    LoadGrowthFixture fixture;
    fixture.write_config(R"([{"id":"temperature","enabled":true,"col":0,"row":0,
                              "colspan":1,"rowspan":1}])");

    auto widgets = fixture.populate();
    CHECK(fixture.rendered_colspan("temperature") > 1);

    const auto saved = fixture.serialized();
    CHECK(saved["pages"][0]["widgets"][0]["colspan"] == 1);
}
```

- [ ] **Step 2: Run them and watch them fail**

```bash
make t F='[1559]'
```

Expected: FAIL — the span is honoured verbatim and the tile renders clipped.

- [ ] **Step 3: Add the fit-aware clamp**

In `src/ui/grid_edit_mode.cpp`, after the existing registry clamp in both `handle_resize_move` and `handle_resize_end`:

```cpp
    // Registry limits first, then the tile's own answer. Walk back from the
    // drag target toward the current size one snap step at a time and stop at
    // the nearest size the tile can draw; fits_at is monotonic, so the first
    // accepting size is the closest one.
    const auto [fit_c, fit_r] = clamp_span_to_fit(resize_id, live_widget(), result.colspan,
                                                  result.rowspan, m);
    const bool refused_for_fit = (fit_c != result.colspan || fit_r != result.rowspan);
    result.colspan = fit_c;
    result.rowspan = fit_r;
```

and fold the refusal into the existing limit signal, which already drives the red preview:

```cpp
    at_limit = at_limit || refused_for_fit;
```

`live_widget()` reads the selected object's user data, which each tile sets in its own `attach()`. Convert spans to pixels with `grid_track_extent(m.cell_w, m.gutter, span)` — the same arithmetic `populate_widgets` uses, so clamp and render cannot disagree.

- [ ] **Step 4: Grow a too-small saved span at populate**

In `populate_widgets`' first pass, after `set_config()` and before placement, widen the *placement* request only:

```cpp
    // A span saved against a different panel, language or theme can be smaller
    // than this tile can draw. Grow the request to the smallest size that fits.
    // The growth stays on the placement request: PanelWidgetEntry::colspan is
    // what serialize_pages() writes, and a size forced by this panel is not the
    // user's authored layout.
    if (slot.instance) {
        grow_to_fit(*slot.instance, metrics, slot.want_colspan, slot.want_rowspan);
    }
```

- [ ] **Step 5: Run the tests and watch them pass**

```bash
make t F='[1559]' && ./build/bin/helix-tests "[grid_edit]"
```

Expected: PASS, and the four existing `clamp_span` cases still green.

- [ ] **Step 6: Commit**

```bash
git add -N -- tests/unit/test_panel_widget_load_growth.cpp
git commit -m "feat(home): resize and load ask the tile whether a size fits

The resize clamp now walks back from the drag target to the nearest size the
selected tile can actually draw, and the preview turns red there exactly as it
does at a registry limit. At populate, a saved span too small for this panel is
grown to the smallest fitting size; the growth stays on the placement request so
serialize_pages never writes a size this panel forced.

Mutation: removing the grow_to_fit call leaves the load test rendering at the
saved one-track span." -- src/ui/grid_edit_mode.cpp include/grid_edit_mode.h src/ui/panel_widget_manager.cpp tests/unit/test_panel_widget_load_growth.cpp tests/unit/test_grid_edit_mode.cpp
```

---

### Task 6: Per-instance sizing helper and the shared styles

**Files:**
- Create: `src/ui/panel_widgets/tile_sizing.h` / `.cpp`
- Modify: `ui_xml/styles.xml`
- Create: `ui_xml/components/tile_icon.xml`, `tile_value.xml`, `tile_label.xml`
- Modify: `src/ui/panel_widget_manager.cpp` (pass attrs)
- Test: `tests/unit/test_widget_size_tiles.cpp` (append)

**Interfaces:**
- Produces: `helix::TileSizing`, constructed with the instance id, owning a `SubjectManager`. Registers `<id>_tile_icon`, `<id>_tile_value`, `<id>_tile_label`, `<id>_tile_row` via `UI_MANAGED_SUBJECT_INT`. `measure_and_publish(int w, int h)` and `bool fits(int w, int h) const`. `subject_names()` returns the attr pairs the manager passes to `lv_xml_create`.

- [ ] **Step 1: Write the failing test**

The two-instances case, which is exactly what a skipped binding fails:

```cpp
TEST_CASE("two fan tiles at different sizes resolve independently",
          "[widget_size][tile][1559]") {
    // Per-instance subjects are the whole point: a shared name would make the
    // second instance overwrite the first's verdict and both would agree.
    TileFixture fixture;
    PanelWidgetHarness<FanWidget> wide(fixture.test_screen(), "fan:0", fixture.printer_state());
    PanelWidgetHarness<FanWidget> narrow(fixture.test_screen(), "fan:1", fixture.printer_state());

    wide.resize(4, 4, 220, 216);
    narrow.resize(1, 1, 55, 54);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    const lv_font_t* wide_face =
        lv_obj_get_style_text_font(wide.child("fan_widget_icon"), LV_PART_MAIN);
    const lv_font_t* narrow_face =
        lv_obj_get_style_text_font(narrow.child("fan_widget_icon"), LV_PART_MAIN);
    CHECK(wide_face != narrow_face);
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
make t F='[widget_size][tile]'
```

Expected: FAIL — both icons resolve to the same face.

- [ ] **Step 3: Write `TileSizing`**

Subjects are registered in the **constructor** (ruling 2), via `UI_MANAGED_SUBJECT_INT` into the owned `SubjectManager`, so `deinit_all()` withdraws each name before freeing the subject. Names are `<instance_id>_tile_*`, unique per instance in the global XML scope.

- [ ] **Step 4: Add the shared styles**

In `ui_xml/styles.xml`, add the icon rung styles (`tile_icon_xs`…`tile_icon_xl`, each `text_font="#icon_font_<rung>"`), the text rung styles, and the two direction styles. Each direction style carries `layout="flex"` as well as `flex_flow` — a style with `flex_flow` and no `layout` leaves the container with no active layout while `lv_obj_get_style_flex_flow` still reports the flow:

```xml
    <style name="tile_column" layout="flex" flex_flow="column"/>
    <style name="tile_row" layout="flex" flex_flow="row"/>
```

- [ ] **Step 5: Pass the subject names as attrs**

In `populate_widgets`' second pass, build the flat attr array from the instance and hand it to `lv_xml_create` in place of `nullptr`. Values only need to outlive the call.

- [ ] **Step 6: Run the test and watch it pass, then commit**

```bash
make t F='[widget_size][tile]'
git add -N -- src/ui/panel_widgets/tile_sizing.h src/ui/panel_widgets/tile_sizing.cpp ui_xml/components/tile_icon.xml ui_xml/components/tile_value.xml ui_xml/components/tile_label.xml
git commit -m "feat(home): tiles publish their size verdict as per-instance subjects

Each tile owns a TileSizing that registers its own subject names in the widget
constructor, before the manager creates the XML that binds them, and withdraws
them on teardown. The manager passes those names to the component as attributes,
so two fan tiles at different sizes resolve independently instead of sharing one
type-global verdict.

Mutation: sharing one subject name across instances makes the two-fan test
resolve both icons to the same face." -- src/ui/panel_widgets/tile_sizing.h src/ui/panel_widgets/tile_sizing.cpp src/ui/panel_widget_manager.cpp ui_xml/styles.xml ui_xml/components/tile_icon.xml ui_xml/components/tile_value.xml ui_xml/components/tile_label.xml tests/unit/test_widget_size_tiles.cpp
```

---

### Task 7: `home_action_tile` and the seven single-icon tiles

**Files:**
- Create: `ui_xml/components/home_action_tile.xml`
- Modify: the seven tile XML files, `src/xml_registration.cpp`
- Test: `tests/unit/test_widget_size_tiles.cpp` (append)

- [ ] **Step 1: Write the failing test** — a one-cell appearance pin for each of the seven, asserting the icon face and label visibility match today's values on every shipping geometry. This is the regression net for the collapse.
- [ ] **Step 2: Run it against the unmigrated tiles and watch it PASS.** This pin must be green *before* the migration; it is the baseline the migration must not move. Record the values.
- [ ] **Step 3: Write `home_action_tile.xml`** with props `icon`, `icon_variant` (default `secondary`), `label`, `label_tag`, `callback`, `disabled_cond` (default empty), and the verdict subject props. Compose `tile_icon` and `tile_label`.
- [ ] **Step 4: Migrate the seven**, each becoming a `<home_action_tile>` with its props. `firmware_restart` normalises from `size="sm" variant="text"` to the shared rung (ruling 5).
- [ ] **Step 5: Re-run the pin.** Expected: six unchanged, `firmware_restart` changed. Update its pinned value and name the change in the commit.
- [ ] **Step 6: Regenerate translations** — `make translation-sync && make translations`, staging the YAMLs and `ui_xml/translations/*.xml`.
- [ ] **Step 7: Commit.**

---

### Task 8: The other eleven tiles

**Files:** the remaining tile XML, their widget sources, `tests/unit/test_widget_size_tiles.cpp`

One commit per family, each with its own appearance pin written first:

- [ ] **State-swapped sets** (lock, network, bypass): every icon in the set binds the rung, not just the first — six for network. Use `bind_style_if_eq name="styles.tile_icon_*"` on each.
- [ ] **Overlay-badge tiles** (notifications, power_device): the badge scales with the glyph, per `nozzle_icon.xml`'s rule that a disc can never be smaller than the line height of the font inside it.
- [ ] **Icon + value** (fan, thermistor): the nested row is the direction switch point, not the root. `thermistor` also migrates `panel_widget_thermistor_carousel.xml`, which is a bare container the C++ populates and needs no size binding of its own.
- [ ] **Unwrappable composites** (filament, temperature, bed_temperature, chamber_temperature): bind `styles.*` directly. The temperature three pass `hide_target_cond` from their verdict (ruling 9). `filament`'s three state labels are *not* gated by `show_widget_labels` and must stay that way.

---

### Task 9a: The half-cell flags, as a placement-engine change

This is not a registry-table edit. `snap_step_for` sits under auto-placement, growth, load clamping, legacy port quantisation, edit-mode snap, the drop lattice, catalog placement and card merge. Eighteen widgets moving from step 2 to step 1 changes which origins are legal, which in turn moves *other* widgets, and origins persist unconditionally (ruling 15).

**Files:** `src/ui/panel_widget_registry.cpp`, `include/panel_widget_registry.h`, `tests/unit/test_grid_layout.cpp`, `tests/unit/test_grid_half_cell_placement.cpp`, plus the pinned-outcome tests below.

- [ ] **Step 1: Repair the placement fixed point FIRST, and prove it still discriminates.** In `tests/unit/test_grid_half_cell_placement.cpp`, substitute `control_buttons` for `macros` *and* widen the scenario's free run to an odd-aligned 4-track gap (`tips` spanning `grid.cols - 7`). Keep `REQUIRE(hole % kCell != 0)` and add the guard against the vacuous form:

```cpp
    // control_buttons is 4 tracks wide. If the free run is narrower it can
    // never seat, the col >= 0 guard below skips every assertion, and this
    // case passes having exercised no step rule at all.
    const auto* cb = find_widget_def("control_buttons");
    REQUIRE(cb != nullptr);
    REQUIRE(run_width >= cb->colspan);
```

  Run it and watch it pass *before* touching the registry. Then confirm it can still go red: temporarily give `control_buttons` a half flag and check the case fails. A fixed point that cannot fail is not a fixed point.

- [ ] **Step 2: Update the classification map — this is the first red build.** `tests/unit/test_grid_layout.cpp`'s "PanelWidgetDef: half-cell capability is classified per widget" holds an exhaustive `std::map<std::string, std::pair<bool,bool>>` of every registry id to its flag pair, with `REQUIRE(it != expected.end())`. Every one of the 18 changes there. It fails before any placement test even runs.

- [ ] **Step 3: Set the flags.** Both half flags true on the 18. **Spell out `multi_instance` and both flags positionally on every row you touch** — `merges_into_card` follows the half flags, and a row that omits them shifts the card flag silently.

- [ ] **Step 4: Re-derive the pinned outcomes.** These pin packing, positions or spans and will shift. Each needs its expectations re-derived from the new behaviour, not merely re-recorded:

| File | What it pins |
|---|---|
| `test_panel_widget_grid_full.cpp` | GridFull eviction outcomes — odd remnants become seats, so expected failures become placements |
| `test_default_layout.cpp` | Exact per-tier positions and spans of the shipped layout |
| `test_panel_widget_manager_cell_px.cpp` | Writes `colspan 1, rowspan 1` for shutdown; the whole-cell row step currently ceils rowspan up to 2 |
| `test_widget_catalog_placement.cpp` | Catalog seat expectations |
| `test_home_edit_page_swipe.cpp`, `test_grid_edit_drag_path.cpp` | `temperature` as a whole-cell fixed point — it is one of the 18 |
| `test_layout_port.cpp` | `temperature` never straddles; doubling expectations |
| `test_panel_widget_card_merge.cpp` | Merge adjacency recomputed at the widget's step |

- [ ] **Step 5: Run the placement pass.** `[widget_def]`, `[half_cell]`, `[grid_edit]`, `[panel_widget]`, `[manager]`. Note the correction to the failure vocabulary: `GridFull` routes to `evict_for_full_grid` (col/row become -1, `enabled` untouched, toast only if it was on screen). It does **not** disable. The disable-and-toast route is for the other `PlacementFailure` reasons.

- [ ] **Step 6: Commit** the flags and the re-derived expectations together. A commit that changes flags without them leaves the suite red.

---

### Task 9b: Registry maxima

**Files:** `src/ui/panel_widget_registry.cpp`, `include/panel_widget_registry.h`, `tests/unit/test_grid_edit_mode.cpp`, `tests/unit/test_widget_content_fits.cpp`

- [ ] **Step 1: Set the spans.** Each of the 18 gets `min 1,1` and `max GridLayout::MAX_TRACKS` on both axes. `BAND_COLSPAN` is the established spelling for that maximum and is already used by print_status, temp_graph and tips.
- [ ] **Step 2: Fix the clamp pins.** `test_grid_edit_mode.cpp`'s "clamp_span non-scalable widget stays fixed" pins `macros` at `min == max == 2` and breaks. The tips case (`effective_max_colspan() == MAX_TRACKS`) holds.
- [ ] **Step 3: Update the flag comment** in `include/panel_widget_registry.h`. Both "the minimum on every axis is a whole cell, so this only ever ADDS sizes" and "leave it off for a centred fixed glyph with a short label" are now false.
- [ ] **Step 4: Run the sweeps.** `[span_bands]`, `[content_fits]`. `test_registry_span_bands.cpp`'s parity case needs no edit (ruling 11), but its **pinned band table** breaks when a minimum crosses a band on any of its eight geometries, and "no authored span exceeds the narrowest grid" breaks on minimum growth.
- [ ] **Step 5: Extend the content-fit sweep.** It measures at the authored *minimum*, so lowering minimums to one track re-sizes every measurement and can mint new clipping pairs, which hard-fail. Entries that stop clipping only warn. `kKnownClipping` is hand-edited in-file and may only shrink.
- [ ] **Step 6: Commit.**

---

### Task 10: Raised maxima for the self-laying-out widgets

**Files:** `src/ui/panel_widget_registry.cpp`, `tests/unit/test_widget_content_fits.cpp`

- [ ] Raise `max_colspan` / `max_rowspan` for tool_switcher, fan_stack, temp_stack, active_spool, humidity, width_sensor and favorite_macro **only as far as their existing layout code copes**, judged by the content-fit sweep at each new maximum. A widget whose sweep reddens keeps its current maximum and is noted as needing its own design. Registry-only: no edits inside `active_spool_widget.cpp`.

---

### Task 11: Documentation

**Files:** the six docs plus the superseded design lines

- [ ] `docs/devel/LAYOUT_SYSTEM.md` — replace "Every minimum is a whole cell" and the "leave it off for a centred fixed glyph" guidance; the four fixed-footprint buttons are no longer unresizable.
- [ ] `docs/devel/PANEL_WIDGET_GUIDE.md` — the tile sizing pattern: sizing helper, tile parts, `fits_at`, and that per-instance subjects register in the constructor.
- [ ] `docs/devel/HOME_EDIT_MODE.md` — the resize clamp consults the tile; add a bullet to "Extending edit mode".
- [ ] `docs/devel/UI_CONTRIBUTOR_GUIDE.md` and `docs/devel/LVGL9_XML_GUIDE.md` — an icon's face is bindable; the tile styles live in `styles.xml`.
- [ ] `docs/devel/plans/2026-08-12-home-widgets-design.md` — mark its no-new-half-cell-widget line superseded.
- [ ] Delete `docs/devel/plans/2026-09-15-home-tile-sizing-design.md` and this file in the final commit.

---

### Task 12: Completion gates and visual sign-off

- [ ] `make full-test-run`
- [ ] `make mutate-diff` across the whole diff, scoped per task where it is slow. Each commit body already names its mutation.
- [ ] `make check-tautology`, `make test-vacuous` — the cheap screens, before the oracle.
- [ ] Visual sign-off from a `ctl`-driven mock at 480x272, 800x480 and 1280x720, resizing tiles to half, one cell, 3x2 and the full grid:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv -s 800x480 \
  --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate home
```

Capture screenshots at each geometry and hand them to Preston with the specific question: does `firmware_restart` look right now that it scales with the others.

---

### Task 13: The larger icon face — measure, then decide

- [ ] Measure a stripped AD5M binary with and without the next face linked. Report the delta. Do not link it on a constrained target without that number. A face carrying only the glyphs tiles use is the cheaper alternative and needs a font-pipeline change; scope it separately if the delta is unacceptable.

---

## Self-review

**Spec coverage.** Design §1 → Tasks 1-3, 6. §2 (the ladder) → Task 4. §3.1 registry → Tasks 9a and 9b; §3.2 clamp and §3.3 two-dimensional fit → Task 5; §3.4 loading → Task 5; §3.5 handles → **retired**, `detect_resize_edge` already caps inward reach at `min(band, width/3)` and its comment names the half-cell case explicitly; §3.6 catalog unchanged → covered incidentally by 9a, since `place_widget_from_catalog` is a `snap_step_for` consumer. §4 larger face → Task 13. Testing §1-6 → Tasks 4, 6, 7, 8, 9a, 9b; §7 gates → Task 12. Documentation → Task 11.

**Where the design was wrong.** Seventeen rulings, but five matter most: `content_fits` does not exist and its name collides with an unrelated harness; per-instance subjects registered in `attach()` would never bind; `bypass` is not an unwrappable composite and the seven action tiles are not interchangeable; the span-bands assertion needs no edit because it is already conditional; and the half-cell flags are a placement-engine change rather than a table edit, which is the single largest scope correction in this plan.

**What the design does not mention at all**, each now carrying the assertion that catches it: the `is_icon_font` defect at faces above 64px; `serialize_pages` writing spans unconditionally; origins persisting with no guard whatsoever; the positional-initialiser trap around `merges_into_card`; `macros` breaking an existing placement test; `test_grid_layout.cpp`'s classification map being the first red build; and that substituting a too-wide widget into that placement scenario makes it pass while testing nothing.

**Known-vague, deliberately.** Task 9b's "re-derive the pinned outcomes" and Task 10's "as far as their layout code copes" are judgement calls that cannot be pinned before the numbers exist. Both name the gate that decides them (the content-fit sweep) rather than pretending to a threshold.

**Type consistency.** `fits_at` is spelled that way in Tasks 3, 5, 9a and the risk table. `TileVerdict` field names match between Task 4's interface block and Task 6's subject names. `clamp_span_to_fit` is a member throughout and the static `clamp_span` is never re-signatured, which keeps its four existing tests green — except the `macros` non-scalable pin, which Task 9b fixes because the *registry* changed under it, not its signature.
