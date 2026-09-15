# Home tile sizing: centred tiles resize from half a cell to the whole grid

Design for prestonbrown/helixscreen#1559, settled 2026-09-15. Phase 1 of two. The
implementation plan is written from this document once it is approved.

## Problem

Most home widgets are a centred icon with a short label, and their registry spans
either pin them to one cell or cap them at two. Where a larger span is allowed, the
content does not use it: the icon and text stay at the one-cell size in the middle of
an empty tile. Nothing smaller than one cell is possible.

| Today | Widgets |
|---|---|
| Locked at 1x1 | shutdown, lock, power_device, firmware_restart, led_controls, macros, motion, gcode_console |
| Can widen to 2x1 | network, led, fan, thermistor, bypass, filament, notifications |
| Can grow to 2x2 | temperature, bed_temperature, chamber_temperature |

Source: the span columns of `s_widget_defs` in `src/ui/panel_widget_registry.cpp`.

## Goal

For the 18 tiles above, resizing is the default behaviour:

- any size from **half a cell** on each axis to **the whole grid**
- content stays **centred**
- the icon and text **grow and shrink with the tile**, up to the largest icon face the
  device's build carries
- a size at which the tile cannot show what identifies it is **never offered**

Phase 2, not designed here: showing more information when a tile is larger than one
cell, decided one widget at a time.

## Scope

**In:** the 18 tiles listed above. The rule is the registry default, so a new simple
tile gets it without opting in.

**Out, keeping their own layout logic:** the widgets that already rearrange their content
by size (tool_switcher, fan_stack, temp_stack, active_spool, humidity, width_sensor,
favorite_macro) and `ams`. After the 18 tiles land, their maximum spans are raised as
far as their existing layout code copes, as the last step of phase 1.

