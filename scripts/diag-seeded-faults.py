#!/usr/bin/env python3
"""DIAG.10b: seeded detector failures that the diagnostic suite must observe.

Contract: docs/design/runtime-diagnostics.md#diag-10b. Each seed in the registry
(tests/support/diag/seeded-faults.json) breaks one instrument the way it breaks in practice: a sanitizer that is no
longer compiled in, a report that is suppressed or redirected, a removed guard, a fatal record that is lost and a
wrong symbol identity accepted. A seed is either a set of exact source edits (each `old` text must occur exactly once)
or extra environment for the test run. For every seed that applies to the build lane the runner:

  1. builds the seed's targets and runs its detecting tests unseeded (the control: every test must run and pass);
  2. applies the seed, rebuilds and runs the same tests with CTest's JUnit output;
  3. restores every edited file byte for byte (newer timestamp, so the build sees it), checks that git reports each
     edited file exactly as before the seed (an uncommitted change survives untouched), rebuilds and reruns.

A seed is DETECTED only when each of its detecting tests is in the JUnit, failed, and its output carries the
diagnostic text the registry names. The verdict is read from JUnit and the test output, never from CTest's exit
status, and no test is ever rerun to turn a failure into a pass.

Verdicts (exit code of a single seed; a run exits 0 only when every applicable seed is DETECTED):
  DETECTED 0, SURVIVED 2, CONTROL_FAILED 3, ZERO_RUN 4, BUILD_FAILED 5, RESTORE_FAILED 6, REGISTRY_STALE 7,
  NOT_APPLICABLE 9, TOOL_MISSING 10, WRONG_FAILURE 11, LANE_UNSUPPORTED 12.

Usage
  python scripts/diag-seeded-faults.py --validate
  python scripts/diag-seeded-faults.py --build-dir <dir> --list
  python scripts/diag-seeded-faults.py --build-dir <dir> [--preset <test-preset>] [--seed <id> ...]
      [--trace-dir <dir>] [--json <file>] [--expect VERDICT]
On Windows run it inside the MSVC environment (scripts/msvc-env.bat), as every build there needs.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REGISTRY = ROOT / "tests" / "support" / "diag" / "seeded-faults.json"
SCHEMA = "cerid-diag-seeded-faults/1"
REPORT_SCHEMA = "cerid-diag-seeded-faults-report/1"

VERDICTS = {
    "DETECTED": 0,
    "SURVIVED": 2,
    "CONTROL_FAILED": 3,
    "ZERO_RUN": 4,
    "BUILD_FAILED": 5,
    "RESTORE_FAILED": 6,
    "REGISTRY_STALE": 7,
    "NOT_APPLICABLE": 9,
    "TOOL_MISSING": 10,
    "WRONG_FAILURE": 11,
    "LANE_UNSUPPORTED": 12,
}

# The five instrument failures DIAG.10b's acceptance names; the registry must seed each one.
CLASSES = ("sanitizer-activation", "suppressed-report", "removed-guard", "lost-fatal-record", "symbol-mapping")

# Lane facts a seed or a detecting test can be conditioned on, read from the build directory.
FACTS = {"os": ("windows", "linux"), "asan": (True, False), "tsan": (True, False), "ubsan": (True, False),
         "msvc": (True, False), "asserts": (True, False), "shipping": (True, False)}

SEED_KEYS = {"id", "class", "summary", "when", "edits", "env", "files", "build", "detect"}
ID_PATTERN = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
FILE_NAME_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
ENV_NAME_PATTERN = re.compile(r"^[A-Z_][A-Z0-9_]*$")
PLACEHOLDER = re.compile(r"\{([a-z]+)\}")
DEFAULT_TEST_TIMEOUT = 600
DEFAULT_BUILD_TIMEOUT = 3600


class RegistryError(ValueError):
    """The registry cannot be trusted to describe a seed."""


# --------------------------------------------------------------------------------------------------------------------
# Registry
# --------------------------------------------------------------------------------------------------------------------
def _check_when(where, when):
    if when is None:
        return
    alternatives = when if isinstance(when, list) else [when]
    if not alternatives:
        raise RegistryError("%s: an empty `when` list matches nothing" % where)
    for alternative in alternatives:
        if not isinstance(alternative, dict) or not alternative:
            raise RegistryError("%s: `when` holds objects of lane facts" % where)
        for key, value in alternative.items():
            if key not in FACTS:
                raise RegistryError("%s: unknown lane fact %r (known: %s)" % (where, key, ", ".join(sorted(FACTS))))
            if value not in FACTS[key] or isinstance(value, bool) != isinstance(FACTS[key][0], bool):
                raise RegistryError("%s: lane fact %s cannot be %r" % (where, key, value))


def validate_registry(data):
    """Return the seeds of a parsed registry, or raise RegistryError naming the first defect."""
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise RegistryError("the registry must be an object with schema %s" % SCHEMA)
    seeds = data.get("seeds")
    if not isinstance(seeds, list) or not seeds:
        raise RegistryError("the registry holds no seeds")
    seen = set()
    for seed in seeds:
        if not isinstance(seed, dict):
            raise RegistryError("every seed is an object")
        unknown = set(seed) - SEED_KEYS
        if unknown:
            raise RegistryError("seed %s: unknown keys %s" % (seed.get("id"), ", ".join(sorted(unknown))))
        sid = seed.get("id")
        if not isinstance(sid, str) or not ID_PATTERN.match(sid):
            raise RegistryError("seed ids are lower-case kebab names, not %r" % (sid,))
        if sid in seen:
            raise RegistryError("seed %s appears twice" % sid)
        seen.add(sid)
        if seed.get("class") not in CLASSES:
            raise RegistryError("seed %s: class must be one of %s" % (sid, ", ".join(CLASSES)))
        if not isinstance(seed.get("summary"), str) or not seed["summary"].strip():
            raise RegistryError("seed %s: a summary says what breaks" % sid)
        _check_when("seed %s" % sid, seed.get("when"))
        edits = seed.get("edits", [])
        env = seed.get("env", {})
        files = seed.get("files", {})
        if not isinstance(edits, list) or not isinstance(env, dict) or not isinstance(files, dict):
            raise RegistryError("seed %s: edits is a list, env and files are objects" % sid)
        if not edits and not env:
            raise RegistryError("seed %s: a seed changes source (edits) or the run (env)" % sid)
        for edit in edits:
            if not isinstance(edit, dict) or set(edit) != {"file", "old", "new"}:
                raise RegistryError("seed %s: an edit carries exactly file, old and new" % sid)
            if not all(isinstance(edit[key], str) for key in ("file", "old", "new")):
                raise RegistryError("seed %s: edit fields are strings" % sid)
            if not edit["old"] or edit["old"] == edit["new"]:
                raise RegistryError("seed %s: an edit replaces non-empty text with different text" % sid)
            path = Path(edit["file"])
            if path.is_absolute() or ".." in path.parts or "\\" in edit["file"]:
                raise RegistryError("seed %s: edit paths are repository-relative with forward slashes" % sid)
        for name, value in env.items():
            if not ENV_NAME_PATTERN.match(name) or not isinstance(value, str):
                raise RegistryError("seed %s: env maps variable names to strings" % sid)
            for placeholder in PLACEHOLDER.findall(value):
                if placeholder not in ("trace", "source", "inherit"):
                    raise RegistryError("seed %s: unknown placeholder {%s} in %s" % (sid, placeholder, name))
        for name, spec in files.items():
            if not FILE_NAME_PATTERN.match(name) or not isinstance(spec, dict) or not set(spec) <= {"from", "text"}:
                raise RegistryError("seed %s: files map plain names to {from, text}" % sid)
            if not isinstance(spec.get("text", ""), str) or not isinstance(spec.get("from", ""), str):
                raise RegistryError("seed %s: file parts are strings" % sid)
        build = seed.get("build", [])
        if not isinstance(build, list) or not all(isinstance(t, str) and t for t in build):
            raise RegistryError("seed %s: build lists target names" % sid)
        if not build:
            raise RegistryError("seed %s: build names the targets holding the detecting tests, so every run tests "
                                "the current tree" % sid)
        detect = seed.get("detect")
        if not isinstance(detect, list) or not detect:
            raise RegistryError("seed %s: detect names the tests that must fail" % sid)
        names = []
        for entry in detect:
            if not isinstance(entry, dict) or not set(entry) <= {"test", "text", "when"} or "test" not in entry:
                raise RegistryError("seed %s: a detect entry carries test, text and optionally when" % sid)
            if not isinstance(entry["test"], str) or not entry["test"].strip():
                raise RegistryError("seed %s: an empty test name" % sid)
            text = entry.get("text")
            if not isinstance(text, list) or not text or not all(isinstance(t, str) and t for t in text):
                raise RegistryError("seed %s: %s names the diagnostic text its failure must carry"
                                    % (sid, entry["test"]))
            _check_when("seed %s test %s" % (sid, entry["test"]), entry.get("when"))
            names.append(entry["test"])
        if len(names) != len(set(names)):
            raise RegistryError("seed %s: a detecting test is listed twice" % sid)
    missing = [c for c in CLASSES if c not in {s["class"] for s in seeds}]
    if missing:
        raise RegistryError("no seed for the acceptance classes: %s" % ", ".join(missing))
    return seeds


def load_registry(path):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RegistryError("%s: %s" % (path, error)) from error
    return validate_registry(data)


def stale_edit(seed, root):
    """Return why a seed's edits no longer fit the tree, or None when every `old` text occurs exactly once."""
    for edit in seed.get("edits", []):
        path = Path(root) / edit["file"]
        try:
            # Bytes, exactly as apply_edits reads them: no newline translation may hide a CRLF checkout.
            text = path.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError) as error:
            return "%s: %s" % (edit["file"], error)
        count = text.count(edit["old"])
        if count != 1:
            return "%s: the seeded text occurs %d times, not once" % (edit["file"], count)
    for name, spec in seed.get("files", {}).items():
        if spec.get("from") and not (Path(root) / spec["from"]).is_file():
            return "file %s: %s does not exist" % (name, spec["from"])
    return None


