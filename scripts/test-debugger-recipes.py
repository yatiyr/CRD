#!/usr/bin/env python3
"""DIAG.6c(j): verdict-logic tests for debugger-recipes.py.

Covers the verdicts the box's own state cannot exercise live (lldb is present but unrunnable here): TOOL_MISSING,
DEBUGGER_UNRUNNABLE (with the 0xC0000135 classification), and the SYMBOLS_RESOLVED / SYMBOLS_MISSING batch outcomes.
Run: python scripts/test-debugger-recipes.py
"""
import importlib.util
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("debugger_recipes", ROOT / "scripts" / "debugger-recipes.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)


class FakeProc:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class DebuggerVerdictTests(unittest.TestCase):
    def test_tool_missing(self):
        with mock.patch.object(mod, "_lldb_path", return_value=None):
            self.assertEqual(mod.probe_lldb("spec.exe"), 2)

    def test_debugger_unrunnable_dll_not_found(self):
        with mock.patch.object(mod, "_lldb_path", return_value="lldb.exe"), \
             mock.patch.object(mod.subprocess, "run", return_value=FakeProc(returncode=-1073741515)):
            self.assertEqual(mod.probe_lldb("spec.exe"), 3)

    def test_classify_dll_not_found_mentions_python(self):
        msg = mod.classify_launch_failure(3221225781)  # unsigned form of 0xC0000135
        self.assertIn("0xC0000135", msg)
        self.assertIn("python310", msg)

    def test_symbols_resolved(self):
        def fake_run(args, **kw):
            if "--version" in args:
                return FakeProc(0)
            return FakeProc(0, stdout=f"frame #0 {mod.HOT_SYMBOL} at cpu_hotspot_specimen.cpp:20\n")

        with mock.patch.object(mod, "_lldb_path", return_value="lldb.exe"), \
             mock.patch.object(mod.os.path, "isfile", return_value=True), \
             mock.patch.object(mod.subprocess, "run", side_effect=fake_run):
            self.assertEqual(mod.probe_lldb("spec.exe"), 0)

    def test_symbols_missing(self):
        def fake_run(args, **kw):
            if "--version" in args:
                return FakeProc(0)
            return FakeProc(0, stdout="frame #0 0x00007ff6`00001234\n")  # no symbol

        with mock.patch.object(mod, "_lldb_path", return_value="lldb.exe"), \
             mock.patch.object(mod.os.path, "isfile", return_value=True), \
             mock.patch.object(mod.subprocess, "run", side_effect=fake_run):
            self.assertEqual(mod.probe_lldb("spec.exe"), 4)

    def test_available_when_version_ok_no_batch(self):
        with mock.patch.object(mod, "_lldb_path", return_value="lldb.exe"), \
             mock.patch.object(mod.subprocess, "run", return_value=FakeProc(0)):
            self.assertEqual(mod.probe_lldb("spec.exe", run_batch=False), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
