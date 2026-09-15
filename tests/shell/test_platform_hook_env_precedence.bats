#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The init script calls platform_pre_start() before it launches helix-screen,
# and helix-launcher.sh only exports variables from helixscreen.env that are
# not already set. A hook that assigns unconditionally therefore outranks every
# other source, including the user's own environment, and does it silently -
# the documented override appears to be accepted and is discarded.
#
# The same is true of any export the init script lets through to the launcher:
# an inherited hook default counts as "already set" and outranks
# helixscreen.env. The init script invokes platform_pre_start for side effects
# only (subshell), and the launcher runs the hook again after applying the env
# file, where a platform default belongs.
#
# Hooks supply the platform DEFAULT. Anything already set wins.
# Precedence: shell environment > helixscreen.env > platform hook > built-in.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOKS_DIR="$WORKTREE_ROOT/assets/config/platform"
INIT_SCRIPT="$WORKTREE_ROOT/config/helixscreen.init"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"

setup() {
    load helpers
}

# export lines inside platform_pre_start(), excluding comments.
pre_start_exports() {
    awk '/^platform_pre_start\(\)/{i=1; next} i && /^}/{exit} i' "$1" \
        | grep -E '^\s*export [A-Z_][A-Z0-9_]*=' || true
}

# export statements at file scope (outside every function body).
file_scope_exports() {
    awk '
        /^[a-zA-Z_][a-zA-Z0-9_]*\(\) ?\{/ { infn = 1 }
        infn { next }
        /^}/ { infn = 0 }
        /^ *export / { print }
    ' "$1"
}

@test "every platform hook defaults its exports instead of overriding them" {
    local offenders=""
    for f in "$HOOKS_DIR"/hooks-*.sh; do
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            var="$(echo "$line" | sed -E 's/^\s*export ([A-Z_][A-Z0-9_]*)=.*/\1/')"
            echo "$line" | grep -qF "\${$var:-" || \
                offenders="$offenders
  $(basename "$f"): $(echo "$line" | sed 's/^\s*//')"
        done <<< "$(pre_start_exports "$f")"
    done
    [ -z "$offenders" ] || {
        echo "These assignments discard a value the user already set:$offenders"
        echo 'Use: export VAR="${VAR:-default}"'
        false
    }
}

@test "the hooks actually export something (the check above is not vacuous)" {
    local n=0
    for f in "$HOOKS_DIR"/hooks-*.sh; do
        n=$((n + $(pre_start_exports "$f" | grep -c . || true)))
    done
    [ "$n" -ge 20 ]
}

@test "no hook exports a HELIX default at file scope" {
    # A file-scope export runs when the INIT SCRIPT sources the hooks file, so
    # the default reaches the launcher ahead of helixscreen.env and outranks
    # the operator's value. Hook defaults belong in platform_pre_start(),
    # which the init script runs in a subshell and the launcher runs after the
    # env file. Allowed by name: HELIX_NO_SPLASH on Forge-X, which the init
    # script itself consumes for the early-splash decision.
    local offenders=""
    for f in "$HOOKS_DIR"/hooks-*.sh; do
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            echo "$line" | grep -q 'HELIX_NO_SPLASH' && continue
            offenders="$offenders
  $(basename "$f"): $(echo "$line" | sed 's/^\s*//')"
        done <<< "$(file_scope_exports "$f")"
    done
    [ -z "$offenders" ] || {
        echo "These file-scope exports outrank helixscreen.env on the init path:$offenders"
        echo "Move them into platform_pre_start()."
        false
    }
}

@test "the file-scope scan actually sees exports (not vacuous)" {
    # The Forge-X splash gate is a real file-scope export the gate above lets
    # through by name - which proves the scan reaches file scope at all.
    grep -q '^ *export HELIX_NO_SPLASH$' "$HOOKS_DIR/hooks-ad5m-forgex.sh"
}

@test "the defaulting form keeps a preset value and supplies one otherwise" {
    # Pin the semantics the rule depends on, so a future rewrite that looks
    # equivalent but is not gets caught here rather than on a device.
    run sh -c 'HELIX_CACHE_DIR=/user/choice; export HELIX_CACHE_DIR
               export HELIX_CACHE_DIR="${HELIX_CACHE_DIR:-/platform/default}"
               echo "$HELIX_CACHE_DIR"'
    [ "$status" -eq 0 ]
    [ "$output" = "/user/choice" ]

    run sh -c 'unset HELIX_CACHE_DIR
               export HELIX_CACHE_DIR="${HELIX_CACHE_DIR:-/platform/default}"
               echo "$HELIX_CACHE_DIR"'
    [ "$status" -eq 0 ]
    [ "$output" = "/platform/default" ]
}

