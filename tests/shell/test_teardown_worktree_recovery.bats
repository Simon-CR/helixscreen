#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/teardown-worktree.sh resolves its target by combining a filesystem
# path guess with a check against `git worktree list --porcelain`. When a
# worktree's OWN .git pointer is already gone - a previous teardown that got
# partway through rm -rf before root-owned leftovers (a docker build, say)
# stopped it - every `git -C "$WT_ABS"` call in the script discovers upward
# past that missing pointer and silently answers for whatever repository it
# finds next, which for a worktree living under the main tree is the main
# tree itself. The script then reports the main tree's branch and uncommitted
# files as the worktree's own, and --force on it attempts to delete the main
# tree's own checked-out branch - saved only by git's own refusal to delete
# a branch checked out somewhere.
#
# These tests pin that a worktree missing its .git pointer is refused with a
# clear message naming only its own leftover files, never the main repo's,
# and that a normal teardown (no corruption) still completes.

SCRIPT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/teardown-worktree.sh"

setup() {
    load helpers
    MAIN="$BATS_TEST_TMPDIR/mainrepo"
    mkdir -p "$MAIN"
    cd "$MAIN" || return 1
    git init -q -b master .
    git config user.email t@example.com
    git config user.name tester
    echo hello > README.md
    git add README.md
    git commit -qm base --no-verify
}

make_worktree() {
    local name="$1"
    mkdir -p "$MAIN/.worktrees"
    git worktree add -q -b "feature/$name" "$MAIN/.worktrees/$name"
}

@test "a normal worktree tears down cleanly" {
    make_worktree normal
    run "$SCRIPT" normal --into master
    [ "$status" -eq 0 ]
    contains "Teardown complete" "$output"
    [ ! -d "$MAIN/.worktrees/normal" ]
    run git -C "$MAIN" worktree list --porcelain
    lacks ".worktrees/normal" "$output"
}

@test "a worktree missing its .git pointer is refused, not misattributed to the main tree" {
    make_worktree broken
    echo leftover > "$MAIN/.worktrees/broken/leftover.txt"
    rm -f "$MAIN/.worktrees/broken/.git"

    # Untracked and never committed, so it exists ONLY in the main tree's own
    # working directory - never checked out into any worktree. A script that
    # escapes to the main tree via `git -C <worktree> status` would report it;
    # one that does not can never mention it.
    echo only-in-main-tree > "$MAIN/main-tree-uncommitted.txt"

    run "$SCRIPT" broken --into master
    [ "$status" -ne 0 ]

    # The whole point: never names the main tree's own state.
    lacks "main-tree-uncommitted.txt" "$output"
    lacks "branch:   master" "$output"

    # It does explain what is actually left in the worktree.
    contains "leftover.txt" "$output"
    contains "no .git of its own" "$output"

    # Refusing must not have touched anything - directory and registration
    # are both still there for the operator (or --force) to act on.
    [ -d "$MAIN/.worktrees/broken" ]
    run git -C "$MAIN" worktree list --porcelain
    contains ".worktrees/broken" "$output"

    # And the main tree's own untracked marker is still just sitting there -
    # nothing tried to report on it, let alone touch it.
    [ -f "$MAIN/main-tree-uncommitted.txt" ]
}

@test "--force finishes a worktree missing its .git pointer without touching the main tree's branch" {
    make_worktree broken2
    echo leftover > "$MAIN/.worktrees/broken2/leftover.txt"
    rm -f "$MAIN/.worktrees/broken2/.git"

    run "$SCRIPT" broken2 --into master --force
    [ "$status" -eq 0 ]

    # Recovery mode skips branch cleanup outright rather than guess at
    # containment from a repo it can no longer safely query - it must never
    # even ATTEMPT a branch deletion.
    lacks "Deleting branch" "$output"

    [ ! -d "$MAIN/.worktrees/broken2" ]
    run git -C "$MAIN" worktree list --porcelain
    lacks ".worktrees/broken2" "$output"

    # The main tree's own branch and tracked file are untouched.
    run git -C "$MAIN" branch --show-current
    contains "master" "$output"
    [ -f "$MAIN/README.md" ]

    run git -C "$MAIN" branch --list "feature/broken2"
    contains "feature/broken2" "$output"
}

@test "an unregistered directory is refused, never treated as a worktree" {
    mkdir -p "$BATS_TEST_TMPDIR/not-a-worktree"
    run "$SCRIPT" "$BATS_TEST_TMPDIR/not-a-worktree" --into master
    [ "$status" -ne 0 ]
    contains "does not list that path as a worktree" "$output"
}

@test "the main tree itself is refused even when named directly" {
    run "$SCRIPT" "$MAIN" --into master
    [ "$status" -ne 0 ]
    contains "that is the main tree" "$output"
}
