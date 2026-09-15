#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/apply_submodule_patch.sh - the single apply verdict
# shared by every stanza in mk/patches.mk.
#
# The verdict must be three-way and honest: a patch either applies, is
# recognized as already applied by a reverse check, or is refused. An
# else-branch that prints "already applied" whenever `git apply --check`
# fails turns a dead patch into a green line, because the same non-zero exit
# covers "already applied" and "will never apply again". Only a from-clean
# run (HELIX_PATCHES_FROM_CLEAN=1, set by `make reapply-patches`) can judge
# the third case: there nothing is applied yet, so every patch must take its
# apply branch. Incremental runs warn instead of failing, because a sibling
# patch earlier in the same recipe may legitimately have moved the context a
# shared-file patch needs.

load helpers

HELPER="scripts/apply_submodule_patch.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1

    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    SUB="$ROOT/lib/fake"
    mkdir -p "$ROOT/patches" "$SUB"

    git -C "$SUB" init -q
    git -C "$SUB" config user.email t@example.invalid
    git -C "$SUB" config user.name "Fixture"
    printf 'one\n' > "$SUB/one.txt"
    git -C "$SUB" add one.txt
    git -C "$SUB" commit -qm pristine

    # good.patch matches the pristine file.
    printf 'one\nALPHA\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/good.patch"
    git -C "$SUB" checkout -- one.txt

    # dead.patch matches no state the submodule can ever reach: neither a
    # forward nor a reverse check can pass.
    cat > "$ROOT/patches/dead.patch" <<'EOF'
diff --git a/one.txt b/one.txt
--- a/one.txt
+++ b/one.txt
@@ -1 +1 @@
-drifted-base
+drifted-new
EOF
}

@test "applies a patch that matches the submodule" {
    run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch applied' <<<"$output"
    grep -q '^ALPHA$' "$SUB/one.txt"
}

@test "recognizes an already-applied patch through the reverse check" {
    git -C "$SUB" apply "$ROOT/patches/good.patch"
    run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch already applied' <<<"$output"
    # The reverse-apply branch must leave the tree alone.
    [ "$(git -C "$SUB" diff -- one.txt | grep -c '^+ALPHA$')" -eq 1 ]
}

@test "a dead patch warns but does not fail an incremental run" {
    run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'not verifiable in place' <<<"$output"
    # The warning must not wear a success checkmark: a green glyph here is
    # how a dead patch hides.
    ! grep -q '✓.*fixture patch' <<<"$output"
}

@test "a dead patch fails a from-clean run" {
    HELIX_PATCHES_FROM_CLEAN=1 run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 1 ]
    grep -q 'does not apply to a clean checkout' <<<"$output"
    grep -q 'Regenerate it' <<<"$output"
}

@test "a matching patch passes a from-clean run" {
    HELIX_PATCHES_FROM_CLEAN=1 run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch applied' <<<"$output"
}

@test "redirected output carries no ANSI escapes" {
    run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    ! grep -q $'\033' <<<"$output"
}

@test "mk/patches.mk routes every LVGL stanza through the helper" {
    # The helper owns the apply verdict; an LVGL stanza that runs `git apply`
    # directly restores the per-stanza else-branch the helper replaced. The
    # libhv stanzas keep their own sentinel guards, which exit 1 on failure.
    [ "$(grep -cE 'APPLY_PATCH\) \$\((LVGL|LIBHV)_DIR\)' mk/patches.mk)" -gt 50 ]
    ! grep -qE '^\t+git -C \$\(LVGL_DIR\) apply' mk/patches.mk
}

@test "reapply-patches judges from clean" {
    # The from-clean verdict only binds when the flag actually reaches the
    # recipe shells: the sub-make must pass it and the variable must be
    # exported.
    grep -q 'force-apply-patches HELIX_PATCHES_FROM_CLEAN=1' mk/patches.mk
    grep -q '^export HELIX_PATCHES_FROM_CLEAN' mk/patches.mk
}
