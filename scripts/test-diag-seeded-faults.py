#!/usr/bin/env python3
"""DIAG.10b: verdict tests for diag-seeded-faults.py.

The seeds themselves run against a real build lane (the runner rebuilds and reruns the detecting tests). These tests
need no build: they hold the committed registry to the tree (every seeded text occurs exactly once, every acceptance
class has a seed) and drive every verdict through a fake lane, including the ones a healthy run never produces: a
seed the suite does not notice, a failure without the named diagnostic, a control that already fails, a test that
did not run, a mutant that does not compile, a restore git does not recognise and a restore that cannot rebuild. Each
run restores its temporary tree byte for byte (an uncommitted change included), and a seeded build that throws still
restores it.
Run: python scripts/test-diag-seeded-faults.py
"""
import importlib.util
import json
import os
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("diag_seeded_faults", ROOT / "scripts" / "diag-seeded-faults.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

FACTS = {"os": "linux", "asan": True, "tsan": False, "ubsan": True, "msvc": False, "asserts": True,
         "shipping": False}


def preset_facts(name):
    """The lane facts a configure preset promises: its cache variables through `inherits`, and its compiler."""
    presets = {p["name"]: p for p in
               json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))["configurePresets"]}

    def cache(preset):
        parents = presets[preset].get("inherits", [])
        values = {}
        for parent in reversed([parents] if isinstance(parents, str) else parents):
            values.update(cache(parent))
        values.update(presets[preset].get("cacheVariables", {}))
        return values

    values = cache(name)

    def on(key):
        return str(values.get(key, "OFF")).upper() == "ON"

    msvc = values.get("CMAKE_CXX_COMPILER") == "cl"
    return {"os": "windows" if msvc else "linux", "asan": on("CRD_ENABLE_ASAN"), "tsan": on("CRD_ENABLE_TSAN"),
            "ubsan": on("CRD_ENABLE_UBSAN"), "msvc": msvc, "asserts": on("CRD_ENABLE_ASSERTS"),
            "shipping": on("CRD_SHIPPING")}


def junit(cases):
    """A CTest-shaped JUnit document: cases maps a name to (status, output)."""
    body = []
    for name, (status, output) in cases.items():
        failure = '<failure message="Failed"/>' if status == "fail" else ""
        skipped = '<skipped message="Disabled"/>' if status in ("notrun", "disabled") else ""
        body.append('<testcase name="%s" classname="%s" time="0.1" status="%s">%s%s<system-out>%s</system-out>'
                    "</testcase>" % (name, name, status, failure, skipped, output))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="lane" tests="%d">%s</testsuite>\n'
            % (len(cases), "".join(body)))


def seed(**overrides):
    base = {
        "id": "guard-removed",
        "class": "removed-guard",
        "summary": "A guard is removed.",
        "edits": [{"file": "src/guard.cpp", "old": "check(x);\n", "new": "\n"}],
        "build": ["guard-tests"],
        "detect": [{"test": "guard: an overrun is reported", "text": ["Overrun"]}],
    }
    base.update(overrides)
    return base


def registry(*seeds):
    classes = {s["class"] for s in seeds}
    filler = [seed(id="filler-%s" % c, **{"class": c}) for c in mod.CLASSES if c not in classes]
    return {"schema": mod.SCHEMA, "seeds": list(seeds) + filler}


class FakeLane:
    """Answers builds and CTest runs from scripted phase results, and records what the tree held at each run."""

    def __init__(self, root, phases, builds=None):
        self.root = Path(root)
        self.phases = phases          # phase -> {test: (status, output)} or None for "no JUnit"
        self.builds = builds or {}    # phase -> exit code (default 0) or an exception to raise
        self.seen = []                # (phase, guard.cpp contents, env subset)

    def _phase(self, path):
        return Path(path).name.split("-")[0].split(".")[0]

    def build(self, targets, log_path):
        phase = self._phase(log_path)
        result = self.builds.get(phase, 0)
        if isinstance(result, BaseException):
            raise result
        return result

    def test(self, names, env, junit_path, log_path):
        phase = self._phase(junit_path)
        guard = self.root / "src" / "guard.cpp"
        self.seen.append((phase, guard.read_text(encoding="utf-8") if guard.exists() else None,
                          {k: env.get(k) for k in ("ASAN_OPTIONS", "TSAN_OPTIONS")}))
        cases = self.phases.get(phase)
        if cases is not None:
            Path(junit_path).write_text(junit(cases), encoding="utf-8")
        return 0


