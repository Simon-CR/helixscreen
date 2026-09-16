#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The claim store belongs to the repository, not to whichever worktree is asking.
#
# A session asking "is main free?" is almost never standing in main, so a store
# anchored per-worktree would answer FREE for a tree another session is holding,
# and would let a second `take` succeed on top of the first. An advisory lock is
# allowed to be ignored; it is not allowed to fail open.

load helpers

CLAIM="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/helix-claim"

setup() {
    unset HELIX_CLAIM_DIR
    MAIN="$BATS_TEST_TMPDIR/mainrepo"
    LINKED="$MAIN/.worktrees/feat"
    mkdir -p "$MAIN"
    cd "$MAIN" || return 1
    git init -q .
    git config user.email t@example.com
    git config user.name tester
    echo a > f.txt
    git add f.txt
    git commit -qm base --no-verify
    git worktree add -q -b feat "$LINKED"
    # An owner that outlives the test body, so the claim reads LIVE throughout.
    sleep 120 &
    OWNER=$!
}

teardown() {
    [ -n "${OWNER:-}" ] && kill "$OWNER" 2>/dev/null
    return 0
}

@test "a claim taken in the main tree is LIVE from a linked worktree" {
    "$CLAIM" take worktree:mainrepo "held by main" --pid "$OWNER" >/dev/null
    cd "$LINKED"
    run "$CLAIM" check worktree:mainrepo
    [ "$status" -eq 1 ]
    contains "held by main" "$output"
}

@test "take from a linked worktree refuses a claim the main tree holds" {
    "$CLAIM" take worktree:mainrepo "held by main" --pid "$OWNER" >/dev/null
    cd "$LINKED"
    run "$CLAIM" take worktree:mainrepo "second session"
    [ "$status" -ne 0 ]
    contains "REFUSED" "$output"
}

@test "a claim taken in a linked worktree is LIVE from the main tree" {
    cd "$LINKED"
    "$CLAIM" take worktree:feat "held by the worktree" --pid "$OWNER" >/dev/null
    cd "$MAIN"
    run "$CLAIM" check worktree:feat
    [ "$status" -eq 1 ]
    contains "held by the worktree" "$output"
}

@test "one store serves the whole repository" {
    "$CLAIM" take worktree:mainrepo "held" --pid "$OWNER" >/dev/null
    cd "$LINKED"
    "$CLAIM" take build:feat "a build" --pid "$OWNER" >/dev/null
    run bash -c "find '$MAIN' -name .helix-claims -type d | wc -l"
    [ "$output" -eq 1 ]
}

@test "list from a linked worktree sees the main tree's claims" {
    "$CLAIM" take worktree:mainrepo "held by main" --pid "$OWNER" >/dev/null
    cd "$LINKED"
    run "$CLAIM" list
    contains "held by main" "$output"
}

@test "HELIX_CLAIM_DIR still overrides the repository store" {
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/elsewhere"
    "$CLAIM" take worktree:mainrepo "held" --pid "$OWNER" >/dev/null
    [ -d "$HELIX_CLAIM_DIR" ]
    run bash -c "find '$MAIN' -name .helix-claims -type d | wc -l"
    [ "$output" -eq 0 ]
}

# A failed take is loud; an unconditional release is silent on both sides. It hands
# the lock to whoever asked and never tells the holder, so release refuses a claim a
# different live process holds.

@test "release refuses a claim another live session holds" {
    "$CLAIM" take worktree:mainrepo "held by another" --pid "$OWNER" >/dev/null
    run "$CLAIM" release worktree:mainrepo
    [ "$status" -eq 1 ]
    contains "REFUSED" "$output"
    run "$CLAIM" check worktree:mainrepo
    [ "$status" -eq 1 ]
}

@test "release --force drops a claim another live session holds" {
    "$CLAIM" take worktree:mainrepo "held by another" --pid "$OWNER" >/dev/null
    run "$CLAIM" release --force worktree:mainrepo
    [ "$status" -eq 0 ]
    run "$CLAIM" check worktree:mainrepo
    [ "$status" -eq 0 ]
}

@test "release still clears a stale claim, which teardown-worktree depends on" {
    sleep 60 &
    dead=$!
    "$CLAIM" take worktree:mainrepo "dead owner" --pid "$dead" >/dev/null
    kill "$dead" 2>/dev/null
    wait "$dead" 2>/dev/null || true
    run "$CLAIM" release worktree:mainrepo
    [ "$status" -eq 0 ]
    contains "RELEASED" "$output"
}

@test "release drops a claim this session took" {
    "$CLAIM" take worktree:mainrepo "mine" >/dev/null
    run "$CLAIM" release worktree:mainrepo
    [ "$status" -eq 0 ]
    contains "RELEASED" "$output"
}

# Outside a Claude session there is no `claude` ancestor to anchor the owner to,
# so the owner is derived from the caller's own process. A run that takes in one
# shell and releases from a child shell must still recognise its own claim: bats
# `run`, a wrapper script and a CI step all have that shape, and an owner that
# differs between the two leaves a LIVE claim nobody can release.
#
# The shape needs all three of: no `claude` ancestor (the detached launcher exits,
# forks so its child reparents to init), the taker's parent still alive at release time,
# and a release whose own parent differs. The trailing `true` supplies the last
# one: `bash -c` with a single command execs it, keeping the parent unchanged.
@test "a run with no claude ancestor releases a claim it took from a child shell" {
    local out="$BATS_TEST_TMPDIR/detached.out"
    setsid --fork bash -c "cd '$MAIN'
        sleep 0.5
        '$CLAIM' take worktree:mainrepo mine
        bash -c \"'$CLAIM' release worktree:mainrepo; true\"
        '$CLAIM' check worktree:mainrepo" > "$out" 2>&1 &
    local i
    for i in $(seq 200); do
        [ -f "$out" ] && grep -qE "FREE|LIVE" "$out" 2>/dev/null && break
        sleep 0.05
    done
    contains "RELEASED" "$(cat "$out")"
    contains "FREE" "$(cat "$out")"
}
