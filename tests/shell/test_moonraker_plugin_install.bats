#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Guards for moonraker-plugin/install.sh and helix_print.py: the installer
# and plugin stay phase-tracking-free, and the shared helix_macros.cfg
# survives every uninstall path.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"

# Overridable so each guard can be proven to FIRE against a hand-broken copy
# of the script instead of the shipped one.
SCRIPT="${HELIX_PLUGIN_INSTALL_SH:-$REPO_ROOT/moonraker-plugin/install.sh}"
PLUGIN_PY="${HELIX_PLUGIN_PRINT_PY:-$REPO_ROOT/moonraker-plugin/helix_print.py}"

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

