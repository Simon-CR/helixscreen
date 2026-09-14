#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Guards for moonraker-plugin/install.sh and helix_print.py: the installer
# and plugin stay phase-tracking-free, the shared helix_macros.cfg survives
# every uninstall path, and a legacy helix_phase_tracking.cfg left by an old
# install is retired on the next --auto run.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"

# Overridable so each guard can be proven to FIRE against a hand-broken copy
# of the script instead of the shipped one.
SCRIPT="${HELIX_PLUGIN_INSTALL_SH:-$REPO_ROOT/moonraker-plugin/install.sh}"
PLUGIN_PY="${HELIX_PLUGIN_PRINT_PY:-$REPO_ROOT/moonraker-plugin/helix_print.py}"

# Extract the REAL cleanup_legacy_phase_cfg out of install.sh and run it
# against a config dir. Sourcing the whole script would run the installer;
# re-typing the body here would let the test pass against a cleanup that no
# longer matches what ships.
run_cleanup_legacy_phase_cfg() {
    local cfg_dir="$1" fn
    fn="$(sed -n '/^cleanup_legacy_phase_cfg()/,/^}/p' "$SCRIPT")"
    [ -n "$fn" ] || { echo "cleanup_legacy_phase_cfg not found in $SCRIPT"; return 2; }

    sh -c "info() { :; }
warn() { :; }
$fn
cleanup_legacy_phase_cfg \"\$1\"" _ "$cfg_dir"
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
    local region="$BATS_TEST_TMPDIR/auto_uninstall.sh"
    sed -n '/^auto_uninstall()/,/^}/p' "$SCRIPT" > "$region"
    [ -s "$region" ] || fail "auto_uninstall not found in $SCRIPT"
    refute_grep 'rm.*helix_macros' "$region"
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

@test "auto-install retires a legacy helix_phase_tracking.cfg and its include" {
    # Invariant first: with no legacy file, the cleanup touches nothing (a
    # plain --auto run edits moonraker.conf only).
    local clean_cfg="$BATS_TEST_TMPDIR/clean"
    mkdir -p "$clean_cfg"
    printf '%s\n' '[include helix_macros.cfg]' > "$clean_cfg/printer.cfg"

    run run_cleanup_legacy_phase_cfg "$clean_cfg"
    [ "$status" -eq 0 ]
    [ "$(ls -A "$clean_cfg")" = "printer.cfg" ]

    # With the legacy file present: include line out, both files backed up,
    # legacy file gone.
    local cfg="$BATS_TEST_TMPDIR/legacy" printer_backup legacy_backup
    mkdir -p "$cfg"
    {
        printf '%s\n' '[include helix_macros.cfg]'
        printf '%s\n' '[include helix_phase_tracking.cfg]'
    } > "$cfg/printer.cfg"
    printf '%s\n' '# legacy phase macros' > "$cfg/helix_phase_tracking.cfg"

    run run_cleanup_legacy_phase_cfg "$cfg"
    [ "$status" -eq 0 ]

    refute_grep 'include helix_phase_tracking' "$cfg/printer.cfg"
    grep -q '\[include helix_macros.cfg\]' "$cfg/printer.cfg"

    [ ! -e "$cfg/helix_phase_tracking.cfg" ]
    printer_backup="$(ls "$cfg"/printer.cfg.bak.* 2>/dev/null)" || fail "no printer.cfg backup"
    legacy_backup="$(ls "$cfg"/helix_phase_tracking.cfg.bak.* 2>/dev/null)" || fail "no legacy cfg backup"
    grep -q 'legacy phase macros' "$legacy_backup"
}