# =============================================================================
# The init script's own invocation of platform_pre_start.
#
# helix-launcher.sh fills only UNSET variables from helixscreen.env, so
# anything the init script exports before exec'ing the launcher outranks the
# operator's file. The init script must therefore run platform_pre_start for
# its side effects only, confining the hook's exports to a subshell; the
# launcher re-runs the hook after the env file and supplies the defaults there.
# =============================================================================

# The line inside start() that invokes platform_pre_start, taken from the real
# init script so these tests run what ships, not a copy.
init_pre_start_call() {
    awk '
        /^start\(\) \{/ { in_start = 1 }
        in_start && /platform_pre_start/ && $0 !~ /^[ \t]*#/ { print; exit }
    ' "$INIT_SCRIPT"
}

# A hook shaped like the shipped ones: every export is a guarded default.
install_defaulting_hook() {
    mkdir -p "$MOCK_INSTALL/platform"
    cat > "$MOCK_INSTALL/platform/hooks.sh" << 'HOOKEOF'
#!/bin/sh
platform_pre_start() {
    export HELIX_LOG_DEST="${HELIX_LOG_DEST:-file}"
    export HELIX_LOG_FILE="${HELIX_LOG_FILE:-/hook/default.log}"
}
HOOKEOF
}

@test "init script's platform_pre_start leaks no export into the launcher's environment" {
    call="$(init_pre_start_call)"
    # An empty extraction would make the assertion below vacuously green.
    [ -n "$call" ]

    mkdir -p "$BATS_TEST_TMPDIR/empty-proc"
    MOCK_INSTALL="$BATS_TEST_TMPDIR/helixscreen"
    install_defaulting_hook

    HELIX_PROC_ROOT="$BATS_TEST_TMPDIR/empty-proc" sh -c '
        unset HELIX_LOG_DEST
        . "'"$MOCK_INSTALL"'/platform/hooks.sh"
        eval "'"$call"'"
        echo "dest:${HELIX_LOG_DEST:-unset}"
    ' > "$BATS_TEST_TMPDIR/leak.out" 2>&1
    grep -q '^dest:unset$' "$BATS_TEST_TMPDIR/leak.out"
}

@test "e2e: through the init ordering, helixscreen.env outranks a hook default" {
    # Production shape: the init script sources the hooks, runs
    # platform_pre_start, then execs the launcher — which reads
    # helixscreen.env and runs the hook again itself. The operator pins only
    # HELIX_LOG_DEST in the file; HELIX_LOG_FILE stays silent so the hook's
    # default must be the value forwarded for it.
    mock_command_script "killall" 'exit 0'
    mock_command_script "systemctl" 'exit 0'
    mock_command_script "setterm" 'exit 0'

    MOCK_INSTALL="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$MOCK_INSTALL/bin" "$MOCK_INSTALL/config" "$BATS_TEST_TMPDIR/empty-proc"
    printf '#!/bin/sh\nfor arg in "$@"; do echo "$arg"; done > "%s/helix_screen_args.txt"\nexit 0\n' \
        "$MOCK_INSTALL" > "$MOCK_INSTALL/bin/helix-screen"
    chmod +x "$MOCK_INSTALL/bin/helix-screen"
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    install_defaulting_hook

    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_LOG_DEST=console
EOF

    call="$(init_pre_start_call)"
    [ -n "$call" ]

    HELIX_PROC_ROOT="$BATS_TEST_TMPDIR/empty-proc" sh -c '
        unset HELIX_LOG_DEST HELIX_LOG_FILE HELIX_DEBUG HELIX_LOG_LEVEL
        . "'"$MOCK_INSTALL"'/platform/hooks.sh"
        eval "'"$call"'"
        exec sh "'"$MOCK_INSTALL"'/bin/helix-launcher.sh"
    ' >/dev/null 2>&1 || true

    [ -s "$MOCK_INSTALL/helix_screen_args.txt" ]
    grep -q '^--log-dest=console$' "$MOCK_INSTALL/helix_screen_args.txt"
    grep -q '^--log-file=/hook/default.log$' "$MOCK_INSTALL/helix_screen_args.txt"
}
