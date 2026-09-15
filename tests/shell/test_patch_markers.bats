#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the patch marker precondition: scripts/gen_patch_markers.py
# derives mk/patch-markers.tsv, scripts/check_patch_markers.py enforces it.
#
# A marker is one line a patch adds (or removes) that upstream never
# contained, so "is this patch's effect in the checkout" becomes a plain text
# search. It must fail when a wired patch's effect is missing from a tree the
# patch stamp calls current, and it must stay silent on a healthy patched
# tree where the three-way apply verdict only warns because sibling patches
# moved shared context - the two checks answer different questions and must
# not share failure modes.
#
# Fixtures are rewritten whole with printf rather than edited with sed: BSD
# sed cannot insert newlines in a replacement, and these files are three
# lines long.

load helpers

GEN="scripts/gen_patch_markers.py"
CHECK="scripts/check_patch_markers.py"
VERDICT="scripts/apply_submodule_patch.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1

    # make exports this into its recipe shells, and bats runs inside one; a
    # leaked real-tree sentinel would change which verdict the helper prints.
    # The from-clean flag leaks the same way from CI's workflow env.
    unset HELIX_FROM_CLEAN_SENTINEL HELIX_PATCHES_FROM_CLEAN

    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/pm"
    LV="$ROOT/sub_lvgl"
    HV="$ROOT/sub_libhv"
    P="$ROOT/patches"
    MK="$ROOT/mini.mk"
    TSV="$ROOT/markers.tsv"
    mkdir -p "$LV/src" "$HV/base" "$P"

    for sub in "$LV" "$HV"; do
        git -C "$sub" init -q
        git -C "$sub" config user.email t@example.invalid
        git -C "$sub" config user.name "Fixture"
    done

    printf '%s\n' 'alpha-context-line' 'beta-context-line' 'gamma-context-line' \
        > "$LV/src/core.c"
    git -C "$LV" add src/core.c
    git -C "$LV" commit -qm pristine

    printf '%s\n' 'kept-upstream-line' 'upstream_line_removed_by_patch' \
        > "$HV/base/other.c"
    git -C "$HV" add base/other.c
    git -C "$HV" commit -qm pristine

    # add.patch introduces a line upstream never had.
    printf '%s\n' 'alpha-context-line' 'helix_marker_line_one_two_three();' \
        'beta-context-line' 'gamma-context-line' > "$LV/src/core.c"
    git -C "$LV" diff -- src/core.c > "$P/add.patch"
    git -C "$LV" checkout -- src/core.c

    # remove.patch deletes a distinctive upstream line.
    printf '%s\n' 'kept-upstream-line' > "$HV/base/other.c"
    git -C "$HV" diff -- base/other.c > "$P/remove.patch"
    git -C "$HV" checkout -- base/other.c

    printf '%s\n' \
        $'\t$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/add.patch "add marker patch" "Without it the widget crashes on show."' \
        $'\t$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/remove.patch "remove marker patch"' \
        > "$MK"

    git -C "$LV" apply "$P/add.patch"
    git -C "$HV" apply "$P/remove.patch"
}

gen() {
    python3 "$GEN" --mk "$MK" --tsv "$TSV" --patch-dir "$P" \
        --lvgl "$LV" --libhv "$HV" "$@"
}

check() {
    python3 "$CHECK" --mk "$MK" --tsv "$TSV" --patch-dir "$P" \
        --lvgl "$LV" --libhv "$HV"
}

@test "derives a marker per wired patch and passes on the applied fixture" {
    run gen --write
    [ "$status" -eq 0 ]
    [ "$(grep -vc '^sha256' "$TSV")" -eq 2 ]
    run check
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    # Derivation is deterministic: a second run reproduces the table.
    cp "$TSV" "$TSV.first"
    gen --write >/dev/null
    cmp "$TSV" "$TSV.first"
}

@test "a stripped marker fails the check naming the patch, consequence and remedy" {
    gen --write >/dev/null
    printf '%s\n' 'alpha-context-line' 'beta-context-line' 'gamma-context-line' \
        > "$LV/src/core.c"
    run check
    [ "$status" -eq 1 ]
    grep -q 'add.patch' <<<"$output"
    grep -q 'Without it the widget crashes on show.' <<<"$output"
    grep -q "make reapply-patches" <<<"$output"
}

