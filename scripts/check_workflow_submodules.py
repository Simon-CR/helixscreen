#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail when a workflow job runs a submodule-dependent target without checking out submodules.

The shell suite reads files out of the submodules. Most of those tests skip
themselves when a submodule is absent, but `tests/shell/test_lvgl_event_code_gate.bats`
copies `lib/lvgl/src/misc/lv_event.h` in `setup()`, so a bare checkout kills the
whole file instead of skipping it.

That is not hypothetical: `build.yml` was fixed for it in 83b0b9d51 and
`release.yml` was left behind, so the v0.99.117 tag failed `validate-shell` and
skipped every build, publish and deploy job behind it. Nothing was released and
the tag had to be moved.

The enum has to be PATCHED as well, not merely present: `lvgl_display_sync_cb.patch`
inserts four values and renumbers everything after them, so the committed worker
table only matches a patched checkout. Both steps are therefore required.

Every `make apply-patches` command, in any job, must run with
HELIX_PATCHES_FROM_CLEAN=1. A CI runner is always a fresh clone: nothing is
applied when the recipe starts, so every patch must take its apply branch, and a
patch that can neither apply nor reverse is dead, not shadowed. Without the flag
that case only warns and the job builds on with the patch silently missing.

The flag counts in either of two forms: on the make invocation itself, or as
`env: HELIX_PATCHES_FROM_CLEAN: "1"` at the workflow or job level. The env form
is the one a workflow should use, because the apply that matters usually runs
inside a plain `make -j` build step rather than a dedicated apply step - and a
flag pinned to one step never reaches that one.

Deliberately not solved by making the bats file skip on a missing header: that
would turn "the committed table is up to date" green-by-skip, which is the
failure this gate exists to prevent.
"""

import pathlib
import re
import sys

import yaml

WORKFLOW_DIR = pathlib.Path(".github/workflows")
INIT_ACTION = "./.github/actions/init-submodules"
FLAG_ENV = "HELIX_PATCHES_FROM_CLEAN"

# Ways a job can invoke the bats suite. release.yml goes through the make
# target; build.yml and nightly.yml call bats on the directory directly. Both
# read the submodules, so both have to be recognised - keying on only one form
# is how this gate would quietly cover a third of the jobs it is meant to.
INVOCATIONS = (
    (re.compile(r"\bmake\b[^\n;|&]*\btest-shell\b"), "make test-shell"),
    (re.compile(r"\bbats\b[^\n;|&]*\btests/shell\b"), "bats tests/shell/"),
)

# A run string is a list of commands, not one command: `make apply-patches; make
# apply-patches HELIX_PATCHES_FROM_CLEAN=1` flags only the second apply, and a
# flag anywhere in the string must not bless the rest of it. Split on every
# separator shell recognizes, then judge each command on its own.
COMMAND_SPLIT_RE = re.compile(r"[\n;|&]+")
MAKE_RE = re.compile(r"\bmake\b")
# `reapply-patches` is a different target that must not be excused by this rule;
# `force-apply-patches` is the apply target and must be. A plain \b cannot tell
# them apart, so anchor on the character before the target name: 'y' precedes
# the one ('reapply'), '-' the other ('force-').
APPLY_TARGET_RE = re.compile(r"(?<![a-zA-Z0-9])apply-patches\b")
FLAG_RE = re.compile(r"HELIX_PATCHES_FROM_CLEAN=1")


def steps_of(job):
    steps = job.get("steps")
    return steps if isinstance(steps, list) else []


def apply_commands(run):
    """Return the commands in `run` that invoke `make apply-patches`."""
    return [
        seg.strip()
        for seg in COMMAND_SPLIT_RE.split(run)
        if MAKE_RE.search(seg) and APPLY_TARGET_RE.search(seg)
    ]


def flag_covered_by_env(doc, job):
    """Does a workflow- or job-level `env:` set the from-clean flag?"""
    merged = {}
    for env in (doc.get("env"), job.get("env")):
        if isinstance(env, dict):
            merged.update(env)
    return str(merged.get(FLAG_ENV)) == "1"


def job_runs_shell_suite(steps):
    """Return how this job invokes the bats suite, or None if it does not."""
    for step in steps:
        run = step.get("run") or ""
        for pattern, label in INVOCATIONS:
            if pattern.search(run):
                return label
    return None


def job_has_init(steps):
    return any((step.get("uses") or "").strip() == INIT_ACTION for step in steps)


def job_has_apply_patches(steps):
    return any(apply_commands(step.get("run") or "") for step in steps)


def bare_apply_patches(steps, env_covered):
    """Return unflagged `make apply-patches` commands as (step name, command)."""
    bare = []
    for step in steps:
        name = step.get("name") or "unnamed step"
        for command in apply_commands(step.get("run") or ""):
            if not env_covered and not FLAG_RE.search(command):
                bare.append((name, command))
    return bare


def main():
    if not WORKFLOW_DIR.is_dir():
        print(f"❌ {WORKFLOW_DIR} not found", file=sys.stderr)
        return 1

    # removeprefix, not lstrip: lstrip("./") strips a *character set* and would
    # eat the dot in ".github" as well.
    if not (pathlib.Path(INIT_ACTION.removeprefix("./")) / "action.yml").is_file():
        print(f"❌ the composite action {INIT_ACTION} is missing", file=sys.stderr)
        return 1

    errors = []
    checked = 0

    for wf in sorted(WORKFLOW_DIR.glob("*.yml")):
        try:
            doc = yaml.safe_load(wf.read_text())
        except yaml.YAMLError as exc:
            errors.append(f"{wf}: cannot parse: {exc}")
            continue
        if not isinstance(doc, dict):
            continue

        for job_id, job in (doc.get("jobs") or {}).items():
            if not isinstance(job, dict):
                continue
            steps = steps_of(job)
            env_covered = flag_covered_by_env(doc, job)

            for step_name, command in bare_apply_patches(steps, env_covered):
                errors.append(
                    f"{wf.name}: job '{job_id}' step '{step_name}' runs `{command}` "
                    "without HELIX_PATCHES_FROM_CLEAN=1"
                    "\n    A CI runner is a fresh clone, so every patch must take its"
                    " apply branch; without the flag a drifted patch only warns and"
                    " the job builds on with the patch silently missing."
                    "\n    Pass HELIX_PATCHES_FROM_CLEAN=1 on the make invocation, or"
                    f" set `env: {FLAG_ENV}: \"1\"` at the workflow or job level - the"
                    " env form also covers applies that run inside a plain `make -j`."
                )

            invocation = job_runs_shell_suite(steps)
            if not invocation:
                continue

            checked += 1
            missing = []
            if not job_has_init(steps):
                missing.append(f"`uses: {INIT_ACTION}`")
            if not job_has_apply_patches(steps):
                missing.append("`run: make apply-patches`")
            if missing:
                errors.append(
                    f"{wf.name}: job '{job_id}' runs `{invocation}` but is missing "
                    + " and ".join(missing)
                    + "\n    A bare checkout has no lib/lvgl, so test_lvgl_event_code_gate.bats"
                    "\n    dies in setup() and takes every job behind it with it."
                )

    if errors:
        print("❌ workflow submodule gate:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1

    print(
        f"✅ workflow submodule gate: {checked} job(s) running the shell suite "
        "init and patch their submodules"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
