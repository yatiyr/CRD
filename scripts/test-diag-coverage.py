#!/usr/bin/env python3
"""DIAG.10b: verdict tests for diag-coverage.py and the nonreproduced-failure register.

The measurement itself runs on a coverage build (linux-clang-coverage). These tests need no build: they hold the
committed coverage registry and failure register to the tree, and drive every verdict of the evaluator through a
temporary tree and a synthetic llvm-cov export: an outcome value that did not run, a transition site that did not
run, a scope file the lane did not instrument, an exclusion that names something that now runs or no longer exists,
a test set that registers nothing, a JUnit that lost or failed a test, and a build that is not a coverage lane.
Run: python scripts/test-diag-coverage.py
"""
import contextlib
import importlib.util
import io
import json
import os
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("diag_coverage", ROOT / "scripts" / "diag-coverage.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

REGISTER = ROOT / "tests" / "support" / "diag" / "nonreproduced-failures.json"
REGISTER_SCHEMA = "cerid-diag-nonreproduced/1"
REGISTER_STATUSES = ("nonreproduced", "explained", "toolchain", "host")
REGISTER_KEYS = {"id", "first_seen", "lane", "target", "signature", "reruns", "status", "explanation", "session",
                 "rows"}

HEADER = """
#pragma once
enum class Result : unsigned char
{
    Ok,       // accepted
    Refused,  // a bound was exceeded
    Broken,   /* a structural error */
};
"""

SOURCE = """#include "result.hpp"
// Result::Broken in a comment is not a producer
Result check(int x)
{
    if (x > 10)
    {
        return Result::Refused;
    }
    if (x < 0)
    {
        return Result::Broken;
    }
    return Result::Ok;
}
const char* name(Result r)
{
    switch (r)
    {
    case Result::Ok: return "ok";
    default: return r == Result::Broken ? "broken" : "refused";
    }
}
void machine(int& state)
{
    state.store(kStateA);
    state.store(x ? kStateB : kStateC);
}
"""

# Line numbers of the producing sites in SOURCE (1-based): Refused 7, Broken 11, Ok 13; transitions 25 (A) and
# 26 (B, C).
LINE = {"Refused": 7, "Broken": 11, "Ok": 13}


def seg(line, col, count, has=True, entry=True, gap=False):
    return [line, col, count, has, entry, gap]


def segments(ran):
    """Segments that open a region on every producing line; `ran` maps a line to its count."""
    out = [seg(3, 1, 1)]
    for line in sorted(set(LINE.values()) | {25, 26}):
        out.append(seg(line, 1, ran.get(line, 0)))
        out.append(seg(line, 60, 1, has=False, entry=False))
    out.append(seg(27, 1, 0, has=False, entry=False))
    return out


def export_for(root, ran, extra_files=()):
    files = [{
        "filename": str(Path(root) / "src" / "a.cpp").replace("\\", "/"),
        "segments": segments(ran),
        "summary": {m: {"count": 10, "covered": 7} for m in mod.METRICS},
    }]
    for name in extra_files:
        files.append({"filename": str(Path(root) / name).replace("\\", "/"), "segments": [seg(1, 1, 1)],
                      "summary": {m: {"count": 2, "covered": 1} for m in mod.METRICS}})
    return {"data": [{"files": files}]}


def registry(exclusions=(), outcomes=None, scope=None):
    return {
        "schema": mod.SCHEMA,
        "lane": "linux-clang-coverage",
        "programs": ["tests/t"],
        "ctests": [],
        "objects": [],
        "scope": scope or [{"group": "g", "rows": ["DIAG.X"], "paths": ["src/*.cpp"]}],
        "outcomes": outcomes if outcomes is not None else [
            {"name": "Result", "kind": "error-path", "enum": "Result", "header": "include/result.hpp"},
            {"name": "machine", "kind": "state-transition", "states": ["kStateA", "kStateB", "kStateC"],
             "site": r"\bstate\.store\(", "files": ["src/a.cpp"]},
        ],
        "exclusions": list(exclusions),
    }


ALL_RAN = {7: 1, 11: 1, 13: 1, 25: 1, 26: 1}


def unrun(line):
    """ALL_RAN with one producing line's region at count zero."""
    counts = dict(ALL_RAN)
    counts[line] = 0
    return counts


class Tree:
    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        (root / "src").mkdir()
        (root / "include").mkdir()
        (root / "include" / "result.hpp").write_text(HEADER, encoding="utf-8")
        (root / "src" / "a.cpp").write_text(SOURCE, encoding="utf-8")
        return root

    def __exit__(self, *exc):
        self.tmp.cleanup()


class ScanTests(unittest.TestCase):
    def test_strip_comments_keeps_lines_and_columns(self):
        text = 'a /* x\ny */ b // c\n"s // t" d\n'
        clean = mod.strip_comments(text)
        self.assertEqual(len(clean), len(text))
        self.assertEqual(clean.count("\n"), text.count("\n"))
        self.assertNotIn("x", clean)
        self.assertNotIn("c", clean.split("\n")[1])
        self.assertIn("d", clean)
        self.assertNotIn("t", clean.split("\n")[2])

    def test_enum_values_ignore_comments_and_initializers(self):
        self.assertEqual(mod.parse_enum(HEADER, "Result"), ["Ok", "Refused", "Broken"])
        with self.assertRaises(mod.RegistryError):
            mod.parse_enum(HEADER, "Missing")

    def test_producers_skip_case_labels_comparisons_and_comments(self):
        sites = mod.producer_sites(SOURCE, "Result", ["Ok", "Refused", "Broken"])
        self.assertEqual([(line, value) for line, _, value, _ in sites], [(7, "Refused"), (11, "Broken"), (13, "Ok")])
        self.assertEqual(sites[0][1], SOURCE.split("\n")[6].index("Result") + 1)

    def test_qualified_comparison_is_not_a_producer(self):
        text = "bool f(S s) { return s == Outer::Result::Ok; }\nS g() { return Outer::Result::Ok; }\n"
        sites = mod.producer_sites(text, "Result", ["Ok"])
        self.assertEqual([line for line, _, _, _ in sites], [2])

    def test_transitions_follow_the_site_pattern(self):
        sites = mod.transition_sites(SOURCE, r"\bstate\.store\(", ["kStateA", "kStateB", "kStateC"])
        self.assertEqual([(line, state) for line, _, state, _ in sites], [(25, "kStateA"), (26, "kStateB"),
                                                                           (26, "kStateC")])

    def test_count_is_the_innermost_counted_region(self):
        segs = [seg(1, 1, 5), seg(2, 4, 0), seg(2, 9, 5, entry=False), seg(3, 1, 0, has=False, entry=False)]
        self.assertEqual(mod.count_at(segs, 1, 3), 5)
        self.assertEqual(mod.count_at(segs, 2, 5), 0)
        self.assertEqual(mod.count_at(segs, 2, 10), 5)
        self.assertIsNone(mod.count_at(segs, 4, 1))
        self.assertIsNone(mod.count_at(segs, 0, 1))


class EvaluateTests(unittest.TestCase):
    def evaluate(self, root, reg, ran, extra_files=()):
        return mod.evaluate(reg, export_for(root, ran, extra_files), root)

    def test_everything_ran(self):
        with Tree() as root:
            report = self.evaluate(root, registry(), ALL_RAN)
        self.assertEqual(report["verdict"], "PASS", report["gaps"] + report["stale"])
        self.assertEqual(report["denominator"]["scope_files"], 1)
        self.assertEqual(report["denominator"]["instrumented_files"], 1)
        self.assertEqual(report["totals"]["branches"], {"count": 10, "covered": 7})
        self.assertEqual(report["groups"]["g"]["branches"], {"count": 10, "covered": 7})
        self.assertEqual(report["groups"]["g"]["instrumented"], 1)
        self.assertIn("VERDICT PASS", mod.render(report))

    def test_an_error_value_that_did_not_run_is_a_gap_naming_its_site(self):
        with Tree() as root:
            report = self.evaluate(root, registry(), unrun(11))
        self.assertEqual(report["verdict"], "GAP")
        self.assertEqual(len(report["gaps"]), 1)
        self.assertIn("Result (error-path): Broken did not run [src/a.cpp:11]", report["gaps"][0])

    def test_a_transition_site_that_did_not_run_is_a_gap(self):
        with Tree() as root:
            report = self.evaluate(root, registry(), unrun(26))
        self.assertEqual(report["verdict"], "GAP")
        self.assertEqual(sorted(g.split(": ")[1].split(" did")[0] for g in report["gaps"]), ["kStateB", "kStateC"])

    def test_an_owned_exclusion_covers_the_gap(self):
        exclusion = {"outcome": "Result", "value": "Broken", "class": "dying-child", "reason": "dies"}
        with Tree() as root:
            report = self.evaluate(root, registry([exclusion]), unrun(11))
        self.assertEqual(report["verdict"], "PASS", report["gaps"] + report["stale"])
        unit = [u for u in report["outcomes"][0]["units"] if u["value"] == "Broken"][0]
        self.assertEqual(unit["state"], "excluded:dying-child")

    def test_a_site_this_lane_does_not_compile_is_named_and_may_be_windows_only(self):
        with Tree() as root:
            export = export_for(root, ALL_RAN)
            segs = export["data"][0]["files"][0]["segments"]
            for segment in segs:
                if segment[0] == 11 and segment[1] == 1:
                    segment[2], segment[3] = 0, False  # a skipped region: the #if branch this lane does not build
            report = mod.evaluate(registry(), export, root)
            self.assertEqual(report["verdict"], "GAP")
            self.assertIn("Broken did not run [src/a.cpp:11 not compiled here]", report["gaps"][0])
            owned = {"outcome": "Result", "value": "Broken", "class": "windows-only", "reason": "Win32 path"}
            report = mod.evaluate(registry([owned]), export, root)
            self.assertEqual(report["verdict"], "PASS", report["gaps"] + report["stale"])
            report = self.evaluate(root, registry([owned]), unrun(11))
        self.assertEqual(report["verdict"], "STALE")
        self.assertIn("windows-only but is compiled on this lane at src/a.cpp:11", report["stale"][0])

    def test_a_transition_exclusion_names_its_line_text(self):
        right = {"outcome": "machine", "value": "kStateB", "text": "state.store(x ? kStateB : kStateC);",
                 "class": "dying-child", "reason": "dies"}
        wrong = dict(right, text="state.store(kStateB);")
        with Tree() as root:
            ran = unrun(26)
            report = self.evaluate(root, registry([right, dict(right, value="kStateC")]), ran)
            self.assertEqual(report["verdict"], "PASS", report["gaps"] + report["stale"])
            report = self.evaluate(root, registry([wrong]), ran)
        self.assertEqual(report["verdict"], "GAP")
        self.assertTrue(any("matches nothing" in s for s in report["stale"]))

    def test_an_exclusion_of_a_value_that_ran_is_stale(self):
        exclusion = {"outcome": "Result", "value": "Refused", "class": "owned-gap", "reason": "untested"}
        with Tree() as root:
            report = self.evaluate(root, registry([exclusion]), ALL_RAN)
        self.assertEqual(report["verdict"], "STALE")
        self.assertIn("Refused ran but is excluded (owned-gap)", report["stale"][0])

    def test_a_no_producer_exclusion_of_a_produced_value_is_stale(self):
        exclusion = {"outcome": "Result", "value": "Broken", "class": "no-producer", "reason": "dead"}
        with Tree() as root:
            report = self.evaluate(root, registry([exclusion]), unrun(11))
            self.assertEqual(report["verdict"], "STALE")
            self.assertTrue(any("has a producing site" in s for s in report["stale"]))
            with self.assertRaises(mod.RegistryError):
                mod.validate(registry([exclusion]), root)

    def test_a_value_without_any_producer_needs_a_no_site_exclusion(self):
        header = HEADER.replace("Broken,   /* a structural error */", "Broken,\n    Lost,")
        with Tree() as root:
            (root / "include" / "result.hpp").write_text(header, encoding="utf-8")
            report = self.evaluate(root, registry(), ALL_RAN)
            self.assertEqual(report["verdict"], "GAP")
            self.assertIn("Lost did not run [no producing site]", report["gaps"][0])
            with self.assertRaises(mod.RegistryError) as raised:
                mod.validate(registry(), root)
            self.assertIn("Lost has no producing site", str(raised.exception))
            owned = {"outcome": "Result", "value": "Lost", "class": "no-producer", "reason": "dead"}
            self.assertEqual(self.evaluate(root, registry([owned]), ALL_RAN)["verdict"], "PASS")
            mod.validate(registry([owned]), root)
            wrong = dict(owned, **{"class": "dying-child"})
            self.assertEqual(self.evaluate(root, registry([wrong]), ALL_RAN)["verdict"], "STALE")

    def test_an_uninstrumented_scope_file_is_a_gap_unless_excluded(self):
        scope = [{"group": "g", "rows": ["DIAG.X"], "paths": ["src/*.cpp", "src/win.cpp"]}]
        with Tree() as root:
            (root / "src" / "win.cpp").write_text("// windows only\n", encoding="utf-8")
            report = self.evaluate(root, registry(scope=scope), ALL_RAN)
            self.assertEqual(report["verdict"], "GAP")
            self.assertIn("scope file src/win.cpp is not instrumented", report["gaps"][0])
            owned = {"path": "src/win.cpp", "class": "windows-only", "reason": "compiled on Windows only"}
            report = self.evaluate(root, registry([owned], scope=scope), ALL_RAN)
            self.assertEqual(report["verdict"], "PASS", report["gaps"] + report["stale"])
            self.assertEqual(report["denominator"]["excluded_files"], ["src/win.cpp"])
            report = self.evaluate(root, registry([owned], scope=scope), ALL_RAN, extra_files=["src/win.cpp"])
        self.assertEqual(report["verdict"], "STALE")
        self.assertIn("excluded path src/win.cpp is instrumented", report["stale"][0])

    def test_an_exclusion_for_a_missing_value_or_path_is_stale(self):
        with Tree() as root:
            bad_value = {"outcome": "Result", "value": "Gone", "class": "owned-gap", "reason": "r"}
            report = self.evaluate(root, registry([bad_value]), ALL_RAN)
            self.assertEqual(report["verdict"], "STALE")
            bad_path = {"path": "src/gone.cpp", "class": "windows-only", "reason": "r"}
            report = self.evaluate(root, registry([bad_path]), ALL_RAN)
            self.assertEqual(report["verdict"], "STALE")

    def test_a_scope_pattern_that_matches_nothing_is_a_registry_error(self):
        scope = [{"group": "g", "rows": [], "paths": ["src/*.cpp", "src/renamed.cpp"]}]
        with Tree() as root:
            with self.assertRaises(mod.RegistryError):
                mod.evaluate(registry(scope=scope), export_for(root, ALL_RAN), root)

    def test_export_paths_outside_the_source_root_are_ignored(self):
        with Tree() as root:
            export = export_for(root, ALL_RAN)
            export["data"][0]["files"][0]["filename"] = "/elsewhere/src/a.cpp"
            report = mod.evaluate(registry(), export, root)
        self.assertEqual(report["verdict"], "GAP")
        self.assertEqual(report["denominator"]["instrumented_files"], 0)


class RegistryTests(unittest.TestCase):
    def write(self, data):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
        json.dump(data, handle)
        handle.close()
        self.addCleanup(os.unlink, handle.name)
        return handle.name

    def test_shape_errors(self):
        good = registry()
        mod.load_registry(self.write(good))
        cases = [
            dict(good, schema="other"),
            dict(good, extra=1),
            dict(good, programs=[]),
            dict(good, exclusions=[{"outcome": "Result", "value": "Ok", "class": "dying-child"}]),
            dict(good, exclusions=[{"outcome": "Result", "value": "Ok", "class": "unimportant", "reason": "r"}]),
            dict(good, exclusions=[{"outcome": "Nope", "value": "Ok", "class": "owned-gap", "reason": "r"}]),
            dict(good, exclusions=[{"path": "p", "outcome": "Result", "value": "Ok", "class": "owned-gap",
                                    "reason": "r"}]),
            dict(good, outcomes=[{"name": "x", "kind": "error-path"}]),
            dict(good, outcomes=[{"name": "x", "kind": "mutation", "enum": "E", "header": "h"}]),
            dict(good, outcomes=[{"name": "m", "kind": "state-transition", "states": ["a"], "site": "(",
                                  "files": ["f"]}]),
        ]
        for case in cases:
            with self.assertRaises(mod.RegistryError, msg=json.dumps(case)[:200]):
                mod.load_registry(self.write(case))

    def test_committed_registry_describes_the_tree(self):
        reg = mod.load_registry(mod.DEFAULT_REGISTRY)
        scope = mod.validate(reg, ROOT)
        self.assertGreater(len(scope), 0)
        self.assertEqual(reg["lane"], "linux-clang-coverage")
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        lane = [p for p in presets["configurePresets"] if p["name"] == reg["lane"]]
        self.assertEqual(len(lane), 1)
        self.assertEqual(lane[0]["cacheVariables"].get("CRD_ENABLE_COVERAGE"), "ON")
        kinds = {o["kind"] for o in reg["outcomes"]}
        self.assertEqual(kinds, set(mod.KINDS))
        tiers = json.loads((ROOT / ".github" / "ci-tiers.json").read_text(encoding="utf-8"))
        self.assertEqual(tiers["presets"][reg["lane"]]["tier"], "diagnostic")

    def test_committed_registry_programs_are_registered_tests(self):
        reg = mod.load_registry(mod.DEFAULT_REGISTRY)
        cmake_text = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                               for p in (ROOT / "tests").rglob("CMakeLists.txt"))
        for program in reg["programs"]:
            name = Path(program).name
            self.assertRegex(cmake_text, r"add_executable\(\s*%s\b" % re.escape(name))
        for ctest in reg["ctests"]:
            target = ctest[: -len("-corpus")]
            self.assertTrue(ctest.endswith("-corpus"), ctest)
            self.assertRegex(cmake_text, r"crd_fuzz_target\(\s*%s\b" % re.escape(target))


