#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Apply one patch to one vendored submodule with a verdict that cannot lie.
#
# A bare `git apply --check` cannot distinguish "already applied" from "this
# patch no longer matches the submodule": both exit non-zero, so an else-branch
# that prints "already applied" turns a dead patch into a green line. A reverse
# check does not settle it either — several patches share one file (seven touch
# src/misc/lv_event.c), and once a sibling patch has moved the context, a
# correctly applied patch neither forward- nor reverse-applies.
#
# So the verdict is three-way, and the hard failure belongs to the only run
# that can judge: a from-clean apply (HELIX_PATCHES_FROM_CLEAN=1, set by `make
# reapply-patches`). From clean nothing is applied yet, so every patch must
# take the apply branch; anything else means the patch does not match the
# submodule. Incremental runs warn instead, because there a sibling patch
# earlier in the same recipe may legitimately have moved the context.
#
# The flag is a claim about the tree, and the claim is verified, not assumed:
# `make clean` deletes the stamp but leaves the submodules patched, and on a
# patched tree shared-file shadowing makes healthy patches read "neither", so
# an unverified flag would fail a perfectly good checkout. A mk/patches.mk run
# settles the claim once at recipe start into HELIX_FROM_CLEAN_SENTINEL; a
# standalone invocation judges the submodule directly.
#
# Usage: apply_submodule_patch.sh <submodule-dir> <patch-file> <label> [note]
#
# The optional note names the runtime consequence of building without the
# patch. It is appended to the two verdicts that mean "this patch may be
# missing" - the in-place warn and the from-clean fatal - never to the
# success lines.

set -u

submodule=$1
patch_file=$2
label=$3
note=${4:-}

# A pre-commit hook exports GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE for the
# superproject, and anything spawning git under it inherits them: `git -C
# lib/lvgl ...` would then answer with the superproject's repository instead
# of the submodule's. Scrub them so every question below is answered by the
# submodule the -C path names (mirrors GIT_NOENV in mk/patches.mk).
git_submodule() {
  env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
    git -C "$submodule" "$@"
}

if [ -n "${NO_COLOR:-}" ] ||
   ! { [ -t 0 ] && [ -t 2 ] && [ -n "${TERM:-}" ] && [ "${TERM:-}" != "dumb" ]; }; then
  green='' yellow='' red='' reset=''
else
  green=$'\033[32m' yellow=$'\033[33m' red=$'\033[31m' reset=$'\033[0m'
fi

# Did this run actually start from a pristine checkout? The fatal verdict is
# licensed by the answer, and the answer cannot come from the flag alone: on a
# patched tree a healthy shared-file patch reads "neither" too.
from_clean_verified() {
  local sentinel="${HELIX_FROM_CLEAN_SENTINEL:-}"
  if [ -n "$sentinel" ] && [ -f "$sentinel" ]; then
    [ "$(cat "$sentinel" 2>/dev/null)" = "1" ]
    return
  fi
  # Standalone invocation with no recipe guard: judge the submodule directly.
  # A submodule whose state git cannot read is treated as not verifiable.
  git_submodule status --porcelain >/dev/null 2>&1 || return 1
  [ -z "$(git_submodule status --porcelain 2>/dev/null)" ]
}

if git_submodule apply --check "$patch_file" 2>/dev/null; then
  echo "${yellow}→ Applying ${label}...${reset}"
  if ! git_submodule apply "$patch_file"; then
    echo "${red}✗ ${label}: git apply failed${reset}"
    exit 1
  fi
  echo "${green}✓ ${label} applied${reset}"
elif git_submodule apply --check --reverse "$patch_file" 2>/dev/null; then
  echo "${green}✓ ${label} already applied${reset}"
elif [ "${HELIX_PATCHES_FROM_CLEAN:-0}" = "1" ] && from_clean_verified; then
  echo "${red}✗ ${label} does not apply to a clean checkout — the patch and the submodule disagree.${note:+ $note}" >&2
  echo "${red}  Regenerate it: patches/README.md § \"Regenerating a patch whose file is shared\"${reset}" >&2
  exit 1
else
  echo "${yellow}⚠ ${label} is not verifiable in place: neither applies nor reverses (later patches may share its files).${note:+ $note} Run 'make reapply-patches' to judge it from a clean checkout${reset}"
fi
