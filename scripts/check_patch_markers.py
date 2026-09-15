#!/usr/bin/env python3
"""Fail the build when a wired patch's marker is absent from the checkout.

The three-way apply verdict in scripts/apply_submodule_patch.sh answers "can
this patch apply or reverse here", which git must adjudicate and which shared
files make ambiguous mid-sequence. This check answers a narrower question
git-free: "is the one line this patch uniquely adds (or removes) present in
the file". It runs on every build, including docker trees rsynced from
worktrees where the submodules are not git repositories at all.

Three layers, each failing loudly:

  coverage   every wired apply stanza in mk/patches.mk has a row in the table
             (a new patch without a row would otherwise pass vacuously);
  staleness  each patch file's sha256 matches the recorded one (a changed
             patch invalidates its derivation - regenerate the table);
  presence   the marker text is in (for '+') or absent from (for '-') the
             target file.

Both fixable states name their remedy: 'make reapply-patches' restores a
missing patch, 'make regen-patch-markers' refreshes a stale table.
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_patch_markers import stanzas  # noqa: E402 - single stanza grammar

COLUMNS = ("sha256", "patch", "kind", "dir", "file", "marker", "label", "note")


def read_table(path):
    rows = []
    try:
        fh = open(path, encoding="utf-8")
    except OSError:
        sys.exit(f"{path}: no marker table - run 'make regen-patch-markers'")
    with fh:
        header = fh.readline().rstrip("\n").split("\t")
        if header != list(COLUMNS):
            sys.exit(f"{path}: unexpected header {header!r} - regenerate the table")
        for line in fh:
            line = line.rstrip("\n")
            if line:
                rows.append(dict(zip(COLUMNS, line.split("\t"))))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mk", default="mk/patches.mk")
    ap.add_argument("--tsv", default="mk/patch-markers.tsv")
    ap.add_argument("--patch-dir", default="patches")
    ap.add_argument("--lvgl", default="lib/lvgl")
    ap.add_argument("--libhv", default="lib/libhv")
    ap.add_argument("--list-files", action="store_true",
                    help="print the files the table reads, and exit")
    args = ap.parse_args()
    dirs = {"LVGL_DIR": args.lvgl, "LIBHV_DIR": args.libhv}

    rows = read_table(args.tsv)
    if args.list_files:
        seen = set()
        for row in rows:
            key = (row["dir"], row["file"])
            if key not in seen:
                seen.add(key)
                print(f"{dirs[row['dir']]}/{row['file']}")
        return

    wired = {name: label for _, name, label, _ in stanzas(args.mk)}
    tabled = {row["patch"]: row for row in rows}
    failures = []

    missing_rows = [n for n in wired if n not in tabled]
    for name in missing_rows:
        failures.append(
            f"{name} ({wired[name]}) has no marker row - a patch the table "
            f"does not cover reads as applied forever. Run 'make regen-patch-markers'.")
    stale_rows = [r for r in rows if r["patch"] not in wired]
    for row in stale_rows:
        failures.append(
            f"{row['patch']} has a marker row but no wired stanza - the table "
            f"is staler than mk/patches.mk. Run 'make regen-patch-markers'.")

    for row in rows:
        name = row["patch"]
        if name in missing_rows or name in {r["patch"] for r in stale_rows}:
            continue
        patch_path = os.path.join(args.patch_dir, name)
        try:
            with open(patch_path, "rb") as fh:
                digest = hashlib.sha256(fh.read()).hexdigest()
        except OSError:
            failures.append(f"{name}: patch file {patch_path} is missing.")
            continue
        if digest != row["sha256"]:
            failures.append(
                f"{name} changed since the marker table was derived - its "
                f"marker may no longer exist. Run 'make regen-patch-markers'.")
            continue
        target = f"{dirs[row['dir']]}/{row['file']}"
        try:
            with open(target, encoding="utf-8", errors="replace") as fh:
                content = fh.read()
        except OSError:
            failures.append(f"{name}: target file {target} does not exist.")
            continue
        present = row["marker"] in content
        wants = row["kind"] == "+"
        if present != wants:
            verb = "is missing from" if wants else "is back in"
            consequence = f" {row['note']}" if row["note"] else ""
            failures.append(
                f"{name} ({row['label']}){consequence} Its marker {verb} "
                f"{target}: the checkout does not carry this patch. "
                f"Run 'make reapply-patches'.")

    if failures:
        for line in failures:
            print(f"✗ {line}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