class LaneTests(unittest.TestCase):
    def show(self, build, tests):
        return {"tests": [{"name": n, "command": [str(Path(build) / c)] if c else None} for n, c in tests]}

    def test_selection_by_program_and_name(self):
        with tempfile.TemporaryDirectory() as build:
            reg = dict(registry(), programs=["tests/t", "tests/u"], ctests=["corpus"])
            show = self.show(build, [("a", "tests/t"), ("other", "tests/v"), ("b", "tests/u"), ("corpus", "fz"),
                                     ("unbuilt", None)])
            numbers, names, commands = mod.select_tests(reg, build, show)
        self.assertEqual(numbers, [1, 3, 4])
        self.assertEqual(names, ["a", "b", "corpus"])
        self.assertEqual(len(commands), 3)

    def test_a_program_without_tests_or_a_missing_named_test_fails_the_run(self):
        with tempfile.TemporaryDirectory() as build:
            reg = dict(registry(), programs=["tests/t", "tests/u"])
            with self.assertRaises(mod.Failure) as raised:
                mod.select_tests(reg, build, self.show(build, [("a", "tests/t")]))
            self.assertEqual(raised.exception.verdict, "RUN_FAILED")
            self.assertIn("tests/u", str(raised.exception))
            reg = dict(registry(), ctests=["corpus"])
            with self.assertRaises(mod.Failure):
                mod.select_tests(reg, build, self.show(build, [("a", "tests/t")]))

    def test_junit_must_hold_every_selected_test_passing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "j.xml"
            path.write_text('<testsuite><testcase name="a" status="run"/><testcase name="b" status="fail"/>'
                            '<testcase name="c" status="notrun"/></testsuite>', encoding="utf-8")
            problems = mod.read_junit(path, ["a", "b", "c", "d"])
            self.assertEqual(problems, ["b: fail", "c: notrun", "d: not in the JUnit"])
            self.assertEqual(mod.read_junit(path, ["a"]), [])
            path.write_text("<testsuite", encoding="utf-8")
            with self.assertRaises(mod.Failure):
                mod.read_junit(path, ["a"])

    def test_a_build_that_is_not_a_coverage_lane_is_refused(self):
        with tempfile.TemporaryDirectory() as build:
            (Path(build) / "CMakeCache.txt").write_text("CRD_ENABLE_COVERAGE:BOOL=OFF\nCRD_ENABLE_ASAN:BOOL=ON\n",
                                                        encoding="utf-8")
            with self.assertRaises(mod.Failure) as raised:
                mod.run_lane(registry(), build, Path(build) / "out", None, 10)
            self.assertEqual(raised.exception.verdict, "LANE")
            with self.assertRaises(mod.Failure) as raised:
                mod.run_lane(registry(), Path(build) / "absent", Path(build) / "out", None, 10)
            self.assertEqual(raised.exception.verdict, "LANE")

    def test_exit_codes_for_export_mode(self):
        with Tree() as root:
            export = Path(root) / "export.json"
            export.write_text(json.dumps(export_for(root, ALL_RAN)), encoding="utf-8")
            reg = Path(root) / "reg.json"
            reg.write_text(json.dumps(registry()), encoding="utf-8")
            original = mod.ROOT
            mod.ROOT = Path(root)
            quiet = contextlib.ExitStack()
            quiet.enter_context(contextlib.redirect_stdout(io.StringIO()))
            quiet.enter_context(contextlib.redirect_stderr(io.StringIO()))
            try:
                with quiet:
                    self.assertEqual(mod.main(["--registry", str(reg), "--export", str(export)]), 0)
                    export.write_text(json.dumps(export_for(root, unrun(7))), encoding="utf-8")
                    self.assertEqual(mod.main(["--registry", str(reg), "--export", str(export)]), mod.EXIT["GAP"])
                    reg.write_text("{}", encoding="utf-8")
                    self.assertEqual(mod.main(["--registry", str(reg), "--export", str(export)]),
                                     mod.EXIT["REGISTRY"])
            finally:
                mod.ROOT = original


