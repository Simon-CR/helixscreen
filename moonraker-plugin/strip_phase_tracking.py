#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Strip HELIX_TRACKING marker blocks from a Klipper config tree.

HelixScreen versions before 1.1 could instrument a PRINT_START macro with
calls to HELIX_PHASE_*/HELIX_READY, each call wrapped in a pair of marker
comment lines. This script undoes that: it walks a config directory, finds
every well-formed marker block sitting inside a [gcode_macro ...] body, and
removes it, backing up each file it edits first.

A file with any marker anomaly (unmatched or nested markers, a marker line
outside a gcode body, a block that is not exactly the three lines the
instrumentation ever wrote) is left byte-for-byte untouched; only its path
and the reason are reported. Nothing here restarts Klipper - the caller is
told the edit takes effect at the next restart.

Usage: strip_phase_tracking.py CONFIG_DIR
Exit status: 0 if no file's edit failed (files may still have been skipped
as anomalous - that is not a failure), 1 if any file's edit failed.
"""

import argparse
import os
import re
import shutil
import sys
import tempfile

TRACKING_MARKER_BEGIN = b"# <<< HELIX_TRACKING v2 >>>"
TRACKING_MARKER_END = b"# <<< /HELIX_TRACKING >>>"

# The only line _instrument_gcode ever wrote between the markers: a bare
# HELIX_PHASE_* or HELIX_READY call, no arguments.
_MACRO_CALL_RE = re.compile(rb"^[ \t]*(HELIX_PHASE_[A-Z_]+|HELIX_READY)[ \t]*$")

_SECTION_HEADER_RE = re.compile(rb"^\[gcode_macro\s+[^\]]+\]", re.IGNORECASE)

# Klipper's own SAVE_CONFIG snapshots, e.g. "printer-20260101_120000.cfg" -
# point-in-time history that a phase-tracking strip should never touch.
_SNAPSHOT_NAME_RE = re.compile(r"^.+-\d{8}_\d{6}\.cfg$")


class StripAnomaly(Exception):
    """A file's markers don't match the one shape the instrumentation wrote."""


class StripWriteError(Exception):
    """A backup or write step failed; the original is left untouched."""


class StripResult:
    def __init__(self, status, new_bytes=None, reason=None, blocks=0):
        self.status = status  # "clean" | "anomaly" | "stripped"
        self.new_bytes = new_bytes
        self.reason = reason
        self.blocks = blocks


def _find_marker_lines(lines):
    """Return [(index, "BEGIN"|"END"), ...] for every line that is, after
    trimming surrounding whitespace (CRLF tolerated), exactly one marker.
    A line that merely contains marker text is not a marker."""
    result = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped == TRACKING_MARKER_BEGIN:
            result.append((i, "BEGIN"))
        elif stripped == TRACKING_MARKER_END:
            result.append((i, "END"))
    return result


def _pair_markers(marker_lines):
    """Walk marker lines in file order, enforcing BEGIN then END before the
    next BEGIN, with no nesting. Returns [(begin_idx, end_idx), ...]."""
    pairs = []
    open_begin = None
    for idx, kind in marker_lines:
        if kind == "BEGIN":
            if open_begin is not None:
                raise StripAnomaly(f"nested BEGIN marker at line {idx + 1}")
            open_begin = idx
        else:
            if open_begin is None:
                raise StripAnomaly(f"END marker with no BEGIN at line {idx + 1}")
            pairs.append((open_begin, idx))
            open_begin = None
    if open_begin is not None:
        raise StripAnomaly(f"unmatched BEGIN marker at line {open_begin + 1}")
    return pairs


def _find_gcode_macro_body_spans(lines):
    """Yield (start, end) exclusive line-index ranges for each
    [gcode_macro ...] section's gcode: body.

    Mirrors how Klipper itself finds a macro's body: a body runs from the
    line after "gcode:" until a non-blank line starts in column 0, and a new
    section header ends the current one outright, with or without a body.
    """
    spans = []
    i = 0
    n = len(lines)
    while i < n:
        if not _SECTION_HEADER_RE.match(lines[i]):
            i += 1
            continue
        i += 1
        body_start = None
        while i < n:
            cur = lines[i]
            if cur.lstrip(b" \t").startswith(b"["):
                break
            if body_start is None:
                if cur.strip().startswith(b"gcode:"):
                    body_start = i + 1
                i += 1
                continue
            if cur.strip() and not cur[:1].isspace():
                break
            i += 1
        if body_start is not None:
            spans.append((body_start, i))
        # i now sits on the line that ended the section (a new header, or
        # EOF); the outer loop re-examines it without advancing further.
    return spans


def _validate_blocks(lines, pairs, body_spans):
    for begin_idx, end_idx in pairs:
        if end_idx != begin_idx + 2:
            raise StripAnomaly(f"marker block at line {begin_idx + 1} is not exactly 3 lines")
        if not _MACRO_CALL_RE.match(lines[begin_idx + 1].rstrip(b"\r\n")):
            raise StripAnomaly(
                f"marker block at line {begin_idx + 1} does not call a "
                "HELIX_PHASE_* or HELIX_READY macro"
            )
        if not any(start <= begin_idx and end_idx < end for start, end in body_spans):
            raise StripAnomaly(
                f"marker block at line {begin_idx + 1} is outside a [gcode_macro ...] body"
            )


def compute_stripped_content(raw_bytes):
    """Decide what to do with one file's content.

    Returns a StripResult: "clean" (no markers, nothing to do), "anomaly"
    (a marker exists but the shape is wrong; `reason` explains why), or
    "stripped" (`new_bytes` is the content with every valid block removed).
    """
    lines = raw_bytes.splitlines(keepends=True)
    marker_lines = _find_marker_lines(lines)
    if not marker_lines:
        return StripResult("clean")

    try:
        pairs = _pair_markers(marker_lines)
        body_spans = _find_gcode_macro_body_spans(lines)
        _validate_blocks(lines, pairs, body_spans)
    except StripAnomaly as exc:
        return StripResult("anomaly", reason=str(exc))

    remove = set()
    for begin_idx, end_idx in pairs:
        remove.update(range(begin_idx, end_idx + 1))
    new_bytes = b"".join(line for i, line in enumerate(lines) if i not in remove)
    return StripResult("stripped", new_bytes=new_bytes, blocks=len(pairs))


def _timestamped_backup_path(real_path):
    import time

    return f"{real_path}.bak.{time.strftime('%Y%m%d_%H%M%S')}"


def safe_replace_file(path, new_bytes):
    """Back up `path`'s real target, verify it, write `new_bytes` through a
    verified temp file, then replace the target's content - never the
    symlink itself, its mode, or (where permitted) its owner.

    Raises StripWriteError on any failure; the original is left untouched
    and any temp file is removed.
    """
    real_path = os.path.realpath(path)

    try:
        orig_stat = os.stat(real_path)
        with open(real_path, "rb") as f:
            original_on_disk = f.read()
    except OSError as exc:
        raise StripWriteError(f"could not read {path}: {exc}") from exc

    backup_path = _timestamped_backup_path(real_path)
    try:
        shutil.copy2(real_path, backup_path)
        with open(backup_path, "rb") as f:
            backup_bytes = f.read()
    except OSError as exc:
        # copy2 can raise partway through (e.g. EFBIG on a full disk),
        # leaving a partial file at backup_path - never leave that behind.
        _remove_quiet(backup_path)
        raise StripWriteError(f"could not back up {path}: {exc}") from exc
    if backup_bytes != original_on_disk:
        _remove_quiet(backup_path)
        raise StripWriteError(f"backup of {path} did not verify")

    dir_name = os.path.dirname(real_path) or "."
    tmp_fd, tmp_path = tempfile.mkstemp(prefix=".helix-tracking-strip-", dir=dir_name)
    try:
        with os.fdopen(tmp_fd, "wb") as f:
            f.write(new_bytes)
            f.flush()
            os.fsync(f.fileno())
        with open(tmp_path, "rb") as f:
            written = f.read()
        if written != new_bytes:
            raise StripWriteError(f"write to a temp file for {path} did not verify")

        import stat as stat_module

        os.chmod(tmp_path, stat_module.S_IMODE(orig_stat.st_mode))
        try:
            os.chown(tmp_path, orig_stat.st_uid, orig_stat.st_gid)
        except (OSError, AttributeError):
            pass  # not permitted (non-root), or chown unavailable on this platform

        os.replace(tmp_path, real_path)
        tmp_path = None  # replaced; nothing left to clean up

        with open(real_path, "rb") as f:
            final_bytes = f.read()
        if final_bytes != new_bytes:
            shutil.copy2(backup_path, real_path)
            raise StripWriteError(f"{path} did not verify after writing; restored from backup")
    finally:
        if tmp_path is not None:
            _remove_quiet(tmp_path)

    return backup_path


def _remove_quiet(path):
    try:
        os.remove(path)
    except OSError:
        pass


def _is_snapshot_name(name):
    return bool(_SNAPSHOT_NAME_RE.match(name))


def find_cfg_files(scan_dir):
    """Every .cfg file under scan_dir, recursively, deduplicated by realpath
    and sorted for a deterministic processing order. Skips SAVE_CONFIG
    snapshot files and anything under a config_backups directory, and never
    re-descends into a directory it has already visited by realpath (a
    symlink cycle guard)."""
    real_paths = set()
    visited_dirs = set()
    for root, dirs, files in os.walk(scan_dir, followlinks=True):
        real_root = os.path.realpath(root)
        if real_root in visited_dirs:
            dirs[:] = []
            continue
        visited_dirs.add(real_root)
        dirs[:] = [d for d in dirs if d != "config_backups"]
        for name in files:
            if not name.endswith(".cfg") or _is_snapshot_name(name):
                continue
            real_paths.add(os.path.realpath(os.path.join(root, name)))
    return sorted(real_paths)


def process_file(path):
    try:
        with open(path, "rb") as f:
            original = f.read()
    except OSError as exc:
        return {"path": path, "status": "failed", "reason": f"could not read: {exc}"}

    result = compute_stripped_content(original)
    if result.status == "clean":
        return {"path": path, "status": "clean"}
    if result.status == "anomaly":
        return {"path": path, "status": "skipped", "reason": result.reason}

    try:
        backup_path = safe_replace_file(path, result.new_bytes)
    except (StripWriteError, OSError) as exc:
        # Any unhandled OS-level failure (a disk-full mid rename, for
        # example) is one file's failure, never a reason to stop processing
        # the rest of the tree or to claim success for this one.
        return {"path": path, "status": "failed", "reason": str(exc)}

    return {
        "path": path,
        "status": "edited",
        "backup": backup_path,
        "blocks": result.blocks,
    }


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config_dir", help="Klipper config directory to scan")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.config_dir):
        print(f"INFO: no config directory at {args.config_dir}; nothing to do")
        return 0

    edited, skipped, failed = [], [], []
    for path in find_cfg_files(args.config_dir):
        r = process_file(path)
        if r["status"] == "edited":
            edited.append(r)
        elif r["status"] == "skipped":
            skipped.append(r)
        elif r["status"] == "failed":
            failed.append(r)

    for r in edited:
        print(f"INFO: stripped phase-tracking instrumentation from {r['path']} "
              f"(backup: {r['backup']})")
    for r in skipped:
        print(f"WARN: left {r['path']} untouched: {r['reason']}")
    for r in failed:
        print(f"ERROR: failed to strip {r['path']}: {r['reason']}", file=sys.stderr)

    if edited:
        print("INFO: This takes effect at the next Klipper restart. "
              "Loaded macros keep working until then.")

    print(f"INFO: phase-tracking strip summary: edited {len(edited)}, "
          f"skipped {len(skipped)}, failed {len(failed)}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