class Tree:
    """A temporary repository with one source file to seed."""

    def __init__(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        (self.root / "src").mkdir()
        self.guard = self.root / "src" / "guard.cpp"
        self.original = b"int f(int x)\n{\ncheck(x);\nreturn x;\n}\n"
        self.guard.write_bytes(self.original)
        self.trace = self.root / "trace"

    def close(self):
        self.dir.cleanup()


PASS = {"guard: an overrun is reported": ("run", "All tests passed")}
CAUGHT = {"guard: an overrun is reported": ("fail", "CHECK( rec.count_of(Overrun) == 1 ) with expansion: 0 == 1")}


def clean(_paths, _root):
    return []


def uncommitted(paths, _root):
    return [" M " + path for path in paths]


class RegistryTests(unittest.TestCase):
    def test_the_committed_registry_fits_the_tree(self):
        seeds = mod.load_registry(mod.DEFAULT_REGISTRY)
        for s in seeds:
            self.assertIsNone(mod.stale_edit(s, ROOT), s["id"])
        self.assertEqual(set(mod.CLASSES), {s["class"] for s in seeds})

    def test_every_committed_seed_runs_on_a_ci_lane_and_every_ci_lane_runs_a_seed(self):
        # The hosted lanes are the presets the CI step `Seeded detector failures` rides on (.github/ci-tiers.json),
        # with the lane facts their configure presets promise. A lane where no seed applies would end ZERO_RUN.
        step = json.loads((ROOT / ".github" / "ci-tiers.json").read_text(encoding="utf-8"))["preset_steps"]
        names = step["Seeded detector failures"]["presets"]
        self.assertEqual(["win-debug", "win-asan", "linux-gcc-debug", "linux-gcc-asan", "linux-clang-tsan"], names)
        lanes = {name: preset_facts(name) for name in names}
        self.assertEqual({"os": "windows", "asan": True, "tsan": False, "ubsan": False, "msvc": True,
                          "asserts": True, "shipping": False}, lanes["win-asan"])
        self.assertEqual({"os": "linux", "asan": False, "tsan": True, "ubsan": False, "msvc": False,
                          "asserts": True, "shipping": False}, lanes["linux-clang-tsan"])
        seeds = mod.load_registry(mod.DEFAULT_REGISTRY)
        for s in seeds:
            self.assertTrue(any(mod.applicable_detect(s, lane) for lane in lanes.values()), s["id"])
        applied = {name: [s["id"] for s in seeds if mod.applicable_detect(s, lane)] for name, lane in lanes.items()}
        self.assertEqual({"win-debug": 3, "win-asan": 6, "linux-gcc-debug": 3, "linux-gcc-asan": 7,
                          "linux-clang-tsan": 6}, {name: len(ids) for name, ids in applied.items()}, applied)

    def test_validate_mode_passes_on_the_tree(self):
        self.assertEqual(0, mod.main(["--validate"]))

    def test_registry_defects_are_refused(self):
        cases = [
            ({"schema": "other", "seeds": []}, "schema"),
            (registry(seed(), seed()), "twice"),
            (registry(seed(**{"class": "flaky"})), "class"),
            (registry(seed(detect=[])), "detect"),
            (registry(seed(detect=[{"test": "t", "text": []}])), "diagnostic text"),
            (registry(seed(when={"gpu": True})), "unknown lane fact"),
            (registry(seed(when={"asan": "yes"})), "cannot be"),
            (registry(seed(edits=[], env={})), "changes source"),
            (registry(seed(build=[])), "names the targets"),
            (registry(seed(edits=[{"file": "../x", "old": "a", "new": "b"}])), "repository-relative"),
            (registry(seed(edits=[{"file": "x", "old": "a", "new": "a"}])), "different text"),
            (registry(seed(env={"ASAN_OPTIONS": "{home}/x"}, edits=[])), "placeholder"),
            (registry(seed(detect=[{"test": "t", "text": ["x"]}, {"test": "t", "text": ["y"]}])), "twice"),
            ({"schema": mod.SCHEMA, "seeds": [seed()]}, "acceptance classes"),
        ]
        for data, needle in cases:
            with self.assertRaises(mod.RegistryError) as raised:
                mod.validate_registry(data)
            self.assertIn(needle, str(raised.exception))

    def test_a_crlf_checkout_is_stale_not_silently_unseeded(self):
        tree = Tree()
        try:
            tree.guard.write_bytes(tree.original.replace(b"\n", b"\r\n"))
            self.assertIn("0 times", mod.stale_edit(seed(), tree.root))
            with self.assertRaises(RuntimeError):
                mod.apply_edits(seed(), tree.root, {})
        finally:
            tree.close()

    def test_a_seeded_text_must_occur_exactly_once(self):
        tree = Tree()
        try:
            self.assertIsNone(mod.stale_edit(seed(), tree.root))
            self.assertIn("0 times", mod.stale_edit(seed(edits=[{"file": "src/guard.cpp", "old": "gone",
                                                                  "new": "x"}]), tree.root))
            tree.guard.write_bytes(tree.original + b"check(x);\n")
            self.assertIn("2 times", mod.stale_edit(seed(), tree.root))
            self.assertIn("missing.cpp", mod.stale_edit(seed(edits=[{"file": "src/missing.cpp", "old": "a",
                                                                      "new": "b"}]), tree.root))
        finally:
            tree.close()


class SuppressionPolicyTests(unittest.TestCase):
    """The committed sanitizer suppressions may name only third-party shared libraries (test-instruments.md)."""

    def test_the_committed_suppressions_name_only_shared_libraries(self):
        # The files the presets hand to a sanitizer, and every suppression file beside the specimens.
        presets = (ROOT / "CMakePresets.json").read_text(encoding="utf-8")
        named = {match.replace("${sourceDir}/", "") for match in re.findall(r"suppressions=([^ \"]+)", presets)}
        files = {ROOT / name for name in named} | set((ROOT / "tests" / "support" / "diag").glob("*suppressions*.txt"))
        self.assertTrue(files)
        for path in sorted(files):
            self.assertEqual([], mod.suppression_violations(path.read_text(encoding="utf-8")), str(path))

    def test_an_entry_for_cerid_code_is_refused(self):
        text = "# comment\n\nrace:libvulkan_lvp.so\ndeadlock:libVkLayer_khronos_validation.so\n"
        self.assertEqual([], mod.suppression_violations(text))
        for entry in ("race:data_race_specimen.cpp", "race:crd::jobs::Scheduler", "leak:*", "race:libcrd-core.so",
                      "called_from_lib", "race:"):
            self.assertEqual([entry], mod.suppression_violations(text + entry + "\n"), entry)


class LaneTests(unittest.TestCase):
    def write_lane(self, root, cache, compiler_id="GNU", simulate_id=""):
        (root / "CMakeFiles" / "4.3.2").mkdir(parents=True)
        (root / "CMakeCache.txt").write_text("\n".join("%s:BOOL=%s" % kv for kv in cache.items()) + "\n",
                                             encoding="utf-8")
        (root / "CMakeFiles" / "4.3.2" / "CMakeCXXCompiler.cmake").write_text(
            'set(CMAKE_CXX_COMPILER_ID "%s")\nset(CMAKE_CXX_SIMULATE_ID "%s")\n' % (compiler_id, simulate_id),
            encoding="utf-8")

    def test_facts_come_from_the_cache_and_the_compiler(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self.write_lane(root, {"CRD_ENABLE_ASAN": "ON", "CRD_ENABLE_ASSERTS": "ON"}, "Clang", "MSVC")
            facts, problem = mod.lane_facts(root, host_os="windows")
            self.assertIsNone(problem)
            self.assertEqual({"os": "windows", "asan": True, "tsan": False, "ubsan": False, "msvc": True,
                              "asserts": True, "shipping": False}, facts)

    def test_multi_config_and_missing_trees_are_unsupported(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self.assertIn("CMakeCache", mod.lane_facts(root)[1])
            self.write_lane(root, {"CMAKE_CONFIGURATION_TYPES": "Debug;Release"})
            self.assertIn("multi-config", mod.lane_facts(root)[1])

    def test_when_is_any_of_all_of(self):
        self.assertTrue(mod.matches(None, FACTS))
        self.assertTrue(mod.matches({"asan": True, "os": "linux"}, FACTS))
        self.assertFalse(mod.matches({"asan": True, "msvc": True}, FACTS))
        self.assertTrue(mod.matches([{"tsan": True}, {"asan": True}], FACTS))
        self.assertFalse(mod.matches([{"tsan": True}, {"os": "windows"}], FACTS))
        s = seed(detect=[{"test": "a", "text": ["x"]}, {"test": "b", "text": ["y"], "when": {"tsan": True}}])
        self.assertEqual(["a"], [d["test"] for d in mod.applicable_detect(s, FACTS)])
        self.assertEqual([], mod.applicable_detect(seed(when={"os": "windows"}), FACTS))

    def test_the_selection_regex_matches_exactly_the_names(self):
        names = ["crash capture (linux): a wild write [x] writes one record", "a+b? c|d $e ^f .g*", "plain"]
        pattern = re.compile(mod.cmake_regex(names))
        for name in names:
            self.assertTrue(pattern.fullmatch(name))
        self.assertFalse(pattern.fullmatch("plain and more"))
        self.assertFalse(pattern.fullmatch("crash capture linux: a wild write x writes one record"))

    def test_preset_environment_follows_inheritance(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "CMakePresets.json").write_text(json.dumps({"testPresets": [
                {"name": "base", "environment": {"A": "1", "B": "${sourceDir}/x"}},
                {"name": "lane", "inherits": "base", "environment": {"A": "2"}},
            ]}), encoding="utf-8")
            env = mod.preset_environment("lane", root)
            self.assertEqual("2", env["A"])
            self.assertEqual(root.as_posix() + "/x", env["B"])
            with self.assertRaises(mod.RegistryError):
                mod.preset_environment("missing", root)

    def test_the_lane_test_record_survives_the_seed_runs(self):
        # The hosted evidence artifact uploads Testing/Temporary/*.log: the seeds' CTest runs must leave the lane's
        # own record, not a LastTestsFailed.log naming the tests a seed broke.
        with tempfile.TemporaryDirectory() as d:
            temporary = Path(d) / "Testing" / "Temporary"
            temporary.mkdir(parents=True)
            (temporary / "LastTest.log").write_bytes(b"the lane's full run\r\n")
            (temporary / "CTestCostData.txt").write_text("cost", encoding="utf-8")
            with self.assertRaises(KeyboardInterrupt):
                with mod.LaneTestRecord(d):
                    (temporary / "LastTest.log").write_text("restored run", encoding="utf-8")
                    (temporary / "LastTestsFailed.log").write_text("1:seeded test", encoding="utf-8")
                    raise KeyboardInterrupt()
            self.assertEqual(b"the lane's full run\r\n", (temporary / "LastTest.log").read_bytes())
            self.assertFalse((temporary / "LastTestsFailed.log").exists())
            self.assertEqual("cost", (temporary / "CTestCostData.txt").read_text(encoding="utf-8"))
            (temporary / "LastTestsFailed.log").write_text("1:a lane failure", encoding="utf-8")
            with mod.LaneTestRecord(d):
                (temporary / "LastTestsFailed.log").unlink()
            self.assertEqual("1:a lane failure", (temporary / "LastTestsFailed.log").read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as d:
            with mod.LaneTestRecord(d):
                pass
            self.assertFalse((Path(d) / "Testing").exists())

    def test_env_placeholders_and_seed_files(self):
        tree = Tree()
        try:
            (tree.root / "supp.txt").write_text("race:libvulkan_lvp.so", encoding="utf-8")
            s = seed(edits=[], build=[], env={"TSAN_OPTIONS": "{inherit} suppressions={trace}/s.txt",
                                              "OTHER": "{source}/x"},
                     files={"s.txt": {"from": "supp.txt", "text": "race:mine.cpp\n"}})
            tree.trace.mkdir()
            mod.write_seed_files(s, tree.trace, tree.root)
            self.assertEqual("race:libvulkan_lvp.so\nrace:mine.cpp\n",
                             (tree.trace / "s.txt").read_text(encoding="utf-8"))
            env = mod.render_env(s, {"TSAN_OPTIONS": "halt_on_error=1"}, tree.trace, tree.root)
            self.assertEqual("halt_on_error=1 suppressions=%s/s.txt" % tree.trace.as_posix(), env["TSAN_OPTIONS"])
            self.assertEqual(tree.root.as_posix() + "/x", env["OTHER"])
            self.assertEqual("suppressions=%s/s.txt" % tree.trace.as_posix(),
                             mod.render_env(s, {}, tree.trace, tree.root)["TSAN_OPTIONS"])
        finally:
            tree.close()


class JudgeTests(unittest.TestCase):
    def parse(self, cases):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "j.xml"
            path.write_text(junit(cases), encoding="utf-8")
            return mod.parse_junit(path)

    def test_junit_statuses_and_output(self):
        results = self.parse({"a": ("run", "ok"), "b": ("fail", "verdict=Unexpected"), "c": ("notrun", "")})
        self.assertEqual("run", results["a"]["status"])
        self.assertEqual("fail", results["b"]["status"])
        self.assertIn("verdict=Unexpected", results["b"]["output"])
        self.assertIn("Failed", results["b"]["output"])
        self.assertEqual("notrun", results["c"]["status"])
        self.assertIsNone(mod.parse_junit(Path(tempfile.gettempdir()) / "no-such-junit.xml"))

    def test_control_verdicts(self):
        self.assertIsNone(mod.judge_passing(self.parse(PASS), list(PASS), "CONTROL_FAILED", "control"))
        self.assertEqual("ZERO_RUN", mod.judge_passing(None, ["a"], "CONTROL_FAILED", "control")[0])
        self.assertEqual("ZERO_RUN", mod.judge_passing({}, ["a"], "CONTROL_FAILED", "control")[0])
        self.assertEqual("ZERO_RUN", mod.judge_passing(self.parse(PASS), ["other"], "CONTROL_FAILED", "c")[0])
        self.assertEqual("CONTROL_FAILED", mod.judge_passing(self.parse(CAUGHT), list(CAUGHT), "CONTROL_FAILED",
                                                             "control")[0])
        skipped = {"guard: an overrun is reported": ("notrun", "")}
        self.assertEqual("RESTORE_FAILED", mod.judge_passing(self.parse(skipped), list(skipped), "RESTORE_FAILED",
                                                             "restored")[0])

    def test_seeded_verdicts(self):
        detects = seed()["detect"]
        self.assertEqual("DETECTED", mod.judge_seeded(self.parse(CAUGHT), detects)[0])
        self.assertEqual("SURVIVED", mod.judge_seeded(self.parse(PASS), detects)[0])
        other = {"guard: an overrun is reported": ("fail", "SIGSEGV in an unrelated helper")}
        verdict, reason = mod.judge_seeded(self.parse(other), detects)
        self.assertEqual("WRONG_FAILURE", verdict)
        self.assertIn("Overrun", reason)
        self.assertEqual("ZERO_RUN", mod.judge_seeded(None, detects)[0])
        self.assertEqual("ZERO_RUN", mod.judge_seeded(self.parse({"x": ("fail", "Overrun")}), detects)[0])
        self.assertEqual("ZERO_RUN", mod.judge_seeded(
            self.parse({"guard: an overrun is reported": ("notrun", "")}), detects)[0])
        wrapped = {"guard: an overrun is reported": ("fail", "with message:\n  reason=exit 1 without an\n  Overrun")}
        self.assertEqual("DETECTED", mod.judge_seeded(self.parse(wrapped), [
            {"test": "guard: an overrun is reported", "text": ["without an Overrun"]}])[0])
        two = detects + [{"test": "b", "text": ["y"]}]
        self.assertEqual("SURVIVED", mod.judge_seeded(self.parse(dict(CAUGHT, b=("run", ""))), two)[0])


class RunSeedTests(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()

    def tearDown(self):
        self.tree.close()

    def run_seed(self, lane, s=None, state=clean, facts=FACTS):
        return mod.run_seed(s or seed(), facts, lane, {"ASAN_OPTIONS": "detect_leaks=1"}, self.tree.trace,
                            self.tree.root, state=state)

    def assert_restored(self):
        self.assertEqual(self.tree.original, self.tree.guard.read_bytes())

    def test_a_detected_seed_runs_control_seeded_and_restored(self):
        lane = FakeLane(self.tree.root, {"control": PASS, "seeded": CAUGHT, "restored": PASS})
        record = self.run_seed(lane)
        self.assertEqual("DETECTED", record["verdict"], record["reason"])
        self.assertEqual(["control", "seeded", "restored"], [s[0] for s in lane.seen])
        self.assertIn("check(x);", lane.seen[0][1])
        self.assertNotIn("check(x);", lane.seen[1][1])
        self.assertIn("check(x);", lane.seen[2][1])
        self.assert_restored()
        self.assertEqual({"guard: an overrun is reported": "fail"}, record["runs"]["seeded"]["results"])

    def test_a_seed_the_suite_misses_survives(self):
        lane = FakeLane(self.tree.root, {"control": PASS, "seeded": PASS, "restored": PASS})
        self.assertEqual("SURVIVED", self.run_seed(lane)["verdict"])
        self.assert_restored()

    def test_a_failing_control_stops_before_the_seed(self):
        lane = FakeLane(self.tree.root, {"control": CAUGHT})
        self.assertEqual("CONTROL_FAILED", self.run_seed(lane)["verdict"])
        self.assertEqual(["control"], [s[0] for s in lane.seen])
        self.assert_restored()

    def test_a_control_that_ran_nothing_is_zero_run(self):
        lane = FakeLane(self.tree.root, {"control": None})
        self.assertEqual("ZERO_RUN", self.run_seed(lane)["verdict"])

    def test_a_mutant_that_does_not_build_is_not_a_detection(self):
        lane = FakeLane(self.tree.root, {"control": PASS, "restored": PASS}, builds={"seeded": 1})
        self.assertEqual("BUILD_FAILED", self.run_seed(lane)["verdict"])
        self.assert_restored()

    def test_the_tree_is_restored_when_the_seeded_build_throws(self):
        lane = FakeLane(self.tree.root, {"control": PASS}, builds={"seeded": KeyboardInterrupt()})
        with self.assertRaises(KeyboardInterrupt):
            self.run_seed(lane)
        self.assert_restored()

    def test_a_restore_that_does_not_build_or_pass_is_reported(self):
        lane = FakeLane(self.tree.root, {"control": PASS, "seeded": CAUGHT}, builds={"restored": 2})
        self.assertEqual("RESTORE_FAILED", self.run_seed(lane)["verdict"])
        lane = FakeLane(self.tree.root, {"control": PASS, "seeded": CAUGHT, "restored": CAUGHT})
        record = self.run_seed(lane)
        self.assertEqual("RESTORE_FAILED", record["verdict"])
        self.assertEqual("DETECTED", record["seeded_verdict"])
        self.assert_restored()

    def test_a_restore_git_does_not_recognise_is_reported(self):
        calls = []

        def state(paths, _root):
            calls.append(list(paths))
            return [] if len(calls) == 1 else [" M " + p for p in paths]

        lane = FakeLane(self.tree.root, {"control": PASS, "seeded": CAUGHT, "restored": PASS})
        record = self.run_seed(lane, state=state)
        self.assertEqual("RESTORE_FAILED", record["verdict"])
        self.assertIn("after the restore", record["reason"])

    def test_an_uncommitted_change_is_seeded_and_survives(self):
        lane = FakeLane(self.tree.root, {"control": PASS, "seeded": CAUGHT, "restored": PASS})
        self.assertEqual("DETECTED", self.run_seed(lane, state=uncommitted)["verdict"])
        self.assert_restored()

    def test_a_tree_git_cannot_describe_runs_nothing(self):
        lane = FakeLane(self.tree.root, {"control": PASS})
        self.assertEqual("TOOL_MISSING", self.run_seed(lane, state=lambda p, r: None)["verdict"])
        self.assertEqual([], lane.seen)

    def test_stale_and_inapplicable_seeds_run_nothing(self):
        lane = FakeLane(self.tree.root, {"control": PASS})
        stale = seed(edits=[{"file": "src/guard.cpp", "old": "absent();\n", "new": "\n"}])
        self.assertEqual("REGISTRY_STALE", self.run_seed(lane, s=stale)["verdict"])
        self.assertEqual("NOT_APPLICABLE", self.run_seed(lane, s=seed(when={"os": "windows"}))["verdict"])
        self.assertEqual([], lane.seen)

    def test_an_env_seed_changes_only_the_seeded_run(self):
        s = seed(edits=[], env={"ASAN_OPTIONS": "{inherit} log_path={trace}/report"})
        lane = FakeLane(self.tree.root, {"control": PASS, "seeded": CAUGHT})
        record = self.run_seed(lane, s=s)
        self.assertEqual("DETECTED", record["verdict"], record["reason"])
        self.assertEqual(["control", "seeded"], [x[0] for x in lane.seen])
        self.assertEqual("detect_leaks=1", lane.seen[0][2]["ASAN_OPTIONS"])
        self.assertEqual("detect_leaks=1 log_path=%s/report" % (self.tree.trace / s["id"]).as_posix(),
                         lane.seen[1][2]["ASAN_OPTIONS"])


class MainTests(unittest.TestCase):
    def test_a_lane_where_no_seed_applies_is_zero_run(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            data = registry(seed(when={"os": "windows"}))
            for s in data["seeds"]:
                s["when"] = {"shipping": True}
            reg = root / "r.json"
            reg.write_text(json.dumps(data), encoding="utf-8")
            (root / "CMakeCache.txt").write_text("CRD_SHIPPING:BOOL=OFF\n", encoding="utf-8")
            self.assertEqual(mod.VERDICTS["ZERO_RUN"], mod.main(["--registry", str(reg), "--build-dir", str(root),
                                                                  "--trace-dir", str(root / "t")]))

    def test_an_unreadable_registry_is_stale(self):
        with tempfile.TemporaryDirectory() as d:
            reg = Path(d) / "r.json"
            reg.write_text("{", encoding="utf-8")
            self.assertEqual(mod.VERDICTS["REGISTRY_STALE"], mod.main(["--registry", str(reg), "--validate"]))

    def test_an_unconfigured_build_dir_is_unsupported(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(mod.VERDICTS["LANE_UNSUPPORTED"], mod.main(["--build-dir", d, "--list"]))


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUTF8", "1")
    unittest.main(verbosity=1)
