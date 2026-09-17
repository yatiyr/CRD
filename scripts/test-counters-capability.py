#!/usr/bin/env python3
"""DIAG.6c(h): verdict-logic tests for counters-capability.py.

Covers the pure classification (never-zero) logic and the branches a non-elevated box cannot observe live:
PMU parsing, AVAILABLE/UNAVAILABLE classification, the multiplexing decision, and that a denied capture yields
PERMISSION_DENIED without orphaning a kernel session. Run: python scripts/test-counters-capability.py
"""
import importlib.util
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("counters_capability", ROOT / "scripts" / "counters-capability.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

# A realistic `xperf -pmcsources` capture (trimmed): 9 concurrent, cache/branch sources present.
PMCSOURCES = """Maximum selectable profile sources: 9.

Id  Name                             Interval  Min      Max
--------------------------------------------------------------
  0 Timer                               10000  1221    1000000
  6 BranchInstructions                  65536  4096 2147483647
 10 CacheMisses                         65536  4096 2147483647
 11 BranchMispredictions                65536  4096 2147483647
 29 LLCMisses                           65536  4096 2147483647
"""

KF = "       CSWITCH             : Context Switch\n       HARD_FAULTS         : Hard Faults\n"


class FakeProc:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class CapabilityTests(unittest.TestCase):
    def test_parse_pmcsources(self):
        mx, sources = mod.parse_pmcsources(PMCSOURCES)
        self.assertEqual(mx, 9)
        self.assertIn("CacheMisses", sources)
        self.assertIn("BranchMispredictions", sources)
        self.assertIn("Timer", sources)  # parsed, but not a PMU keyword

    def test_classify_pmu_available(self):
        mx, sources = mod.parse_pmcsources(PMCSOURCES)
        state, _detail, pmu, _mx = mod.classify_pmu(mx, sources)
        self.assertEqual(state, "AVAILABLE")
        self.assertNotIn("Timer", pmu)          # Timer is not a cache/branch PMU counter
        self.assertIn("CacheMisses", pmu)

    def test_classify_pmu_unavailable_when_no_pmu(self):
        # Only Timer -> no cache/branch PMU -> UNAVAILABLE with a reason, never zero.
        state, detail, _pmu, _mx = mod.classify_pmu(9, ["Timer"])
        self.assertEqual(state, "UNAVAILABLE")
        self.assertIn("hypervisor", detail.lower())

    def test_classify_kernel_flag(self):
        self.assertEqual(mod.classify_kernel_flag(KF, "CSWITCH")[0], "AVAILABLE")
        self.assertEqual(mod.classify_kernel_flag(KF, "PMC_MISSING")[0], "UNAVAILABLE")

    def test_plan_pmu_capture(self):
        self.assertEqual(mod.plan_pmu_capture(4, 9), ("AVAILABLE", 0))
        self.assertEqual(mod.plan_pmu_capture(10, 9), ("MULTIPLEXING_REQUIRED", 5))
        self.assertEqual(mod.plan_pmu_capture(1, 0), ("UNAVAILABLE", 4))

    def test_pmu_concurrent_limit_reserves_timer_slot(self):
        # observed: 9 profile slots -> 8 concurrent hardware counters (one slot is the timer)
        self.assertEqual(mod.pmu_concurrent_limit(9), 8)
        self.assertEqual(mod.pmu_concurrent_limit(0), 0)

    def test_capture_pmu_default_reserves_timer_slot(self):
        seen = {}

        def fake_run(xperf, args):
            if args[0] == "-pmcsources":
                return FakeProc(0, stdout=PMCSOURCES)  # 4 PMU sources in the sample; limit 8
            if args[0] == "-on":
                seen["on"] = args
                return FakeProc(returncode=2147942405, stderr="Access is denied (0x5)")  # then hits the priv gate
            return FakeProc(0)

        with mock.patch.object(mod, "_run", side_effect=fake_run):
            self.assertEqual(mod.capture_class("xperf", "cache_branch_pmu", "out"), 3)
        self.assertIn("-PmcProfile", seen["on"])  # a valid (<=8) counter set was requested before the denial

    def test_capture_too_many_counters_is_multiplexing_not_denied(self):
        def fake_run(xperf, args):
            if args[0] == "-pmcsources":
                return FakeProc(0, stdout=PMCSOURCES)
            if args[0] == "-on":
                return FakeProc(returncode=2147549183, stderr="xperf: error: Too many counters specified (max=8)")
            return FakeProc(0)

        with mock.patch.object(mod, "_run", side_effect=fake_run):
            # xperf reports a count limit -> classified MULTIPLEXING_REQUIRED (5), never PERMISSION_DENIED (3)
            self.assertEqual(mod.capture_class("xperf", "cache_branch_pmu", "out", requested_pmu=8), 5)

    def test_capture_permission_denied_no_orphan(self):
        calls = []

        def fake_run(xperf, args):
            calls.append(args[0])
            if args[0] == "-on":
                return FakeProc(returncode=3310899217, stderr="Failed to enable the policy")
            return FakeProc(0)

        with mock.patch.object(mod, "_run", side_effect=fake_run):
            self.assertEqual(mod.capture_class("xperf", "context_switch", "out"), 3)
        self.assertIn("-on", calls)
        self.assertNotIn("-stop", calls)  # start failed -> nothing to stop, no orphan

    def test_capture_pmu_multiplexing(self):
        def fake_run(xperf, args):
            if args[0] == "-pmcsources":
                return FakeProc(0, stdout=PMCSOURCES)
            return FakeProc(0)

        with mock.patch.object(mod, "_run", side_effect=fake_run):
            # request more counters than the 9 concurrent max -> MULTIPLEXING_REQUIRED before any capture
            self.assertEqual(mod.capture_class("xperf", "cache_branch_pmu", "out", requested_pmu=12), 5)

    def test_capture_unknown_class(self):
        self.assertEqual(mod.capture_class("xperf", "nonsense", "out"), 64)


if __name__ == "__main__":
    unittest.main(verbosity=2)