SUPPRESSION_ENTRY = re.compile(r"^[a-z_]+:(lib[A-Za-z0-9_+.-]+\.so(\.[0-9]+)*)$")


def suppression_violations(text):
    """Entries of a sanitizer suppression file that name anything but a third-party shared library.

    The committed files may silence only libraries that are not built with the sanitizer (their own synchronization is
    invisible to it); a report in Cerid code is fixed, never suppressed. A seed may add such an entry for its own run
    to prove the suite notices; the committed file never carries one.
    """
    bad = []
    for line in text.splitlines():
        entry = line.strip()
        if not entry or entry.startswith("#"):
            continue
        match = SUPPRESSION_ENTRY.match(entry)
        if match is None or match.group(1).startswith(("libcrd", "libcerid")):
            bad.append(entry)
    return bad


# --------------------------------------------------------------------------------------------------------------------
# Lane facts and selection
# --------------------------------------------------------------------------------------------------------------------
def read_cache(build_dir):
    values = {}
    try:
        lines = (Path(build_dir) / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    for line in lines:
        match = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def _on(value):
    return str(value).upper() in ("ON", "1", "TRUE", "YES", "Y")


def compiler_ids(build_dir):
    for path in sorted((Path(build_dir) / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")):
        text = path.read_text(encoding="utf-8", errors="replace")
        cid = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', text)
        sim = re.search(r'set\(CMAKE_CXX_SIMULATE_ID "([^"]*)"\)', text)
        return (cid.group(1) if cid else ""), (sim.group(1) if sim else "")
    return "", ""


def lane_facts(build_dir, host_os=None):
    """Return (facts, problem). A lane the runner cannot judge (no cache, multi-config) carries a problem."""
    cache = read_cache(build_dir)
    if cache is None:
        return None, "%s has no CMakeCache.txt" % build_dir
    if cache.get("CMAKE_CONFIGURATION_TYPES", "").strip():
        return None, "a multi-config build tree; seed a single-config Ninja lane"
    cid, sim = compiler_ids(build_dir)
    host_os = host_os or ("windows" if os.name == "nt" else "linux")
    facts = {
        "os": host_os,
        "asan": _on(cache.get("CRD_ENABLE_ASAN", "OFF")),
        "tsan": _on(cache.get("CRD_ENABLE_TSAN", "OFF")),
        "ubsan": _on(cache.get("CRD_ENABLE_UBSAN", "OFF")),
        "msvc": cid == "MSVC" or sim == "MSVC",
        "asserts": _on(cache.get("CRD_ENABLE_ASSERTS", "OFF")),
        "shipping": _on(cache.get("CRD_SHIPPING", "OFF")),
    }
    return facts, None


def matches(when, facts):
    if when is None:
        return True
    alternatives = when if isinstance(when, list) else [when]
    return any(all(facts.get(key) == value for key, value in alt.items()) for alt in alternatives)


def applicable_detect(seed, facts):
    if not matches(seed.get("when"), facts):
        return []
    return [entry for entry in seed["detect"] if matches(entry.get("when"), facts)]


def cmake_regex(names):
    """An anchored CMake regular expression that selects exactly `names`."""
    escaped = [re.sub(r"([\\^$.\[\]|()*+?])", r"\\\1", name) for name in names]
    return "^(" + "|".join(escaped) + ")$"


# --------------------------------------------------------------------------------------------------------------------
# JUnit evidence and verdicts
# --------------------------------------------------------------------------------------------------------------------
def parse_junit(path):
    """Return {test name: {'status', 'output'}} from a CTest JUnit file, or None when it is missing or unreadable."""
    try:
        tree = ET.parse(str(path))
    except (OSError, ET.ParseError):
        return None
    results = {}
    for case in tree.getroot().iter("testcase"):
        name = case.get("name")
        if name is None:
            continue
        status = case.get("status") or "run"
        failure = case.find("failure")
        if failure is not None and status == "run":
            status = "fail"
        if case.find("skipped") is not None and status == "run":
            status = "notrun"
        parts = []
        for tag in ("failure", "system-out", "system-err"):
            node = case.find(tag)
            if node is not None:
                parts.append(node.get("message") or "")
                parts.append(node.text or "")
        results[name] = {"status": status, "output": "\n".join(parts)}
    return results


def judge_passing(results, names, failure_verdict, phase):
    """Every named test must be present and have passed: the control and the restored runs."""
    if results is None:
        return "ZERO_RUN", "the %s run left no readable JUnit" % phase
    if not results:
        return "ZERO_RUN", "the %s run selected zero tests" % phase
    for name in names:
        if name not in results:
            return "ZERO_RUN", "the %s run did not run %r" % (phase, name)
    for name in names:
        status = results[name]["status"]
        if status != "run":
            return failure_verdict, "the %s run reports %r as %s" % (phase, name, status)
    return None


def flatten(text):
    return " ".join(text.split())


def judge_seeded(results, detects):
    """DETECTED only when every detecting test failed and its output carries the named diagnostic text."""
    if results is None:
        return "ZERO_RUN", "the seeded run left no readable JUnit"
    survived = []
    wrong = []
    for entry in detects:
        name = entry["test"]
        if name not in results:
            return "ZERO_RUN", "the seeded run did not run %r" % name
        status = results[name]["status"]
        if status in ("notrun", "disabled"):
            return "ZERO_RUN", "the seeded run reports %r as %s" % (name, status)
        if status == "run":
            survived.append(name)
            continue
        # Catch2 wraps a long message over indented lines, so the text is compared with whitespace runs collapsed.
        output = flatten(results[name]["output"])
        missing = [text for text in entry["text"] if flatten(text) not in output]
        if missing:
            wrong.append("%r failed without %s" % (name, ", ".join(repr(t) for t in missing)))
    if survived:
        return "SURVIVED", "passed with the seed: %s" % ", ".join(repr(n) for n in survived)
    if wrong:
        return "WRONG_FAILURE", "; ".join(wrong)
    return "DETECTED", "%d test(s) failed with the expected diagnostic" % len(detects)


# --------------------------------------------------------------------------------------------------------------------
# Environment and files of a seed
# --------------------------------------------------------------------------------------------------------------------
def render_env(seed, base_env, trace_dir, root):
    env = dict(base_env)
    for name, template in seed.get("env", {}).items():
        values = {"trace": Path(trace_dir).as_posix(), "source": Path(root).as_posix(),
                  "inherit": base_env.get(name, "")}
        env[name] = PLACEHOLDER.sub(lambda m: values[m.group(1)], template).strip()
    return env


def write_seed_files(seed, trace_dir, root):
    for name, spec in seed.get("files", {}).items():
        content = ""
        if spec.get("from"):
            content = (Path(root) / spec["from"]).read_text(encoding="utf-8")
            if content and not content.endswith("\n"):
                content += "\n"
        content += spec.get("text", "")
        (Path(trace_dir) / name).write_text(content, encoding="utf-8", newline="\n")


def preset_environment(preset, root):
    """The environment of a CMake test preset (and the presets it inherits), with ${sourceDir} expanded."""
    data = json.loads((Path(root) / "CMakePresets.json").read_text(encoding="utf-8"))
    presets = {p["name"]: p for p in data.get("testPresets", [])}
    if preset not in presets:
        raise RegistryError("CMakePresets.json has no test preset %r" % preset)
    chain = []
    name = preset
    while name is not None:
        if name in chain:
            raise RegistryError("test preset %r inherits itself" % name)
        chain.append(name)
        parent = presets[name].get("inherits")
        name = parent[0] if isinstance(parent, list) and parent else parent
        if name is not None and name not in presets:
            raise RegistryError("test preset %r inherits unknown %r" % (chain[-1], name))
    env = {}
    for name in reversed(chain):
        for key, value in presets[name].get("environment", {}).items():
            env[key] = str(value).replace("${sourceDir}", Path(root).as_posix())
    return env


# --------------------------------------------------------------------------------------------------------------------
# The build lane: CMake build and CTest runs (replaced by a fake in the verdict tests)
# --------------------------------------------------------------------------------------------------------------------
class Lane:
    def __init__(self, build_dir, cmake, ctest, jobs, test_timeout, build_timeout):
        self.build_dir = Path(build_dir).resolve()
        self.cmake = cmake
        self.ctest = ctest
        self.jobs = jobs
        self.test_timeout = test_timeout
        self.build_timeout = build_timeout

    def build(self, targets, log_path):
        command = [self.cmake, "--build", str(self.build_dir), "--target"] + list(targets)
        if self.jobs:
            command += ["-j", str(self.jobs)]
        return self._run(command, os.environ.copy(), log_path, self.build_timeout)

    def test(self, names, env, junit_path, log_path):
        command = [self.ctest, "--test-dir", str(self.build_dir), "-R", cmake_regex(names), "--no-tests=error",
                   "--timeout", str(self.test_timeout), "--output-junit", str(Path(junit_path).resolve())]
        if self.jobs:
            command += ["-j", str(self.jobs)]
        # Generous outer bound: CTest kills a test at --timeout; this only guards CTest itself.
        return self._run(command, env, log_path, self.test_timeout * max(1, len(names)) + 120)

    @staticmethod
    def _run(command, env, log_path, timeout):
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            log.write("$ %s\n" % subprocess.list2cmdline(command))
            log.flush()
            try:
                return subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout,
                                      check=False).returncode
            except subprocess.TimeoutExpired:
                log.write("\n[diag-seeded-faults] killed after %d s\n" % timeout)
                return None
            except OSError as error:
                log.write("\n[diag-seeded-faults] %s\n" % error)
                return None


def git_state(paths, root):
    """git's porcelain status lines for `paths` ([] when all are unchanged; None when git cannot say)."""
    if not paths:
        return []
    try:
        done = subprocess.run(["git", "status", "--porcelain", "--"] + list(paths), cwd=str(root),
                              capture_output=True, text=True, check=False)
    except OSError:
        return None
    if done.returncode != 0:
        return None
    return sorted(line for line in done.stdout.splitlines() if line.strip())


# --------------------------------------------------------------------------------------------------------------------
# One seed, end to end
# --------------------------------------------------------------------------------------------------------------------
def apply_edits(seed, root, originals):
    """Apply the seed's edits, recording {path: original bytes} in `originals` before each file is first changed."""
    for edit in seed.get("edits", []):
        path = Path(root) / edit["file"]
        if path not in originals:
            originals[path] = path.read_bytes()
        text = path.read_bytes().decode("utf-8")
        seeded = text.replace(edit["old"], edit["new"], 1)
        if seeded == text:
            raise RuntimeError("%s: the seeded text is absent; the seed would not apply" % edit["file"])
        path.write_bytes(seeded.encode("utf-8"))


def restore_edits(originals):
    """Write every original back with a fresh timestamp; return the paths whose bytes still differ."""
    differing = []
    now = time.time()
    for path, data in originals.items():
        path.write_bytes(data)
        os.utime(path, (now, now))
        if path.read_bytes() != data:
            differing.append(str(path))
    return differing


def run_seed(seed, facts, lane, base_env, trace_root, root, state=git_state):
    record = {"id": seed["id"], "class": seed["class"], "summary": seed["summary"], "runs": {}}

    def finish(verdict, reason):
        record["verdict"] = verdict
        record["reason"] = reason
        return record

    detects = applicable_detect(seed, facts)
    if not detects:
        return finish("NOT_APPLICABLE", "no detecting test applies to this lane")
    names = [entry["test"] for entry in detects]
    record["tests"] = names
    stale = stale_edit(seed, root)
    if stale:
        return finish("REGISTRY_STALE", stale)
    edited = sorted({edit["file"] for edit in seed.get("edits", [])})
    before = state(edited, root)
    if before is None:
        return finish("TOOL_MISSING", "git cannot report the state of %s" % ", ".join(edited))
    trace = Path(trace_root) / seed["id"]
    trace.mkdir(parents=True, exist_ok=True)
    record["trace"] = str(trace)
    targets = seed["build"]

    def build(phase):
        code = lane.build(targets, trace / ("%s-build.log" % phase))
        record["runs"].setdefault(phase, {})["build_exit"] = code
        return code == 0

    def test(phase, env):
        junit = trace / ("%s.xml" % phase)
        if junit.exists():
            junit.unlink()
        code = lane.test(names, env, junit, trace / ("%s-ctest.log" % phase))
        results = parse_junit(junit)
        run = record["runs"].setdefault(phase, {})
        run["ctest_exit"] = code
        run["junit"] = str(junit)
        run["results"] = {} if results is None else {n: r["status"] for n, r in results.items()}
        return results

    # 1. The control: the detecting tests run and pass on the unseeded tree.
    if not build("control"):
        return finish("BUILD_FAILED", "the unseeded targets do not build (see control-build.log)")
    failure = judge_passing(test("control", base_env), names, "CONTROL_FAILED", "control")
    if failure:
        return finish(*failure)

    # 2. The seeded run, then 3. the restored run. The tree is restored whatever happens in between.
    write_seed_files(seed, trace, root)
    seeded_env = render_env(seed, base_env, trace, root)
    record["runs"]["seeded"] = {"env": {k: seeded_env[k] for k in seed.get("env", {})}}
    originals = {}
    try:
        apply_edits(seed, root, originals)
        if not originals or build("seeded"):
            verdict = judge_seeded(test("seeded", seeded_env), detects)
        else:
            verdict = ("BUILD_FAILED", "the seeded targets do not build (see seeded-build.log)")
    finally:
        differing = restore_edits(originals)
    # Kept even when the restore phase fails below, so a restore problem never hides what the seed did.
    record["seeded_verdict"], record["seeded_reason"] = verdict
    if differing:
        return finish("RESTORE_FAILED", "restored bytes differ in %s" % ", ".join(differing))
    if originals:
        after = state(edited, root)
        if after != before:
            return finish("RESTORE_FAILED", "git reports %s after the restore, %s before"
                          % (after or "no change", before or "no change"))
        if not build("restored"):
            return finish("RESTORE_FAILED", "the restored targets do not build (see restored-build.log)")
        failure = judge_passing(test("restored", base_env), names, "RESTORE_FAILED", "restored")
        if failure:
            return finish("RESTORE_FAILED", failure[1])
    return finish(*verdict)


# --------------------------------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------------------------------
def tool_path(cache, key, name):
    candidate = (cache or {}).get(key)
    if candidate and Path(candidate).is_file():
        return candidate
    return shutil.which(name)


def main(argv):
    parser = argparse.ArgumentParser(description="DIAG.10b seeded detector failures")
    parser.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    parser.add_argument("--validate", action="store_true", help="check the registry against the tree, build nothing")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--list", action="store_true", help="print the seeds that apply to the lane")
    parser.add_argument("--seed", action="append", default=[], help="run only this seed (repeatable)")
    parser.add_argument("--preset", help="test preset whose environment the runs inherit")
    parser.add_argument("--trace-dir", type=Path, help="default: <build-dir>/diag-seeded-faults")
    parser.add_argument("--json", type=Path, help="report file (default: <trace-dir>/report.json)")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--test-timeout", type=int, default=DEFAULT_TEST_TIMEOUT)
    parser.add_argument("--build-timeout", type=int, default=DEFAULT_BUILD_TIMEOUT)
    parser.add_argument("--expect", choices=sorted(VERDICTS), help="with one --seed: exit 0 only for this verdict")
    args = parser.parse_args(argv)

    try:
        seeds = load_registry(args.registry)
    except RegistryError as error:
        print("REGISTRY_STALE: %s" % error)
        return VERDICTS["REGISTRY_STALE"]
    if args.validate:
        problems = [(s["id"], stale_edit(s, ROOT)) for s in seeds]
        problems = [p for p in problems if p[1]]
        for sid, problem in problems:
            print("REGISTRY_STALE: %s: %s" % (sid, problem))
        if problems:
            return VERDICTS["REGISTRY_STALE"]
        print("registry valid: %d seeds over %d classes" % (len(seeds), len(CLASSES)))
        return 0
    if args.build_dir is None:
        parser.error("--build-dir is required unless --validate")
    if args.expect and len(args.seed) != 1:
        parser.error("--expect judges exactly one --seed")
    unknown = [s for s in args.seed if s not in {seed["id"] for seed in seeds}]
    if unknown:
        parser.error("unknown seed(s): %s" % ", ".join(unknown))

    facts, problem = lane_facts(args.build_dir)
    if problem:
        print("LANE_UNSUPPORTED: %s" % problem)
        return VERDICTS["LANE_UNSUPPORTED"]
    selected = [s for s in seeds if not args.seed or s["id"] in args.seed]
    if args.list:
        for seed in selected:
            detects = applicable_detect(seed, facts)
            state = "applies" if detects else "not applicable"
            print("%-40s %-22s %s (%d test(s))" % (seed["id"], seed["class"], state, len(detects)))
        return 0

    cache = read_cache(args.build_dir)
    cmake = tool_path(cache, "CMAKE_COMMAND", "cmake")
    ctest = tool_path(cache, "CMAKE_CTEST_COMMAND", "ctest")
    if not cmake or not ctest:
        print("TOOL_MISSING: cmake or ctest is not available")
        return VERDICTS["TOOL_MISSING"]
    base_env = os.environ.copy()
    if args.preset:
        try:
            base_env.update(preset_environment(args.preset, ROOT))
        except RegistryError as error:
            print("LANE_UNSUPPORTED: %s" % error)
            return VERDICTS["LANE_UNSUPPORTED"]
    trace_root = (args.trace_dir or args.build_dir / "diag-seeded-faults").resolve()
    trace_root.mkdir(parents=True, exist_ok=True)
    lane = Lane(args.build_dir, cmake, ctest, args.jobs, args.test_timeout, args.build_timeout)

    records = []
    for seed in selected:
        record = run_seed(seed, facts, lane, base_env, trace_root, ROOT)
        records.append(record)
        print("%-40s %-22s %s: %s" % (record["id"], record["class"], record["verdict"], record["reason"]), flush=True)
    report = {"schema": REPORT_SCHEMA, "lane": facts, "build_dir": str(Path(args.build_dir).resolve()),
              "preset": args.preset, "seeds": records}
    (args.json or trace_root / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    if args.expect:
        verdict = records[0]["verdict"]
        if verdict != args.expect:
            print("expected %s" % args.expect)
            return 1
        return 0
    ran = [r for r in records if r["verdict"] != "NOT_APPLICABLE" or args.seed]
    if not ran:
        print("ZERO_RUN: no seed applies to this lane")
        return VERDICTS["ZERO_RUN"]
    for record in ran:
        if record["verdict"] != "DETECTED":
            return VERDICTS[record["verdict"]]
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
