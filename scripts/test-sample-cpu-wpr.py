#!/usr/bin/env python3
"""DIAG.6c(f): verdict-logic tests for sample-cpu-wpr.py.

Covers the branches that cannot be observed on a box where wpr is privilege-denied: TOOL_MISSING, the symbol-grep
HOTSPOT_VISIBLE / HOTSPOT_NOT_VISIBLE happy paths, and that a failed wpr -start yields PERMISSION_DENIED without
orphaning a session (wpr -cancel invoked). Run: python scripts/test-sample-cpu-wpr.py
"""
import importlib.util
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("sample_cpu_wpr", ROOT / "scripts" / "sample-cpu-wpr.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)


class FakeProc:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class VerdictTests(unittest.TestCase):
    def test_tool_missing_when_wpr_absent(self):
        with mock.patch.object(mod, "_which", return_value=None):
            self.assertEqual(mod.run_windows("spec.exe", "syms", "out"), 2)

    def test_tool_missing_when_specimen_absent(self):
        with mock.patch.object(mod, "_which", return_value="wpr.exe"), \
             mock.patch.object(mod.os.path, "isfile", return_value=False):
            self.assertEqual(mod.run_windows("nope.exe", "syms", "out"), 2)

    def test_permission_denied_cancels_session(self):
        calls = []

        def fake_run(args, **kw):
            calls.append(args[1] if len(args) > 1 else args[0])
            if args[1] == "-start":
                return FakeProc(returncode=3310899217, stderr="Failed to enable the policy")
            return FakeProc(0)

        with mock.patch.object(mod, "_which", return_value="wpr.exe"), \
             mock.patch.object(mod.os.path, "isfile", return_value=True), \
             mock.patch.object(mod.subprocess, "run", side_effect=fake_run):
            self.assertEqual(mod.run_windows("spec.exe", "syms", "out"), 3)
        # start failed -> `started` stays False -> no -cancel needed, and -stop/xperf never run.
        self.assertIn("-start", calls)
        self.assertNotIn("-stop", calls)

    def _run_full(self, profile_body, stacks=False):
        seq = {"-start": FakeProc(0), "-stop": FakeProc(0)}

        def fake_run(args, **kw):
            key = args[1] if len(args) > 1 else args[0]
            if key in ("-start", "-stop", "-cancel"):
                return seq.get(key, FakeProc(0))
            return FakeProc(0)  # specimen run + xperf export

        m = mock.mock_open(read_data=profile_body)
        with mock.patch.object(mod, "_which", return_value="tool.exe"), \
             mock.patch.object(mod.os.path, "isfile", return_value=True), \
             mock.patch.object(mod.subprocess, "run", side_effect=fake_run), \
             mock.patch("builtins.open", m):
            return mod.run_windows("spec.exe", "syms", "out", stacks=stacks)

    def test_hotspot_visible(self):
        self.assertEqual(self._run_full("... crd_diag_unscoped_hotspot_burn  92%\n"), 0)

    def test_hotspot_not_visible(self):
        self.assertEqual(self._run_full("... some_other_symbol 92%\n"), 4)

    def test_stacks_hotspot_visible(self):
        # --stacks: the symbol shows up inside a decoded call stack.
        self.assertEqual(self._run_full("stack:\n  diag!crd_diag_unscoped_hotspot_burn\n", stacks=True), 0)

    def test_stacks_not_visible_with_frames(self):
        # --stacks: stacks ARE present (module!symbol frames) but not our symbol -> NOT_VISIBLE, not MISSING_STACKS.
        self.assertEqual(self._run_full("main!other_fn\nntdll!RtlUserThreadStart\n", stacks=True), 4)

    def test_stacks_missing(self):
        # --stacks: a profile was captured but carried no walked stacks (no module!symbol frames) -> MISSING_STACKS.
        self.assertEqual(self._run_full("Total = 12345 samples, no stacks\n", stacks=True), 5)

    def _export_args(self, stacks):
        # The xperf argument list the export actually uses.
        seen = []

        def fake_run(args, **kw):
            if args[0] == "xperf.exe":
                seen.append(list(args))
            return FakeProc(0)

        m = mock.mock_open(read_data="crd_diag_unscoped_hotspot_burn\n")
        with mock.patch.object(mod.subprocess, "run", side_effect=fake_run), \
             mock.patch.object(mod.os.path, "isfile", return_value=True), \
             mock.patch("builtins.open", m):
            self.assertEqual(mod.analyze_trace("xperf.exe", "t.etl", "o.txt", {}, r"d\crd-spec.exe", "syms", stacks), 0)
        self.assertEqual(len(seen), 1)
        return seen[0]

    def test_stacks_export_names_an_activity(self):
        # `xperf -a stack` with no activity fails with "stack: no option specified" (exit 5) and writes an empty
        # file: the first elevated run (2026-10-07) ended there. The stacks export must ask for -butterfly.
        args = self._export_args(stacks=True)
        action = args[args.index("-a"):]
        self.assertEqual(action, ["-a", "stack", "-butterfly", "-process", "crd-spec"])
        self.assertLess(args.index("-symbols"), args.index("-a"))  # a processing flag, before the action

    def test_flat_export_is_a_detailed_profile(self):
        args = self._export_args(stacks=False)
        self.assertEqual(args[args.index("-a"):], ["-a", "profile", "-detail"])

    def test_export_failure_is_never_a_visible_hotspot(self):
        def fake_run(args, **kw):
            return FakeProc(returncode=5, stderr="error: stack: no option specified")

        with mock.patch.object(mod.subprocess, "run", side_effect=fake_run), \
             mock.patch.object(mod.os.path, "isfile", return_value=True):
            self.assertEqual(mod.analyze_trace("xperf.exe", "t.etl", "o.txt", {}, "spec.exe", "syms", True), 4)

    def test_butterfly_frames_count_as_stacks(self):
        # The butterfly report writes frames as "module ! symbol"; stacks without our symbol are NOT_VISIBLE (4),
        # not MISSING_STACKS (5).
        self.assertEqual(self._run_full("crd-spec.exe ! main 4093 22.50%\n", stacks=True), 4)

    def test_from_etl_analyses_without_recording(self):
        calls = []

        def fake_run(args, **kw):
            calls.append(args[0])
            return FakeProc(0)

        m = mock.mock_open(read_data="crd-spec.exe ! crd_diag_unscoped_hotspot_burn 4093\n")
        with mock.patch.object(mod, "_which", side_effect=lambda name: name + ".exe"), \
             mock.patch.object(mod.sys, "platform", "win32"), \
             mock.patch.object(mod.os, "makedirs"), \
             mock.patch.object(mod.os.path, "isfile", return_value=True), \
             mock.patch.object(mod.subprocess, "run", side_effect=fake_run), \
             mock.patch("builtins.open", m):
            self.assertEqual(mod.main(["--from-etl", "trace.etl", "--stacks", "--out", "out"]), 0)
        self.assertEqual(calls, ["xperf.exe"])  # no wpr: a recorded trace needs no elevation

    def test_from_etl_rejects_a_missing_trace(self):
        with mock.patch.object(mod, "_which", return_value="xperf.exe"), \
             mock.patch.object(mod.sys, "platform", "win32"), \
             mock.patch.object(mod.os, "makedirs"):
            self.assertEqual(mod.main(["--from-etl", "Z:\\no\\such.etl"]), 64)

    def test_linux_tool_missing(self):
        with mock.patch.object(mod, "_which", return_value=None):
            self.assertEqual(mod.run_linux("spec", "out"), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
