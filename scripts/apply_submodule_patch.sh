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
# Usage: apply_submodule_patch.sh <submodule-dir> <patch-file> <label>

set -u

submodule=$1
patch_file=$2
label=$3

if [ -n "${NO_COLOR:-}" ] ||
   ! { [ -t 0 ] && [ -t 2 ] && [ -n "${TERM:-}" ] && [ "${TERM:-}" != "dumb" ]; }; then
  green='' yellow='' red='' reset=''
else
  green=$'\033[32m' yellow=$'\033[33m' red=$'\033[31m' reset=$'\033[0m'
fi

if git -C "$submodule" apply --check "$patch_file" 2>/dev/null; then
  echo "${yellow}→ Applying ${label}...${reset}"
  if ! git -C "$submodule" apply "$patch_file"; then
    echo "${red}✗ ${label}: git apply failed${reset}"
    exit 1
  fi
  echo "${green}✓ ${label} applied${reset}"
elif git -C "$submodule" apply --check --reverse "$patch_file" 2>/dev/null; then
  echo "${green}✓ ${label} already applied${reset}"
elif [ "${HELIX_PATCHES_FROM_CLEAN:-0}" = "1" ]; then
  echo "${red}✗ ${label} does not apply to a clean checkout — the patch and the submodule disagree." >&2
  echo "${red}  Regenerate it: patches/README.md § \"Regenerating a patch whose file is shared\"${reset}" >&2
  exit 1
else
  echo "${yellow}⚠ ${label} is not verifiable in place: neither applies nor reverses (later patches may share its files). Run 'make reapply-patches' to judge it from a clean checkout${reset}"
fi