class NonreproducedRegisterTests(unittest.TestCase):
    """The register of flaky and nonreproduced failures is evidence, never a quarantine list."""

    def entries(self):
        data = json.loads(REGISTER.read_text(encoding="utf-8"))
        self.assertEqual(data.get("schema"), REGISTER_SCHEMA)
        return data["failures"]

    def test_every_entry_is_complete(self):
        ids = set()
        for entry in self.entries():
            self.assertEqual(set(entry) - REGISTER_KEYS, set(), entry.get("id"))
            for key in REGISTER_KEYS:
                self.assertIn(key, entry, entry.get("id"))
            self.assertRegex(entry["id"], r"^[a-z0-9]+(-[a-z0-9]+)*$")
            self.assertNotIn(entry["id"], ids)
            ids.add(entry["id"])
            self.assertRegex(entry["first_seen"], r"^\d{4}-\d{2}-\d{2}$")
            self.assertIn(entry["status"], REGISTER_STATUSES, entry["id"])
            for key in ("lane", "target", "signature", "explanation", "reruns"):
                self.assertTrue(str(entry[key]).strip(), "%s: empty %s" % (entry["id"], key))
            self.assertTrue(entry["rows"], entry["id"])
            session = ROOT / "docs" / "sessions" / entry["session"]
            self.assertTrue(session.is_file(), "%s: no session %s" % (entry["id"], entry["session"]))
            self.assertIn(entry["first_seen"], entry["session"], entry["id"])

    def test_the_register_is_never_consumed_by_a_test_run(self):
        name = REGISTER.name
        consumers = [ROOT / ".github" / "workflows" / "ci.yml", ROOT / "tests" / "CMakeLists.txt",
                     ROOT / "CMakeLists.txt"]
        consumers += list((ROOT / "cmake").glob("*.cmake"))
        consumers += [p for p in (ROOT / "scripts").glob("*") if p.is_file() and p.suffix in (".py", ".sh", ".ps1",
                                                                                              ".bat")
                      and p.name != Path(__file__).name]
        for path in consumers:
            text = path.read_text(encoding="utf-8", errors="replace")
            self.assertNotIn(name, text, "%s reads the failure register; it must never select or skip a test" % path)


if __name__ == "__main__":
    unittest.main()
