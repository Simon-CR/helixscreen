# SPDX-License-Identifier: GPL-3.0-or-later
"""
Unit tests for strip_phase_tracking.py.

Run with: pytest tests/test_strip_phase_tracking.py -v
"""

import os
import shutil
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import strip_phase_tracking as strip  # noqa: E402
from fixtures import pre_1_1_instrumentation as old_writer  # noqa: E402


BEGIN = "# <<< HELIX_TRACKING v2 >>>"
END = "# <<< /HELIX_TRACKING >>>"

VALID_BLOCK = f"    {BEGIN}\n    HELIX_PHASE_HOMING\n    {END}\n"


def write(path: Path, content: str):
    path.write_bytes(content.encode())


def read(path: Path) -> str:
    return path.read_bytes().decode()


# ============================================================================
# Matching contract (item 1)
# ============================================================================


class TestMarkerMatching:
    def test_no_markers_leaves_file_untouched_and_makes_no_backup(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n"
        write(cfg, original)

        result = strip.main([str(tmp_path)])

        assert result == 0
        assert read(cfg) == original
        assert list(tmp_path.iterdir()) == [cfg]

    def test_a_comment_naming_both_markers_is_not_a_marker(self, tmp_path):
        # A line that merely contains marker text - copied into a comment,
        # say - is not itself a marker, so nothing about it looks anomalous
        # either; the file simply has nothing to strip.
        cfg = tmp_path / "printer.cfg"
        original = (
            f"# See {BEGIN} and {END} in the docs\n"
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n"
        )
        write(cfg, original)

        result = strip.main([str(tmp_path)])

        assert result == 0
        assert read(cfg) == original
        assert list(tmp_path.iterdir()) == [cfg]

    def test_a_comment_naming_one_marker_beside_a_real_block_only_touches_the_block(
        self, tmp_path
    ):
        cfg = tmp_path / "printer.cfg"
        original = (
            f"# old marker was {BEGIN}\n"
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n" + VALID_BLOCK
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0

        stripped = read(cfg)
        assert BEGIN not in stripped.split("\n", 1)[1]  # the real block is gone
        assert stripped.startswith(f"# old marker was {BEGIN}\n")  # the comment survives

    def test_unmatched_begin_is_left_untouched_with_no_backup(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = f"[gcode_macro PRINT_START]\ngcode:\n    G28\n    {BEGIN}\n    HELIX_PHASE_HOMING\n"
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == original
        assert list(tmp_path.iterdir()) == [cfg]

    def test_end_with_no_begin_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = f"[gcode_macro PRINT_START]\ngcode:\n    G28\n    HELIX_PHASE_HOMING\n    {END}\n"
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == original

    def test_nested_begin_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            f"[gcode_macro PRINT_START]\ngcode:\n    {BEGIN}\n    {BEGIN}\n"
            f"    HELIX_PHASE_HOMING\n    {END}\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == original

    def test_a_count_balanced_hand_edit_is_caught_by_shape_not_just_counts(self, tmp_path):
        # BEGIN of the first block and END of a second block were deleted by
        # hand: two BEGINs and two ENDs total, but the pairing is broken, and
        # a naive count comparison alone would call this well-formed.
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n"
            "    HELIX_PHASE_HOMING\n"  # orphaned - its BEGIN was deleted
            f"    {END}\n"
            "    QUAD_GANTRY_LEVEL\n"
            f"    {BEGIN}\n"  # orphaned - its END was deleted
            "    HELIX_PHASE_QGL\n"
            "    M109 S{EXTRUDER_TEMP}\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == original
        assert not list(tmp_path.glob("*.bak.*"))

    def test_a_block_outside_a_gcode_macro_body_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        # A real-shaped block sitting between two sections, in nobody's body.
        original = "[printer]\nkinematics: corexy\n\n" + VALID_BLOCK + "\n[extruder]\nstep_pin: PA1\n"
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == original

    def test_a_block_of_the_wrong_length_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            f"[gcode_macro PRINT_START]\ngcode:\n    {BEGIN}\n"
            "    HELIX_PHASE_HOMING\n    G28\n"
            f"    {END}\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == original

    def test_a_block_calling_something_other_than_a_phase_macro_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = f"[gcode_macro PRINT_START]\ngcode:\n    {BEGIN}\n    G28\n    {END}\n"
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == original

    def test_crlf_markers_are_recognized_and_crlf_is_preserved(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\r\n"
            "gcode:\r\n"
            "    G28\r\n"
            f"    {BEGIN}\r\n"
            "    HELIX_PHASE_HOMING\r\n"
            f"    {END}\r\n"
            "    M109 S{EXTRUDER_TEMP}\r\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        result = read(cfg)
        assert BEGIN not in result
        assert "\r\n" in result
        assert result == "[gcode_macro PRINT_START]\r\ngcode:\r\n    G28\r\n    M109 S{EXTRUDER_TEMP}\r\n"

    def test_two_blocks_in_one_file_both_removed(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\ngcode:\n    G28\n"
            + VALID_BLOCK
            + "    M190 S{BED_TEMP}\n"
            f"    {BEGIN}\n    HELIX_PHASE_HEATING_BED\n    {END}\n"
            "    M109 S{EXTRUDER_TEMP}\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        result = read(cfg)
        assert BEGIN not in result and END not in result
        assert result == (
            "[gcode_macro PRINT_START]\ngcode:\n    G28\n    M190 S{BED_TEMP}\n"
            "    M109 S{EXTRUDER_TEMP}\n"
        )

    def test_content_outside_removed_blocks_is_byte_identical(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n"
            + VALID_BLOCK
            + "    M190 S{BED_TEMP}\n"
            "      ; oddly indented comment\n"
            "    M109 S{EXTRUDER_TEMP}\n"
            "\n"
            "    BED_MESH_CALIBRATE\n"
        )
        write(cfg, original)
        expected = (
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n"
            "    M190 S{BED_TEMP}\n"
            "      ; oddly indented comment\n"
            "    M109 S{EXTRUDER_TEMP}\n"
            "\n"
            "    BED_MESH_CALIBRATE\n"
        )

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == expected

    def test_a_file_with_no_trailing_newline_keeps_it_that_way(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK.rstrip("\n")
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        result = read(cfg)
        assert result == "[gcode_macro PRINT_START]\ngcode:\n    G28\n"

    def test_running_the_strip_twice_is_harmless(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        after_first = read(cfg)
        backups_after_first = list(tmp_path.glob("*.bak.*"))

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == after_first
        assert list(tmp_path.glob("*.bak.*")) == backups_after_first


# ============================================================================
# File discovery (item 2)
# ============================================================================


class TestFileDiscovery:
    def test_a_macro_in_a_subdirectory_is_found_and_stripped(self, tmp_path):
        (tmp_path / "macros").mkdir()
        top = tmp_path / "printer.cfg"
        write(top, "[include macros/*.cfg]\n[printer]\nkinematics: corexy\n")
        sub = tmp_path / "macros" / "print_start.cfg"
        write(sub, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        assert strip.main([str(tmp_path)]) == 0
        assert BEGIN not in read(sub)
        assert read(top) == "[include macros/*.cfg]\n[printer]\nkinematics: corexy\n"

    def test_save_config_snapshot_is_skipped(self, tmp_path):
        snapshot = tmp_path / "printer-20260101_120000.cfg"
        write(snapshot, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)
        original = read(snapshot)

        assert strip.main([str(tmp_path)]) == 0
        assert read(snapshot) == original
        assert not list(tmp_path.glob("*.bak.*"))

    def test_a_symlinked_cfg_is_dereferenced_and_the_real_target_stripped(self, tmp_path):
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        real_file = real_dir / "printer.cfg"
        write(real_file, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        config_dir = tmp_path / "config"
        config_dir.mkdir()
        link = config_dir / "printer.cfg"
        link.symlink_to(real_file)

        assert strip.main([str(config_dir)]) == 0

        assert link.is_symlink()
        assert os.readlink(link) == str(real_file)
        assert BEGIN not in real_file.read_text()

    def test_two_paths_to_the_same_real_file_are_only_edited_once(self, tmp_path):
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        real_file = real_dir / "printer.cfg"
        write(real_file, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        config_dir = tmp_path / "config"
        config_dir.mkdir()
        (config_dir / "a.cfg").symlink_to(real_file)
        (config_dir / "b.cfg").symlink_to(real_file)

        assert strip.main([str(config_dir)]) == 0
        assert len(list(real_dir.glob("*.bak.*"))) == 1


# ============================================================================
# Safe writes (item 3)
# ============================================================================


class TestSafeWrites:
    def test_backup_and_target_mode_are_preserved_0444(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)
        cfg.chmod(0o444)

        try:
            assert strip.main([str(tmp_path)]) == 0
            assert stat.S_IMODE(cfg.stat().st_mode) == 0o444
            backups = list(tmp_path.glob("*.bak.*"))
            assert len(backups) == 1
            assert stat.S_IMODE(backups[0].stat().st_mode) == 0o444
        finally:
            cfg.chmod(0o644)  # tmp_path cleanup needs write access

    def test_mode_0600_is_preserved(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)
        cfg.chmod(0o600)

        assert strip.main([str(tmp_path)]) == 0
        assert stat.S_IMODE(cfg.stat().st_mode) == 0o600

    def test_a_read_only_directory_fails_cleanly_without_touching_the_file(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)
        tmp_path.chmod(0o555)

        try:
            result = strip.main([str(tmp_path)])
        finally:
            tmp_path.chmod(0o755)  # restore so pytest can clean up tmp_path

        assert result == 1
        assert read(cfg) == original
        assert not list(tmp_path.glob("*.bak.*"))
        assert not list(tmp_path.glob(".helix-tracking-strip-*"))

    def test_a_write_failure_leaves_the_original_and_backup_intact(self, tmp_path, monkeypatch):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)

        real_fsync = os.fsync

        def failing_fsync(fd):
            raise OSError("simulated disk-full during fsync")

        monkeypatch.setattr(os, "fsync", failing_fsync)
        try:
            result = strip.main([str(tmp_path)])
        finally:
            monkeypatch.setattr(os, "fsync", real_fsync)

        assert result == 1
        assert read(cfg) == original
        backups = list(tmp_path.glob("*.bak.*"))
        assert len(backups) == 1
        assert read(backups[0]) == original
        assert not list(tmp_path.glob(".helix-tracking-strip-*"))

    def test_a_post_write_verification_mismatch_restores_from_backup(self, tmp_path, monkeypatch):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)

        real_replace = os.replace
        calls = []

        def corrupting_replace(src, dst):
            # Simulate the replace landing, but the destination's bytes not
            # matching what was verified in the temp file (e.g. another
            # process wrote in between).
            calls.append((src, dst))
            with open(src, "ab") as f:
                f.write(b"CORRUPT")
            real_replace(src, dst)

        monkeypatch.setattr(os, "replace", corrupting_replace)
        try:
            result = strip.main([str(tmp_path)])
        finally:
            monkeypatch.setattr(os, "replace", real_replace)

        assert result == 1
        assert calls  # the replace path was actually exercised
        assert read(cfg) == original  # restored from the verified backup


# ============================================================================
# Output contract (item 4)
# ============================================================================


class TestOutputContract:
    def test_summary_counts_edited_skipped_and_failed(self, tmp_path, capsys):
        clean = tmp_path / "clean.cfg"
        write(clean, "[printer]\nkinematics: corexy\n")

        edited = tmp_path / "edited.cfg"
        write(edited, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        anomaly = tmp_path / "anomaly.cfg"
        write(
            anomaly,
            f"[gcode_macro PRINT_START]\ngcode:\n    G28\n    {BEGIN}\n    HELIX_PHASE_HOMING\n",
        )

        result = strip.main([str(tmp_path)])
        out = capsys.readouterr().out

        assert result == 0
        assert "edited 1, skipped 1, failed 0" in out
        assert str(edited) in out
        assert str(anomaly) in out
        assert "Klipper restart" in out

    def test_no_edits_prints_no_restart_line(self, tmp_path, capsys):
        write(tmp_path / "clean.cfg", "[printer]\nkinematics: corexy\n")

        strip.main([str(tmp_path)])
        out = capsys.readouterr().out

        assert "Klipper restart" not in out
        assert "edited 0, skipped 0, failed 0" in out

    def test_missing_config_dir_is_not_a_failure(self, tmp_path):
        assert strip.main([str(tmp_path / "does-not-exist")]) == 0


# ============================================================================
# Cross-check against the vendored pre-1.1 writer's own disable logic
# ============================================================================
#
# fixtures/pre_1_1_instrumentation.py is that writer's marker format and
# body-splicing logic, taken as-is with the Moonraker component plumbing
# trimmed away - not a git lookup, so this runs the same way everywhere,
# including with no git history at all.


def _write_helix_macros_cfg(config_dir):
    # The old enable refuses to instrument PRINT_START unless it can already
    # see the HELIX_PHASE_* macros defined somewhere in config_dir.
    repo_root = Path(__file__).resolve().parents[2]
    macros_cfg = repo_root / "assets" / "config" / "helix_macros.cfg"
    (config_dir / "helix_macros.cfg").write_bytes(macros_cfg.read_bytes())


def test_matches_the_old_writers_own_disable_output(tmp_path):
    _write_helix_macros_cfg(tmp_path)
    cfg = tmp_path / "printer.cfg"
    write(
        cfg,
        "[gcode_macro PRINT_START]\n"
        "gcode:\n"
        "    G28\n"
        "    QUAD_GANTRY_LEVEL\n"
        "    BED_MESH_CALIBRATE\n"
        "    M109 S{EXTRUDER_TEMP}\n"
        "    CLEAN_NOZZLE\n",
    )

    enable_result = old_writer.enable(tmp_path)
    assert enable_result["success"] is True

    old_disable_input = cfg.read_bytes()
    disable_result = old_writer.disable(tmp_path)
    assert disable_result["success"] is True
    old_disable_output = cfg.read_bytes()

    # Reset to the instrumented state and run OUR strip over it instead.
    cfg.write_bytes(old_disable_input)
    assert strip.main([str(tmp_path)]) == 0

    assert cfg.read_bytes() == old_disable_output


def test_matches_the_old_writer_with_no_git_available(tmp_path, monkeypatch):
    # The oracle above must not depend on git being reachable at all - there
    # is no subprocess/git call left in this file to disable, so this proves
    # it by removing every git from PATH and repeating the same check.
    monkeypatch.setenv("PATH", str(tmp_path / "empty-path-for-this-test"))
    (tmp_path / "empty-path-for-this-test").mkdir()
    assert shutil.which("git") is None

    config_dir = tmp_path / "config"
    config_dir.mkdir()
    _write_helix_macros_cfg(config_dir)
    cfg = config_dir / "printer.cfg"
    write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n    M109 S{EXTRUDER_TEMP}\n")

    assert old_writer.enable(config_dir)["success"] is True
    old_disable_input = cfg.read_bytes()
    assert old_writer.disable(config_dir)["success"] is True
    old_disable_output = cfg.read_bytes()

    cfg.write_bytes(old_disable_input)
    assert strip.main([str(config_dir)]) == 0
    assert cfg.read_bytes() == old_disable_output


def test_correctly_strips_a_pre_1268_legacy_instrumented_body(tmp_path):
    # The legacy writer deepens the WHOLE body by four spaces on every
    # write, enable or disable, independently of what strip_phase_tracking.py
    # does - so byte-equivalence to that writer's own disable is not the
    # right check here (its disable would re-deepen the body again, which
    # this script correctly never does). What must hold is that every valid
    # marker block a real pre-#1268 install wrote is still recognized and
    # removed, and every substantive line survives.
    _write_helix_macros_cfg(tmp_path)
    cfg = tmp_path / "printer.cfg"
    write(
        cfg,
        "[gcode_macro PRINT_START]\n"
        "gcode:\n"
        "    G28\n"
        "    QUAD_GANTRY_LEVEL\n"
        "    BED_MESH_CALIBRATE\n"
        "    M109 S{EXTRUDER_TEMP}\n"
        "    CLEAN_NOZZLE\n",
    )

    assert old_writer.enable_legacy(tmp_path)["success"] is True
    instrumented = cfg.read_text()
    assert old_writer.TRACKING_MARKER_BEGIN in instrumented

    assert strip.main([str(tmp_path)]) == 0
    stripped = cfg.read_text()

    assert old_writer.TRACKING_MARKER_BEGIN not in stripped
    assert old_writer.TRACKING_MARKER_END not in stripped
    assert "HELIX_PHASE_" not in stripped
    assert "HELIX_READY" not in stripped
    for original_line in (
        "G28",
        "QUAD_GANTRY_LEVEL",
        "BED_MESH_CALIBRATE",
        "M109 S{EXTRUDER_TEMP}",
        "CLEAN_NOZZLE",
    ):
        assert original_line in stripped


def test_a_legacy_body_with_a_blank_line_still_strips_leaving_its_duplicated_tail(tmp_path):
    # The legacy writer's write side only replaces the body up to its first
    # blank line, so a blank-line body gets a duplicated, un-instrumented
    # tail after that line - a pre-existing writer bug, unrelated to this
    # strip. Every block the writer did inject sits inside the same
    # [gcode_macro ...] body span as the duplicated tail (nothing else
    # follows in this file), so they are still well-formed and get removed;
    # the duplicate itself was never marker-delimited, so it survives too.
    _write_helix_macros_cfg(tmp_path)
    cfg = tmp_path / "printer.cfg"
    write(
        cfg,
        "[gcode_macro PRINT_START]\n"
        "gcode:\n"
        "    G28\n"
        "\n"
        "    QUAD_GANTRY_LEVEL\n"
        "    M109 S{EXTRUDER_TEMP}\n",
    )

    assert old_writer.enable_legacy(tmp_path)["success"] is True
    assert old_writer.TRACKING_MARKER_BEGIN in cfg.read_text()

    assert strip.main([str(tmp_path)]) == 0
    stripped = cfg.read_text()

    assert old_writer.TRACKING_MARKER_BEGIN not in stripped
    assert "HELIX_PHASE_" not in stripped
    assert "HELIX_READY" not in stripped
    # The duplicated, un-instrumented tail the legacy writer left behind
    # is untouched: QUAD_GANTRY_LEVEL and M109 each still appear twice.
    assert stripped.count("QUAD_GANTRY_LEVEL") == 2
    assert stripped.count("M109 S{EXTRUDER_TEMP}") == 2
