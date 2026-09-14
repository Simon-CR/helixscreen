#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-test for scripts/audit_codebase.sh --files - the memory-safety audit's
# pre-commit mode (quality-checks.sh's qc_mem_safety calls it this way).
#
# --files names the staged set (`git diff --cached --name-only`), but every
# check in FILE MODE greps a path on disk. Stage a violation and then revert
# the working file to something clean, and a plain `grep "$f"` finds the clean
# copy and reports nothing while the bad blob commits. This pins that the
# script instead scans each staged file's INDEX content.

GATE="scripts/audit_codebase.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    GATE_ORIG="$(pwd)/$GATE"
}

setup_tmp_repo() {
    TMP_REPO="$(mktemp -d "${BATS_TEST_TMPDIR:-${BATS_TMPDIR:-/tmp}}/audit-staged-XXXXXX")"
    cd "$TMP_REPO" || return 1
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test"
    mkdir -p src/ui
    printf 'void f() {\n    int x = 1;\n}\n' > src/ui/ui_panel_foo.cpp
    git add -A && git commit -qm base
}

@test "--files catches a violation in the STAGED blob, not a reverted working file" {
    setup_tmp_repo
    printf 'void f() {\n    lv_obj_t* o = lv_obj_create(NULL);\n    lv_obj_set_user_data(o, new int(5));\n}\n' \
        > src/ui/ui_panel_foo.cpp
    git add src/ui/ui_panel_foo.cpp
    printf 'void f() {\n    int x = 1;\n}\n' > src/ui/ui_panel_foo.cpp
    run bash "$GATE_ORIG" --files src/ui/ui_panel_foo.cpp
    [ "$status" -eq 1 ]
    [[ "$output" == *"allocates user_data with 'new' but has no LV_EVENT_DELETE handler"* ]]
}

@test "--files stays silent on a clean staged file with a dirty violation on disk" {
    setup_tmp_repo
    # Genuinely different from the base commit (a second clean statement), so
    # the file shows up as staged at all - content byte-identical to HEAD
    # stages nothing for git to report, which would pass this test for free.
    printf 'void f() {\n    int x = 1;\n    int y = 2;\n}\n' > src/ui/ui_panel_foo.cpp
    git add src/ui/ui_panel_foo.cpp
    printf 'void f() {\n    lv_obj_t* o = lv_obj_create(NULL);\n    lv_obj_set_user_data(o, new int(5));\n}\n' \
        > src/ui/ui_panel_foo.cpp
    run bash "$GATE_ORIG" --files src/ui/ui_panel_foo.cpp
    [ "$status" -eq 0 ]
}

@test "--files reports the real filename in a message, not the staged scratch path" {
    setup_tmp_repo
    printf 'void f() {\n    lv_obj_t* o = lv_obj_create(NULL);\n    lv_obj_set_user_data(o, new int(5));\n}\n' \
        > src/ui/ui_panel_foo.cpp
    git add src/ui/ui_panel_foo.cpp
    run bash "$GATE_ORIG" --files src/ui/ui_panel_foo.cpp
    [ "$status" -eq 1 ]
    [[ "$output" == *"ui_panel_foo.cpp:"* ]]
}

# A bare positional file argument (no --files) is a manual spot-check against
# the real file on disk, unrelated to the staged set, and must stay that way.
@test "a bare positional argument still reads the working tree, not the index" {
    setup_tmp_repo
    printf 'void f() {\n    lv_obj_t* o = lv_obj_create(NULL);\n    lv_obj_set_user_data(o, new int(5));\n}\n' \
        > src/ui/ui_panel_foo.cpp
    run bash "$GATE_ORIG" src/ui/ui_panel_foo.cpp
    [ "$status" -eq 1 ]
}
