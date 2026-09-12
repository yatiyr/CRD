#!/usr/bin/env python3
"""test_gen_matrix.py -- CEIR-0g §4 step-4 generator checks. Run:
python tools/ceir_capability_matrix/test_gen_matrix.py  (stdlib unittest, no third-party deps)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_matrix as gm  # noqa: E402

ROOT = gm.repo_root()


def _feat(**over):
    """A complete minimal feature (all load-bearing fields present) + overrides."""
    f = dict(category="c", raf_level="n/a", ceir_level=5, providers=["gpu"],
             determinism_tier="", classification="B", status="gate-only", band="X",
             assets=[], tests_vulkan=[], tests_dx12=[], tests_cross_backend=["x"],
             hot_reload_tests=[])
    f.update(over)
    return f


class ChecksTests(unittest.TestCase):
    def _run(self, **over):
        return gm.check({"t": _feat(**over)}, ROOT)

    def test_clean_feature_no_errors(self):
        errors, _ = self._run()
        self.assertEqual(errors, [], errors)

    def test_providers_invariant_hard(self):
        # ceir_level 0 (not a CEIR program) with a provider is a HARD invariant violation
        errors, _ = self._run(ceir_level=0, providers=["gpu"])
        self.assertTrue(any("must be []" in e for e in errors), errors)

    def test_ceir_native_zero_providers_ok(self):
        errors, _ = self._run(ceir_level=0, providers=[])
        self.assertFalse(any("must be []" in e for e in errors), errors)

    def test_stale_asset_hard(self):
        errors, _ = self._run(assets=["assets/nope_xyz_does_not_exist.ceir"])
        self.assertTrue(any("stale ref" in e for e in errors), errors)

    def test_stale_test_path_hard(self):
        # a path-shaped token inside a tests_* string must resolve
        errors, _ = self._run(tests_vulkan=["desc (tests/nope/gone_xyz.cpp)"])
        self.assertTrue(any("stale ref" in e for e in errors), errors)

    def test_real_path_in_references_ok(self):
        errors, _ = self._run(references=["docs/capabilities/gpu-platform-capabilities.toml"])
        self.assertFalse(any("stale ref" in e for e in errors), errors)

    def test_engine_uri_not_path_checked(self):
        # engine:// URIs are resolve URIs, not filesystem paths -- must NOT be existence-checked
        errors, _ = self._run(assets=["engine://frame/forward_basic"])
        self.assertFalse(any("stale ref" in e for e in errors), errors)

    def test_backend_string_is_not_a_provider_class(self):
        # regression: `providers` is the ProviderClass axis (gpu), NOT a backend (vulkan/d3d12)
        errors, _ = self._run(providers=["vulkan"])
        self.assertTrue(any("not a subset" in e for e in errors), errors)

    def test_determinism_claim_on_l2_review(self):
        _, reviews = self._run(ceir_level=2, providers=[], determinism_tier="BitExact")
        self.assertTrue(any("over-claim" in r for r in reviews), reviews)

    def test_missing_load_bearing_hard(self):
        f = _feat()
        del f["status"]
        errors, _ = gm.check({"t": f}, ROOT)
        self.assertTrue(any("missing load-bearing field 'status'" in e for e in errors), errors)

    def test_target_assets_without_tests_not_flagged(self):
        # a target legitimately declares planned assets with no tests yet -- not actionable
        _, reviews = self._run(status="target", ceir_level=0, providers=[],
                               assets=["engine://ui/x"], tests_cross_backend=[])
        self.assertFalse(any("assets-without-tests" in r for r in reviews), reviews)

    def test_committed_manifest_is_clean(self):
        # the REAL committed manifest must have 0 HARD errors (the drift ctest enforces this too)
        data = gm.load(ROOT)
        errors, _ = gm.check(data.get("feature", {}), ROOT)
        self.assertEqual(errors, [], "committed manifest has HARD errors:\n" + "\n".join(errors))


if __name__ == "__main__":
    unittest.main()
