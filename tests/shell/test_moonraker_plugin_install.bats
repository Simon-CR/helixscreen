#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Guards for moonraker-plugin/install.sh and helix_print.py: the installer
# and plugin stay phase-tracking-free, and every uninstall path
# (auto_uninstall and the interactive uninstall) strips any phase-tracking
# instrumentation a pre-1.1 HelixScreen wrote into PRINT_START, leaving the
# shared helix_macros.cfg and everything else in the config tree untouched.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"

# Overridable so each guard can be proven to FIRE against a hand-broken copy
# of the script instead of the shipped one.
SCRIPT="${HELIX_PLUGIN_INSTALL_SH:-$REPO_ROOT/moonraker-plugin/install.sh}"
PLUGIN_PY="${HELIX_PLUGIN_PRINT_PY:-$REPO_ROOT/moonraker-plugin/helix_print.py}"

# Extract the REAL marker constants and strip functions out of install.sh and
# run them against a directory. Sourcing the whole script would run the
# installer; re-typing the bodies here would let the test pass against logic
# that no longer matches what ships.
run_strip_phase_tracking() {
    local scan_dir="$1" funcs
    funcs="$(grep -E '^TRACKING_MARKER_(BEGIN|END)=' "$SCRIPT")
$(sed -n '/^strip_tracking_markers_from_file()/,/^}/p' "$SCRIPT")
$(sed -n '/^strip_phase_tracking_instrumentation()/,/^}/p' "$SCRIPT")"
    [ -n "$funcs" ] || { echo "strip functions not found in $SCRIPT"; return 2; }

    sh -c "info() { printf 'INFO: %s\n' \"\$1\"; }
warn() { printf 'WARN: %s\n' \"\$1\"; }
$funcs
strip_phase_tracking_instrumentation \"\$1\"" _ "$scan_dir"
}

@test "install.sh passes POSIX syntax check" {
    [ -f "$SCRIPT" ]
    run sh -n "$SCRIPT"
    [ "$status" -eq 0 ]
}

@test "uninstall preserves the shared helper macros" {
    # helix_macros.cfg carries HELIX_START_PRINT / HELIX_CLEAN_NOZZLE and
    # other helpers that keep working without the plugin; deleting it on
    # uninstall would break features the user never uninstalled.
    local auto_region="$BATS_TEST_TMPDIR/auto_uninstall.sh"
    local interactive_region="$BATS_TEST_TMPDIR/uninstall.sh"
    sed -n '/^auto_uninstall()/,/^}/p' "$SCRIPT" > "$auto_region"
    sed -n '/^uninstall()/,/^}/p' "$SCRIPT" > "$interactive_region"
    [ -s "$auto_region" ] || fail "auto_uninstall not found in $SCRIPT"
    [ -s "$interactive_region" ] || fail "uninstall not found in $SCRIPT"
    refute_grep 'rm.*helix_macros' "$auto_region"
    refute_grep 'rm.*helix_macros' "$interactive_region"
}

@test "every uninstall path strips phase-tracking instrumentation" {
    local auto_region="$BATS_TEST_TMPDIR/auto_uninstall.sh"
    local interactive_region="$BATS_TEST_TMPDIR/uninstall.sh"
    sed -n '/^auto_uninstall()/,/^}/p' "$SCRIPT" > "$auto_region"
    sed -n '/^uninstall()/,/^}/p' "$SCRIPT" > "$interactive_region"
    [ -s "$auto_region" ] || fail "auto_uninstall not found in $SCRIPT"
    [ -s "$interactive_region" ] || fail "uninstall not found in $SCRIPT"

    grep -q 'strip_phase_tracking_instrumentation' "$auto_region" \
        || fail "auto_uninstall does not strip phase-tracking instrumentation"
    grep -q 'strip_phase_tracking_instrumentation' "$interactive_region" \
        || fail "uninstall does not strip phase-tracking instrumentation"
}

@test "installer carries no phase-tracking flag" {
    [ -f "$SCRIPT" ]
    refute_grep 'with-phase-tracking' "$SCRIPT"
    refute_grep 'ENABLE_PHASE_TRACKING' "$SCRIPT"
}

@test "plugin exposes no phase-tracking endpoints" {
    [ -f "$PLUGIN_PY" ]
    refute_grep 'phase_tracking' "$PLUGIN_PY"
}

@test "strip removes a single phase-tracking block from a file" {
    local cfg="$BATS_TEST_TMPDIR/cfg"
    mkdir -p "$cfg"
    cat > "$cfg/printer.cfg" <<'EOF'
[gcode_macro PRINT_START]
gcode:
    G28
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HOMING
    # <<< /HELIX_TRACKING >>>
    M109 S{EXTRUDER_TEMP}
EOF

    run run_strip_phase_tracking "$cfg"
    [ "$status" -eq 0 ]

    refute_grep 'HELIX_TRACKING' "$cfg/printer.cfg"
    refute_grep 'HELIX_PHASE_HOMING' "$cfg/printer.cfg"
    grep -q 'G28' "$cfg/printer.cfg"
    grep -q 'M109' "$cfg/printer.cfg"
    ls "$cfg"/printer.cfg.bak.* > /dev/null 2>&1 || fail "no backup created"
}

@test "strip removes two phase-tracking blocks from one file" {
    local cfg="$BATS_TEST_TMPDIR/cfg"
    mkdir -p "$cfg"
    cat > "$cfg/printer.cfg" <<'EOF'
[gcode_macro PRINT_START]
gcode:
    G28
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HOMING
    # <<< /HELIX_TRACKING >>>
    M190 S{BED_TEMP}
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HEATING_BED
    # <<< /HELIX_TRACKING >>>
    M109 S{EXTRUDER_TEMP}
EOF

    run run_strip_phase_tracking "$cfg"
    [ "$status" -eq 0 ]

    refute_grep 'HELIX_TRACKING' "$cfg/printer.cfg"
    refute_grep 'HELIX_PHASE' "$cfg/printer.cfg"
    grep -q 'M190' "$cfg/printer.cfg"
    grep -q 'M109' "$cfg/printer.cfg"
}

@test "strip removes phase-tracking blocks split across two files" {
    local cfg="$BATS_TEST_TMPDIR/cfg"
    mkdir -p "$cfg"
    cat > "$cfg/printer.cfg" <<'EOF'
[gcode_macro PRINT_START]
gcode:
    G28
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HOMING
    # <<< /HELIX_TRACKING >>>
EOF
    cat > "$cfg/macros.cfg" <<'EOF'
[gcode_macro OTHER_MACRO]
gcode:
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_QGL
    # <<< /HELIX_TRACKING >>>
EOF

    run run_strip_phase_tracking "$cfg"
    [ "$status" -eq 0 ]

    refute_grep 'HELIX_TRACKING' "$cfg/printer.cfg"
    refute_grep 'HELIX_TRACKING' "$cfg/macros.cfg"
    ls "$cfg"/printer.cfg.bak.* > /dev/null 2>&1 || fail "no printer.cfg backup"
    ls "$cfg"/macros.cfg.bak.* > /dev/null 2>&1 || fail "no macros.cfg backup"
}

@test "strip leaves a file with no markers byte-identical and makes no backup" {
    local cfg="$BATS_TEST_TMPDIR/cfg"
    mkdir -p "$cfg"
    cat > "$cfg/printer.cfg" <<'EOF'
[gcode_macro PRINT_START]
gcode:
    G28
    M109 S{EXTRUDER_TEMP}
EOF
    cp "$cfg/printer.cfg" "$BATS_TEST_TMPDIR/before"

    run run_strip_phase_tracking "$cfg"
    [ "$status" -eq 0 ]

    cmp -s "$BATS_TEST_TMPDIR/before" "$cfg/printer.cfg" || fail "file changed with no markers present"
    [ "$(ls "$cfg" | wc -l)" -eq 1 ]
}

@test "strip leaves a file with an unmatched marker untouched and warns" {
    local cfg="$BATS_TEST_TMPDIR/cfg"
    mkdir -p "$cfg"
    cat > "$cfg/printer.cfg" <<'EOF'
[gcode_macro PRINT_START]
gcode:
    G28
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HOMING
EOF
    cp "$cfg/printer.cfg" "$BATS_TEST_TMPDIR/before"

    run run_strip_phase_tracking "$cfg"
    [ "$status" -eq 0 ]

    contains "WARN" "$output"
    contains "Unmatched" "$output"
    cmp -s "$BATS_TEST_TMPDIR/before" "$cfg/printer.cfg" || fail "unmatched file was edited"
    [ "$(ls "$cfg" | wc -l)" -eq 1 ]
}

@test "running the strip twice is harmless" {
    local cfg="$BATS_TEST_TMPDIR/cfg"
    mkdir -p "$cfg"
    cat > "$cfg/printer.cfg" <<'EOF'
[gcode_macro PRINT_START]
gcode:
    G28
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HOMING
    # <<< /HELIX_TRACKING >>>
    M109 S{EXTRUDER_TEMP}
EOF

    run run_strip_phase_tracking "$cfg"
    [ "$status" -eq 0 ]
    local after_first
    after_first="$(cat "$cfg/printer.cfg")"

    run run_strip_phase_tracking "$cfg"
    [ "$status" -eq 0 ]

    [ "$(cat "$cfg/printer.cfg")" = "$after_first" ]
    [ "$(ls "$cfg"/printer.cfg.bak.* | wc -l)" -eq 1 ]
}

@test "content outside the markers survives byte-identical" {
    local cfg="$BATS_TEST_TMPDIR/cfg"
    mkdir -p "$cfg"
    cat > "$cfg/printer.cfg" <<'EOF'
[gcode_macro PRINT_START]
gcode:
    G28
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HOMING
    # <<< /HELIX_TRACKING >>>
    M190 S{BED_TEMP}
      ; oddly indented comment
    M109 S{EXTRUDER_TEMP}

    BED_MESH_CALIBRATE
EOF
    cat > "$BATS_TEST_TMPDIR/expected" <<'EOF'
[gcode_macro PRINT_START]
gcode:
    G28
    M190 S{BED_TEMP}
      ; oddly indented comment
    M109 S{EXTRUDER_TEMP}

    BED_MESH_CALIBRATE
EOF

    run run_strip_phase_tracking "$cfg"
    [ "$status" -eq 0 ]

    diff "$BATS_TEST_TMPDIR/expected" "$cfg/printer.cfg"
}
