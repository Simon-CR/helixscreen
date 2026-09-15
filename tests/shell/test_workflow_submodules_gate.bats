#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_workflow_submodules.py - the gate over workflow
# jobs that depend on patched submodules.
#
# Two rules: a job running the bats suite must initialize submodules and apply
# patches (test_lvgl_event_code_gate.bats copies a file only a patched checkout
# has), and every `make apply-patches` step in any job must pass
# HELIX_PATCHES_FROM_CLEAN=1 - a CI runner is a fresh clone, where a patch that
# can neither apply nor reverse is dead, not shadowed, and must fail the job
# rather than warn into a build missing the patch.

load helpers

GATE="$BATS_TEST_DIRNAME/../../scripts/check_workflow_submodules.py"

make_fixture() {
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/wf"
    rm -rf "$ROOT"
    mkdir -p "$ROOT/.github/workflows" "$ROOT/.github/actions/init-submodules"
    printf 'name: init\nruns:\n  using: composite\n  steps: []\n' \
        > "$ROOT/.github/actions/init-submodules/action.yml"
    cp "$1" "$ROOT/.github/workflows/ci.yml"
    echo "$ROOT"
}

@test "a fully wired workflow passes" {
    ROOT=$(make_fixture "$BATS_TEST_DIRNAME/fixtures/workflow_submodules_ok.yml")
    run bash -c "cd '$ROOT' && python3 '$GATE'"
    [ "$status" -eq 0 ]
    grep -q 'init and patch their submodules' <<<"$output"
}

@test "an unflagged apply-patches step fails the gate" {
    ROOT=$(make_fixture "$BATS_TEST_DIRNAME/fixtures/workflow_submodules_bare_apply.yml")
    run bash -c "cd '$ROOT' && python3 '$GATE'"
    [ "$status" -eq 1 ]
    grep -q 'without HELIX_PATCHES_FROM_CLEAN=1' <<<"$output"
}

@test "the from-clean rule holds for jobs that never touch the shell suite" {
    ROOT=$(make_fixture "$BATS_TEST_DIRNAME/fixtures/workflow_submodules_bare_apply.yml")
    run bash -c "cd '$ROOT' && python3 '$GATE'"
    [ "$status" -eq 1 ]
    grep -q "job 'build'" <<<"$output"
}

@test "a shell-suite job without submodule init still fails the gate" {
    ROOT=$(make_fixture "$BATS_TEST_DIRNAME/fixtures/workflow_submodules_no_init.yml")
    run bash -c "cd '$ROOT' && python3 '$GATE'"
    [ "$status" -eq 1 ]
    grep -q 'missing' <<<"$output"
}
