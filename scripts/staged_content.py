#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""What a `--staged-only` gate must read: the INDEX, not the working tree.

A `--staged-only` gate exists so the pre-commit hook checks what the commit
will actually contain. Picking the file list from `git diff --cached
--name-only` and then reading each path with `open()` / `Path.read_text()`
gets the SET right and the CONTENT wrong: that call reads whatever is on disk
right now, which is the staged blob only until something touches the working
tree again. Stage a violation, then revert the working copy to something
clean - the file list still names the file, but every gate built this way
reads the clean copy and reports nothing. The bad blob commits.

This module reads the staged blob instead, via one `git cat-file --batch`
process for every path a gate cares about, so nine gates stop hand-rolling
nine copies of the same subprocess dance.

    from staged_content import staged_files

    for path, text in staged_files(suffixes=('.bats',)):
        ...

`staged_files()` covers the common case: the set of files worth checking IS
the staged diff, and each one is a per-file rule (a duplicate name, a bare
`[[ ]]`, an unguarded subscript) that never needs to see a file that did not
change. That is every gate in this module's first set of callers.

A gate whose count is a WHOLE-TREE ratchet (check_hardcoded_pixels.py,
check_panel_widget_scrollable.py) needs a different shape: every file in the
tree the commit will produce, not just the changed ones, so an unrelated
file's pre-existing violations still count toward the baseline. Those two
build that tree with `git write-tree` + `ls-tree` + `cat-file --batch`
directly; the technique is the same read primitive as this module but the
file SET is a different question, so it is not duplicated here.
"""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Iterable, Iterator


def repo_root() -> Path:
    out = subprocess.run(
        ['git', 'rev-parse', '--show-toplevel'],
        capture_output=True, text=True, check=False,
    ).stdout.strip()
    return Path(out) if out else Path.cwd()


def staged_paths(root: Path | None = None, suffixes: tuple[str, ...] | None = None,
                  diff_filter: str = 'ACM') -> list[str]:
    """Relative paths staged for commit, as `git diff --cached` reports them.

    `diff_filter` defaults to Added/Copied/Modified - a path staged for
    deletion has no index entry to read, so callers that want to see those
    too (rename detection, say) pass a filter that includes it and are
    expected to cope with `read_index_blobs` skipping a path that vanished.
    """
    root = root or repo_root()
    out = subprocess.run(
        ['git', '-C', str(root), 'diff', '--cached', '--name-only',
         f'--diff-filter={diff_filter}'],
        capture_output=True, text=True, check=False,
    ).stdout
    paths = [p for p in out.splitlines() if p]
    if suffixes:
        paths = [p for p in paths if p.endswith(suffixes)]
    return paths


def read_index_blobs(paths: Iterable[str], root: Path | None = None) -> Iterator[tuple[str, str]]:
    """Yield (path, text) reading each path's STAGED content (index stage 0).

    One `git cat-file --batch` process streams every blob, keyed by the `:path`
    revision (index stage 0) - the pre-commit hook runs on every commit across
    many sessions, so a process-per-file cost here is a process-per-file cost
    on every one of them. A path with no index entry (staged for deletion, or
    a caller-supplied path that was never staged) reports "missing" and is
    skipped rather than yielded with stale disk content.
    """
    paths = list(paths)
    if not paths:
        return
    root = root or repo_root()
    proc = subprocess.Popen(
        ['git', '-C', str(root), 'cat-file', '--batch'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    )
    try:
        for p in paths:
            assert proc.stdin is not None
            proc.stdin.write(f':{p}\n'.encode())
        proc.stdin.close()
        assert proc.stdout is not None
        for p in paths:
            header = proc.stdout.readline().decode('utf-8', 'replace').split()
            # "<sha> blob <size>" - anything else (missing, a non-blob type)
            # has no bytes to read and nothing following it to skip.
            if len(header) < 3 or header[1] != 'blob':
                continue
            size = int(header[2])
            content = proc.stdout.read(size)
            proc.stdout.read(1)  # trailing newline after each blob
            yield (p, content.decode('utf-8', 'ignore'))
    finally:
        if proc.stdin is not None and not proc.stdin.closed:
            proc.stdin.close()
        proc.wait()


def staged_files(root: Path | None = None, suffixes: tuple[str, ...] | None = None,
                  diff_filter: str = 'ACM') -> Iterator[tuple[str, str]]:
    """Yield (path, text) for every staged file, content from the index.

    The set (`staged_paths`) and the content (`read_index_blobs`) a
    `--staged-only` gate wants, in one call.
    """
    root = root or repo_root()
    yield from read_index_blobs(staged_paths(root, suffixes, diff_filter), root)