**Out entirely:** the LED control and settings UI (prestonbrown/helixscreen#1130), which
gets its own design.

## Constraints the design has to respect

**Cell and track sizes.** A cell is two tracks (`include/grid_layout.h#GRID_CELL`). Measured
track size, from `tests/unit/test_grid_square_cells.cpp#kMeasured`:

| Panel | Tracks | Track px (w x h) |
|---|---|---|
| 480x272 | 12x8 | 34 x 31 |
| 800x480 | 12x8 | 55 x 54 |
| 1024x600 | 16x10 | 51 x 53 |
| 1280x720 | 16x10 | 63 x 63 |
| 1920x440 | 46x10 | 36 x 39 |

A half-by-half tile on the smallest panel is 34x31 px.

**Icon faces are fixed sizes.** An `<icon>` is a label drawn from a pre-rendered MDI font.
`scripts/regen_mdi_fonts.sh` builds 14, 16, 24, 32, 48, 64, 80, 96 and 128 px faces, and
`mk/fonts.mk` links a subset per screen class. LVGL's software renderer does not scale a
glyph (`lv_conf.h`). A tile's icon steps between the faces its build carries:

| Class | Platforms | Tile icon at 1x1 today (`#icon_size`) | Largest face linked |
|---|---|---|---|
| micro / tiny | cc1, snapmaker-u1 | 24 / 32 | 32 / 48 |
| small / medium / large | ad5m, ad5x, k1, k2 | 32 / 48 / 48 | 64 |
| xlarge | k2 | 64 | 80 |
| xxlarge | pi, x86 | 80 | 128 |

**The engine cannot change an icon's size after creation.** `src/ui/ui_icon.cpp#ui_icon_xml_apply`
writes the font as a local style, and a local style outranks any bound style.
`src/ui/ui_text.cpp#apply_semantic_font` solves the same problem for `text_*` labels with a
shared added style per face (#1614). `src/ui/ui_temp_display.cpp` picks its font once at
create time.

**A `<styles>` block is file-local.** A style several files bind lives once in
`ui_xml/styles.xml` and is borrowed as `styles.<name>`; every `bind_style*` handler in
`lib/helix-xml/src/xml/parsers/lv_xml_obj_parser.c` resolves the dotted name. Every hot-reload
save of `styles.xml` retains one scope, so the library stays small.

**Two documented decisions are reversed.** `docs/devel/LAYOUT_SYSTEM.md` states "Every minimum
is a whole cell", and the half-cell flag comment on `PanelWidgetDef` in
`include/panel_widget_registry.h` tells authors to leave it off for a centred glyph tile.
`docs/devel/plans/2026-08-12-home-widgets-design.md` says "No new widget sets
`supports_half_col`". This design overrides all three.

## Design

### 1. Architecture

The shape is the nozzle-temps pattern (`docs/devel/PANEL_WIDGET_GUIDE.md` § "Publish the
verdict as subjects", exemplar `src/ui/panel_widgets/nozzle_layout.h#decide_nozzle_layout`):
C++ measures, a pure function decides, subjects publish the verdict, XML applies it.

**Pure decision function.** `src/ui/panel_widgets/tile_layout.h`, no LVGL calls.

- In: usable width and height, and the measured pixel size of the tile's content at each
  icon face and text size step (icon glyph, value string, label string).
- Out: icon step, value font, label shown and its font, and direction: column (icon above
  text) or row (icon beside text).
- Also answers `fits`: whether any combination draws the icon and the current value.
  `fits` measures the widest value the tile can show (`888°`, `100%`), so a size that fits
  cannot stop fitting when the reading grows. The font and label choice follows the live
  value, as nozzle temps does, because those can change without invalidating a saved layout.

**One sizing helper per tile instance.** Each of the 18 tiles owns one. From
`on_size_changed()` it measures, calls the decision function and publishes the result as
subjects owned by that instance, because fan, thermistor and power_device are
multi-instance and two tiles of one type can differ in size. The subjects are registered
before the tile's XML is created, since the parser skips a binding whose subject does not
exist, and the manager passes their names to the component as a prop. The instance's
`SubjectManager` withdraws the names on teardown.

**Shared styles and parts.**

- Every size style lives once in `ui_xml/styles.xml`: icon steps (`styles.tile_icon_*`),
  text steps (`styles.tile_text_*`), and direction (`styles.tile_column`, `styles.tile_row`,
  each carrying `layout="flex"` as well as `flex_flow`).
- The bindings live once in part components: `tile_icon`, `tile_value`, `tile_label`.
- The single-icon action tiles (led, shutdown, led_controls, macros, motion, gcode_console,
  firmware_restart) collapse into one `home_action_tile` component with icon, label and
  callback props.
- lock and network swap between several state icons, and compose the parts.
- The temperature, filament and bypass tiles have content the parts cannot wrap
  (`heater_icon`, `nozzle_icon`, `temp_display`, `filament_sensor_indicator`); they bind the
  same `styles.*` names directly.

**Engine fixes.**

- `<icon>` applies its size through a shared added style per face, so a bound
  `text_font` wins. Same approach as `apply_semantic_font`.
- `temp_display` gets the same treatment for its labels.

**Fit query.** `PanelWidget` gains `virtual bool content_fits(int width_px, int height_px) const`,
returning true by default so every other widget keeps its registry limits. The 18 tiles answer
it through their sizing helper.

### 2. How a tile grows and shrinks

1. **At its default one-cell span a tile looks exactly as it does today**: same icon face,
   same fonts, on every shipping panel.
2. **The icon leads.** It takes the largest face whose glyph stays at the share of the tile's
   short edge it has at one cell today (the `#icon_size` note in `ui_xml/globals.xml` puts that
   at 40-50% of a cell), capped by the largest face linked for the class.
3. **Text grows in step with the icon.** Each icon step maps to a text step. The value stays
   one step above the label, as the fan tile draws a body-size value over a tiny label.
4. **Direction is decided by fit, not by aspect ratio.** Column whenever the stack fits the
   height; row when only a row fits the width. Half-height strips and wide short tiles
   become rows.
5. **Shrink order:** the target half of a value (`/ 60°`) goes first, then the label, then the
   text drops a step, then the icon drops a step, down to the 16 px face. A size at which the
   icon and the current value no longer both fit is not offered. Action tiles, which have no
   value, need only the icon.
6. **Content is centred** on both axes in either direction.
7. **The "show widget labels" setting still gates labels.** Size can hide a label; it never
   shows one the setting hides.

### 3. Registry, edit mode and loading

1. **Registry.** The 18 tiles get a one-track minimum and `GridLayout::MAX_TRACKS` maximum on
   both axes, with both half-cell flags set. Half-cell lattice dots and half-step snapping
   follow those flags already (`src/ui/grid_edit_mode.cpp#GridEditMode::snap_step_for`).
2. **The resize clamp asks the tile.** `GridEditMode::clamp_span` takes the live instance from
   the selected widget's user data. After the registry limits it walks from the drag target
   back toward the tile's current size, one snap step at a time, to the nearest size where
   `content_fits(w, h)` holds. The preview turns red at that limit, as it does at the maximum.
3. **Fit is two-dimensional.** A half-width tile may fit only when it is a full cell tall
   (icon above value), so narrowing stops at the narrowest width that fits the current
   height. The clamp relies on `fits` being monotonic: a tile that fits at a size fits at every
   larger size.
4. **Loading.** At populate, a saved span smaller than what fits on this panel, language and
   theme is grown to the smallest size that fits. The grown span is never written back,
   the same rule `src/ui/panel_widget_manager.cpp` applies to a span it cuts down ("Never
   persist a span that was cut down"). Collisions
   caused by growing go through the existing retry for tiles that do not fit at their
   authored span.
5. **Resize handles on half-cell tiles.** `GridEditMode::detect_resize_edge` already caps a
   handle's inward reach at a third of the tile's width. Verify it by driving a resize with
   `ctl` on a 480x272 mock before changing anything; if it is too fiddly, that becomes a
   separate design question.
6. **Adding a tile from the catalog is unchanged.** New tiles still place at one cell.

### 4. Larger icon face

Growing past the largest linked face means linking one more face per class, which costs
flash on constrained targets. The 80 px face's generated source is 3.0 MB for 253 glyphs;
no compiled figure exists. This is a separate step after phase 1, decided on a measured AD5M
binary size. A face carrying only the glyphs tiles use is the cheaper alternative, and needs a
font-pipeline change.

## Testing

Every test must fail if the behaviour it names is removed.

1. **Decision function**, `tests/unit/test_tile_layout.cpp`, `[tile][layout]`: every step change
   and direction switch at its exact pixel boundary on measured fixtures for each shipping
   geometry; the shrink order; `fits` on the widest value while fonts follow the live value;
   a sweep proving `fits` is monotonic in width and height.
2. **One-cell appearance pin**: each of the 18 tiles at its default span resolves to today's
   icon face and fonts on every shipping geometry.
3. **Bound outcome**, `[widget_size][tile]`, through the panel widget size harness: a real tile
   sized to a span resolves the expected icon font px, label hidden flag and container
   `LV_STYLE_LAYOUT`; two fan tiles at different sizes resolve independently.
4. **Engine fixes**: a bound font overrides `<icon>`'s face, following
   `tests/unit/test_semantic_font_binding.cpp`; the same for `temp_display`.
5. **Edit mode and loading**: the clamp rejects a non-fitting size and stops at the nearest
   fitting one; a too-small saved span grows at populate and the settings file is unchanged.
6. **Existing sweeps**: `tests/unit/test_widget_content_fits.cpp` also drives each tile at its
   smallest accepted size and at a large span, with the known-clipping baseline allowed only
   to shrink; `tests/unit/test_registry_span_bands.cpp` drops the whole-cell minimum assertion
   for half-capable tiles.
7. **Completion gates**: `make mutate-diff` with the mutation named in the commit body; a
   `ctl`-driven mock at 480x272, 800x480 and 1280x720 resizing tiles to half, one cell, 3x2
   and the full grid, with screenshots for visual sign-off.

## Documentation updated in the same change

- `docs/devel/LAYOUT_SYSTEM.md`: half-cell minimums for centred tiles and the fit-driven clamp
  replace "Every minimum is a whole cell".
- The half-cell flag comment on `PanelWidgetDef` in `include/panel_widget_registry.h`.
- `docs/devel/PANEL_WIDGET_GUIDE.md`: the tile sizing pattern (sizing helper, tile parts,
  `content_fits`).
- `docs/devel/HOME_EDIT_MODE.md`: the resize clamp consults the tile.
- `docs/devel/UI_CONTRIBUTOR_GUIDE.md` and `docs/devel/LVGL9_XML_GUIDE.md`: icon size is
  bindable; the tile styles in `styles.xml`.
- `docs/devel/plans/2026-08-12-home-widgets-design.md`: its no-new-half-cell-widget line is
  superseded.
- This file is deleted in the change that ships phase 1.

## Build order

1. Engine: bindable `<icon>` face, bindable `temp_display` font.
2. `tile_layout.h` decision function and its tests.
3. `content_fits`, the edit-mode clamp and load-time growth.
4. Tile styles in `styles.xml`, the part components, `home_action_tile`, and the seven
   single-icon action tiles migrated.
5. lock, network, fan, thermistor, power_device, notifications, the three temperature tiles,
   filament and bypass migrated.
6. Registry spans for the 18, span band and content-fit sweeps updated.
7. Raised maximum spans for the widgets with their own layout, where their code copes.
8. Documentation.
9. Larger icon face: measure, then decide.