@test "a reintroduced removed line fails the check" {
    gen --write >/dev/null
    printf '%s\n' 'kept-upstream-line' 'upstream_line_removed_by_patch' \
        >> "$HV/base/other.c"
    run check
    [ "$status" -eq 1 ]
    grep -q 'remove.patch' <<<"$output"
    grep -q 'is back in' <<<"$output"
}

@test "a changed patch file demands a table regen" {
    gen --write >/dev/null
    printf '# touched\n' >> "$P/add.patch"
    run check
    [ "$status" -eq 1 ]
    grep -q 'make regen-patch-markers' <<<"$output"
}

@test "a wired stanza without a table row fails instead of passing vacuously" {
    gen --write >/dev/null
    printf '%s\n' \
        $'\t$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/brand_new.patch "brand new"' \
        >> "$MK"
    run check
    [ "$status" -eq 1 ]
    grep -q 'brand_new.patch' <<<"$output"
    grep -q 'make regen-patch-markers' <<<"$output"
}

@test "the generator refuses a marker two patches could satisfy" {
    # A second patch adding the same line to the same file: whichever is
    # missing, the other satisfies the check, so the line attributes to
    # neither and derivation must say so rather than emit a hollow table.
    # twin.patch is written against the tree add.patch produces, since git
    # diff can only produce patches against the committed (pristine) state.
    cat > "$P/twin.patch" <<'EOF'
diff --git a/src/core.c b/src/core.c
--- a/src/core.c
+++ b/src/core.c
@@ -1,4 +1,5 @@
 alpha-context-line
 helix_marker_line_one_two_three();
 beta-context-line
+helix_marker_line_one_two_three();
 gamma-context-line
EOF
    git -C "$LV" apply "$P/twin.patch"
    printf '%s\n' \
        $'\t$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/twin.patch "twin marker patch"' \
        >> "$MK"
    run gen --write
    [ "$status" -ne 0 ]
    grep -q 'no eligible marker' <<<"$output"
    [ ! -f "$TSV" ]
}

@test "markers stay silent while the apply verdict warns on the same tree" {
    gen --write >/dev/null
    # A sibling patch rewrote the added line: the verdict can neither apply
    # nor reverse add.patch against this tree, but the marker (a text search)
    # still proves the patch's effect is present.
    printf '%s\n' 'alpha-context-line' \
        'sibling_rewrote_helix_marker_line_one_two_three();' \
        'beta-context-line' 'gamma-context-line' > "$LV/src/core.c"
    run bash "$VERDICT" "$LV" "$P/add.patch" "fixture add patch"
    [ "$status" -eq 0 ]
    grep -q 'not verifiable in place' <<<"$output"
    run check
    [ "$status" -eq 0 ]
}

@test "every compile rule gated on the patch stamp is gated on the marker stamp" {
    gated=$(grep -F '$(PATCHES_STAMP)' mk/rules.mk mk/egl-link.mk \
        | grep -v '^[^:]*:[[:space:]]*#' || true)
    [ "$(printf '%s\n' "$gated" | grep -c .)" -ge 8 ]
    ungated=$(printf '%s\n' "$gated" | grep -vF '$(PATCH_MARKER_STAMP)' || true)
    [ -z "$ungated" ] || { printf 'ungated rules:\n%s\n' "$ungated"; return 1; }
}

@test "the drift stamp write is gated on the marker check" {
    # A stanza that only warned must not have the tree it left behind recorded
    # as the applied state: the marker check sits between the last apply stanza
    # and the drift stamp write, so a tree missing a patch fails the recipe
    # before any stamp blesses it.
    last_stanza=$(grep -nF '$(APPLY_PATCH)' mk/patches.mk | tail -1 | cut -d: -f1)
    gate_line=$(grep -nF '$(PATCH_MARKER_CHECK);' mk/patches.mk | tail -1 | cut -d: -f1)
    stamp_line=$(grep -nF -- '--write-stamp' mk/patches.mk | tail -1 | cut -d: -f1)
    [ -n "$last_stanza" ] && [ -n "$gate_line" ] && [ -n "$stamp_line" ]
    [ "$gate_line" -gt "$last_stanza" ]
    [ "$stamp_line" -gt "$gate_line" ]
}
