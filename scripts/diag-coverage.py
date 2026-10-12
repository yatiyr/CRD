#!/usr/bin/env python3
"""DIAG.10b: coverage of the diagnostic scope, with its instrumented denominator stated.

Contract: docs/design/runtime-diagnostics.md#diag-10b. The registry (tests/support/diag/coverage-scope.json) names
the diagnostic scope (the source files that are the denominator), the test set that exercises it, the outcome sets
whose every value must be reached, and the owned exclusions. On a configuration built with CRD_ENABLE_COVERAGE (the
`linux-clang-coverage` preset: clang source-based coverage) the runner:

  1. selects exactly the registry's test set from CTest (every program must register tests; zero is a failure);
  2. runs it once with LLVM_PROFILE_FILE pointing at a fresh directory and reads CTest's JUnit: every selected test
     must be in it and pass (a failing run is not coverage evidence, and nothing is rerun). A Catch2 program the
     registry lists as `direct` (a diagnostic binary CTest does not register) runs once with Catch2's JUnit reporter,
     which must show tests and no failure;
  3. merges the raw profiles (any unreadable profile fails the merge) and exports the scope's files from every
     program, specimen and helper binary the registry names;
  4. reports, per scope file, the regions, branches, functions and lines llvm-cov instrumented on this lane and how
     many ran, and evaluates the outcome sets.

The numbers are a measurement, never a threshold. The gate is the outcome sets:
  - error-path: every value of a registered result/status enum must be produced at a site that ran. A producing site
    is an occurrence of `Enum::Value` in the set's files that is not a `case` label and not an operand of `==` or
    `!=`; it ran when the innermost coverage region holding that column has a non-zero count.
  - state-transition: every site that moves a registered state machine into a state (the set's `site` pattern
    followed, on the same line, by a state token) must have run. A site is identified by its file, token and line
    text, so a moved line keeps its identity.
A value or site that did not run must be owned by an exclusion with a class and a reason (for example: reached only
inside a child process that dies, which writes no profile); an exclusion that names a value or site that now runs,
or that no longer exists, is stale. A scope file that is not instrumented at all must be excluded by path.

Exit codes: 0 PASS, 2 GAP (an unowned value, site or file did not run), 3 STALE (an exclusion no longer matches),
4 REGISTRY (the registry does not describe the tree), 5 RUN_FAILED (a selected test failed, was missing or did not
run, or the merge or export failed), 6 LANE (the build is not a coverage configuration), 10 TOOL_MISSING.

Usage
  python scripts/diag-coverage.py --validate
  python scripts/diag-coverage.py --build-dir <dir> [--out <dir>] [--json <file>] [--llvm-bin <dir>]
  python scripts/diag-coverage.py --export <llvm-cov export json> [--source-root <dir>] [--json <file>]
"""
import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REGISTRY = ROOT / "tests" / "support" / "diag" / "coverage-scope.json"
SCHEMA = "cerid-diag-coverage/1"
REPORT_SCHEMA = "cerid-diag-coverage-report/1"

EXIT = {"PASS": 0, "GAP": 2, "STALE": 3, "REGISTRY": 4, "RUN_FAILED": 5, "LANE": 6, "TOOL_MISSING": 10}
KINDS = ("error-path", "state-transition")
# Why a value, site or file is allowed not to run on this lane. Each is a reason the lane cannot observe it, or an
# owned gap; none is "not important".
EXCLUSION_CLASSES = ("dying-child", "windows-only", "other-configuration", "resource-failure", "no-producer",
                     "default-value", "owned-gap")
# Classes for a value that has no producing site in the scope: it is never produced (a dead outcome, owned as such),
# or it is only a field's default initializer.
NO_SITE_CLASSES = ("no-producer", "default-value")
METRICS = ("regions", "branches", "functions", "lines")
DEFAULT_TEST_TIMEOUT = 1800
REGISTRY_KEYS = {"schema", "lane", "programs", "ctests", "direct", "objects", "scope", "outcomes", "exclusions"}
SCOPE_KEYS = {"group", "rows", "paths"}
OUTCOME_KEYS = {"name", "kind", "enum", "header", "states", "site", "files", "row"}
EXCLUSION_KEYS = {"outcome", "value", "text", "path", "class", "reason"}


class RegistryError(ValueError):
    """The registry cannot be trusted to describe the scope."""


class Failure(Exception):
    def __init__(self, verdict, message):
        super().__init__(message)
        self.verdict = verdict


# --------------------------------------------------------------------------------------------------------------------
# Registry


def load_registry(path):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RegistryError("cannot read registry %s: %s" % (path, error)) from error
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise RegistryError("registry schema must be %s" % SCHEMA)
    unknown = set(data) - REGISTRY_KEYS
    if unknown:
        raise RegistryError("unknown registry keys: %s" % ", ".join(sorted(unknown)))
    for key in ("programs", "ctests", "direct", "objects", "scope", "outcomes", "exclusions"):
        if not isinstance(data.get(key, []), list):
            raise RegistryError("%s must be a list" % key)
    if not data.get("programs"):
        raise RegistryError("the test set names no program")
    if not data.get("scope"):
        raise RegistryError("the scope names no file")
    for group in data["scope"]:
        if not isinstance(group, dict) or set(group) - SCOPE_KEYS or not group.get("paths") or not group.get("group"):
            raise RegistryError("a scope group needs a group name, its rows and its paths: %r" % (group,))
    names = set()
    for outcome in data.get("outcomes", []):
        if not isinstance(outcome, dict) or set(outcome) - OUTCOME_KEYS:
            raise RegistryError("bad outcome set: %r" % (outcome,))
        name = outcome.get("name")
        if not name or name in names:
            raise RegistryError("an outcome set needs a unique name: %r" % (name,))
        names.add(name)
        if outcome.get("kind") not in KINDS:
            raise RegistryError("%s: kind must be one of %s" % (name, ", ".join(KINDS)))
        if outcome["kind"] == "error-path":
            if not outcome.get("enum") or not outcome.get("header"):
                raise RegistryError("%s: an error-path set names its enum and the header that declares it" % name)
        else:
            if not outcome.get("states") or not outcome.get("site") or not outcome.get("files"):
                raise RegistryError("%s: a state-transition set names its states, site pattern and files" % name)
            try:
                re.compile(outcome["site"])
            except re.error as error:
                raise RegistryError("%s: bad site pattern: %s" % (name, error)) from error
    for exclusion in data.get("exclusions", []):
        if not isinstance(exclusion, dict) or set(exclusion) - EXCLUSION_KEYS:
            raise RegistryError("bad exclusion: %r" % (exclusion,))
        if exclusion.get("class") not in EXCLUSION_CLASSES:
            raise RegistryError("exclusion class must be one of %s: %r" % (", ".join(EXCLUSION_CLASSES), exclusion))
        if not str(exclusion.get("reason", "")).strip():
            raise RegistryError("an exclusion needs its reason: %r" % (exclusion,))
        has_path = "path" in exclusion
        has_outcome = "outcome" in exclusion and "value" in exclusion
        if has_path == has_outcome:
            raise RegistryError("an exclusion names either a scope path or an outcome value: %r" % (exclusion,))
        if has_outcome and exclusion["outcome"] not in names:
            raise RegistryError("exclusion names an unknown outcome set: %r" % (exclusion,))
    return data


def scope_files(registry, root):
    """Every scope file (repo-relative, posix), each matched by exactly one group; a pattern that matches nothing is
    stale."""
    root = Path(root)
    owners = {}
    for group in registry["scope"]:
        for pattern in group["paths"]:
            matches = sorted(p for p in root.glob(pattern) if p.is_file())
            if not matches:
                raise RegistryError("scope path %s matches no file" % pattern)
            for match in matches:
                rel = match.relative_to(root).as_posix()
                if rel in owners and owners[rel] != group["group"]:
                    raise RegistryError("%s is in two scope groups" % rel)
                owners[rel] = group["group"]
    return owners


def outcome_files(outcome, scope):
    patterns = outcome.get("files")
    if not patterns:
        return sorted(scope)
    selected = sorted(f for f in scope if any(fnmatch.fnmatchcase(f, p) for p in patterns))
    if not selected:
        raise RegistryError("%s: its files match no scope file" % outcome["name"])
    return selected


# --------------------------------------------------------------------------------------------------------------------
# Source scanning


def strip_comments(text):
    """The text with // and /* */ comments and string/char literal contents blanked, keeping every line and column."""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if c == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for k in range(i, end):
                if text[k] != "\n":
                    out[k] = " "
            i = end
            continue
        if c in "\"'":
            quote = c
            if quote == "'" and i > 0 and (text[i - 1].isalnum()):
                i += 1  # a digit separator (1'000), not a literal
                continue
            k = i + 1
            while k < n and text[k] != quote and text[k] != "\n":
                if text[k] == "\\":
                    out[k] = " "
                    k += 1
                if k < n and text[k] != "\n":
                    out[k] = " "
                k += 1
            i = k + 1
            continue
        i += 1
    return "".join(out)


def parse_enum(text, name):
    """The enumerators of `enum class <name>` in a header."""
    clean = strip_comments(text)
    match = re.search(r"\benum\s+class\s+%s\b[^{;]*\{([^}]*)\}" % re.escape(name), clean)
    if not match:
        raise RegistryError("enum class %s not found" % name)
    values = []
    for item in match.group(1).split(","):
        item = item.strip()
        if not item:
            continue
        ident = re.match(r"([A-Za-z_]\w*)", item)
        if not ident:
            raise RegistryError("cannot read an enumerator of %s: %r" % (name, item))
        values.append(ident.group(1))
    if not values:
        raise RegistryError("enum class %s has no enumerators" % name)
    return values


def producer_sites(text, enum, values):
    """(line, column, value, line text) of every producing occurrence of enum::value; columns are 1-based."""
    clean = strip_comments(text)
    raw_lines = text.split("\n")
    sites = []
    pattern = re.compile(r"\b%s::([A-Za-z_]\w*)\b" % re.escape(enum))
    for number, line in enumerate(clean.split("\n"), start=1):
        stripped = line.strip()
        if stripped.startswith("case "):
            continue
        for match in pattern.finditer(line):
            value = match.group(1)
            if value not in values:
                continue
            start = match.start()
            # The qualified name may be longer (Outer::Enum::Value); the comparison test looks at the whole token.
            head = start
            while head > 0 and (line[head - 1].isalnum() or line[head - 1] in "_:"):
                head -= 1
            before = line[:head].rstrip()
            after = line[match.end():].lstrip()
            if before.endswith("==") or before.endswith("!=") or after.startswith("==") or after.startswith("!="):
                continue
            sites.append((number, start + 1, value, raw_lines[number - 1].strip()))
    return sites


def transition_sites(text, site_pattern, states):
    """(line, column, state, line text) of every state token that follows the site pattern on its line."""
    clean = strip_comments(text)
    raw_lines = text.split("\n")
    site = re.compile(site_pattern)
    token = re.compile(r"(?<![\w:])(?:\w+::)*(%s)\b" % "|".join(re.escape(s) for s in states))
    sites = []
    for number, line in enumerate(clean.split("\n"), start=1):
        found = site.search(line)
        if not found:
            continue
        for match in token.finditer(line, found.end()):
            sites.append((number, match.start(1) + 1, match.group(1), raw_lines[number - 1].strip()))
    return sites


# --------------------------------------------------------------------------------------------------------------------
# Coverage export


def count_at(segments, line, column):
    """The execution count of the innermost region holding (line, column), from llvm-cov's sorted segments, or None
    when no counted region holds it."""
    best = None
    for segment in segments:
        seg_line, seg_col, count, has_count = segment[0], segment[1], segment[2], segment[3]
        is_gap = len(segment) > 5 and segment[5]
        if (seg_line, seg_col) > (line, column):
            break
        if has_count and not is_gap:
            best = count
        elif not has_count:
            best = None
    return best


def export_files(export, source_root):
    """Repo-relative path -> llvm-cov file entry.

    The export names files as the compiler saw them, which need not be the resolved form of the source root: a
    Windows 8.3 short name (the hosted runner's temporary directory is C:/Users/RUNNER~1/...) or a symbolic link
    resolves to a different spelling. Both the root as given and its resolved form are accepted.
    """
    roots = []
    for form in (Path(source_root).absolute(), Path(source_root).resolve()):
        text = str(form).replace("\\", "/").rstrip("/") + "/"
        if text not in roots:
            roots.append(text)
    files = {}
    for data in export.get("data", []):
        for entry in data.get("files", []):
            name = entry["filename"].replace("\\", "/")
            for root in roots:
                if name.startswith(root) or name.lower().startswith(root.lower()):
                    files[name[len(root):]] = entry
                    break
    return files


# --------------------------------------------------------------------------------------------------------------------
# Evaluation


def _exclusion_key(exclusion):
    if "path" in exclusion:
        return ("path", exclusion["path"])
    return ("value", exclusion["outcome"], exclusion["value"], exclusion.get("text"))


def evaluate(registry, export, root, source_root=None):
    """The report and its verdict for one export."""
    root = Path(root)
    scope = scope_files(registry, root)
    files = export_files(export, source_root or root)
    exclusions = registry.get("exclusions", [])
    used = set()
    gaps = []
    stale = []

    path_exclusions = {e["path"]: e for e in exclusions if "path" in e}
    for path in path_exclusions:
        if path not in scope:
            stale.append("excluded path %s is not a scope file" % path)

    per_file = []
    totals = {m: {"count": 0, "covered": 0} for m in METRICS}
    instrumented = 0
    for path in sorted(scope):
        entry = files.get(path)
        excluded = path_exclusions.get(path)
        if entry is None:
            if excluded is None:
                gaps.append("scope file %s is not instrumented on this lane" % path)
            else:
                used.add(_exclusion_key(excluded))
            per_file.append({"path": path, "group": scope[path], "instrumented": False,
                             "excluded": excluded["class"] if excluded else None})
            continue
        if excluded is not None:
            stale.append("excluded path %s is instrumented on this lane" % path)
        instrumented += 1
        summary = entry.get("summary", {})
        row = {"path": path, "group": scope[path], "instrumented": True}
        for metric in METRICS:
            count = int(summary.get(metric, {}).get("count", 0))
            covered = int(summary.get(metric, {}).get("covered", 0))
            row[metric] = [covered, count]
            totals[metric]["count"] += count
            totals[metric]["covered"] += covered
        per_file.append(row)

    groups = {}
    for row in per_file:
        group = groups.setdefault(row["group"], {"files": 0, "instrumented": 0,
                                                 **{m: {"count": 0, "covered": 0} for m in METRICS}})
        group["files"] += 1
        if row["instrumented"]:
            group["instrumented"] += 1
            for metric in METRICS:
                group[metric]["covered"] += row[metric][0]
                group[metric]["count"] += row[metric][1]

    outcome_reports = []
    for outcome in registry.get("outcomes", []):
        name = outcome["name"]
        selected = outcome_files(outcome, scope)
        sites = []
        for path in selected:
            text = (root / path).read_text(encoding="utf-8", errors="replace")
            if outcome["kind"] == "error-path":
                values = parse_enum((root / outcome["header"]).read_text(encoding="utf-8"), outcome["enum"])
                found = producer_sites(text, outcome["enum"], values)
            else:
                values = list(outcome["states"])
                found = transition_sites(text, outcome["site"], values)
            entry = files.get(path)
            for line, column, value, line_text in found:
                count = count_at(entry["segments"], line, column) if entry else None
                sites.append({"path": path, "line": line, "column": column, "value": value, "text": line_text,
                              "count": count})
        if outcome["kind"] == "error-path":
            values = parse_enum((root / outcome["header"]).read_text(encoding="utf-8"), outcome["enum"])
        else:
            values = list(outcome["states"])
        mine = [e for e in exclusions if e.get("outcome") == name]
        for exclusion in mine:
            if exclusion["value"] not in values:
                stale.append("%s: excluded value %s is not one of its values" % (name, exclusion["value"]))
        units = []
        if outcome["kind"] == "error-path":
            for value in values:
                producing = [s for s in sites if s["value"] == value]
                ran = [s for s in producing if s["count"]]
                units.append({"value": value, "sites": len(producing), "ran": len(ran),
                              "unrun": ["%s:%d" % (s["path"], s["line"]) for s in producing if s["count"] == 0],
                              "uncompiled": ["%s:%d" % (s["path"], s["line"]) for s in producing
                                             if s["count"] is None]})
        else:
            for s in sites:
                where = "%s:%d" % (s["path"], s["line"])
                units.append({"value": s["value"], "text": s["text"], "site": where, "sites": 1,
                              "ran": 1 if s["count"] else 0, "unrun": [where] if s["count"] == 0 else [],
                              "uncompiled": [where] if s["count"] is None else []})
            for value in values:
                if not any(s["value"] == value for s in sites):
                    units.append({"value": value, "text": None, "site": None, "sites": 0, "ran": 0, "unrun": [],
                                  "uncompiled": []})
        covered = 0
        for unit in units:
            match = None
            for exclusion in mine:
                if exclusion["value"] != unit["value"]:
                    continue
                if outcome["kind"] == "state-transition" and exclusion.get("text") not in (None, unit["text"]):
                    continue
                match = exclusion
                break
            if unit["ran"]:
                covered += 1
                if match is not None:
                    stale.append("%s: %s%s ran but is excluded (%s)" % (
                        name, unit["value"], " at %s" % unit["site"] if unit.get("site") else "", match["class"]))
                    used.add(_exclusion_key(match))
                unit["state"] = "ran"
                continue
            if match is not None:
                used.add(_exclusion_key(match))
                if unit["sites"] == 0 and match["class"] not in NO_SITE_CLASSES:
                    stale.append("%s: %s has no producing site; its exclusion must say %s" % (
                        name, unit["value"], " or ".join(NO_SITE_CLASSES)))
                if unit["sites"] > 0 and match["class"] in NO_SITE_CLASSES:
                    stale.append("%s: %s has a producing site but is excluded as %s" % (
                        name, unit["value"], match["class"]))
                if match["class"] == "windows-only" and unit["unrun"]:
                    stale.append("%s: %s is excluded as windows-only but is compiled on this lane at %s" % (
                        name, unit["value"], ", ".join(unit["unrun"][:4])))
                unit["state"] = "excluded:%s" % match["class"]
                continue
            unit["state"] = "gap"
            listed = unit["unrun"] + ["%s not compiled here" % u for u in unit["uncompiled"]]
            where = ", ".join(listed[:6]) + (", ..." if len(listed) > 6 else "") if listed else "no producing site"
            gaps.append("%s (%s): %s did not run [%s]" % (name, outcome["kind"], unit["value"], where))
        outcome_reports.append({"name": name, "kind": outcome["kind"], "row": outcome.get("row"),
                                "units": units, "covered": covered, "count": len(units)})

    for exclusion in exclusions:
        if "outcome" in exclusion and _exclusion_key(exclusion) not in used:
            stale.append("%s: the exclusion of %s%s matches nothing on this lane" % (
                exclusion["outcome"], exclusion["value"],
                " (%s)" % exclusion["text"] if exclusion.get("text") else ""))

    verdict = "PASS"
    if gaps:
        verdict = "GAP"
    if stale:
        verdict = "STALE" if not gaps else "GAP"
    report = {
        "schema": REPORT_SCHEMA,
        "verdict": verdict,
        "denominator": {"scope_files": len(scope), "instrumented_files": instrumented,
                        "excluded_files": sorted(path_exclusions)},
        "totals": totals,
        "groups": groups,
        "files": per_file,
        "outcomes": outcome_reports,
        "gaps": gaps,
        "stale": stale,
    }
    return report


def render(report):
    lines = []
    den = report["denominator"]
    lines.append("DIAG coverage: %d scope files, %d instrumented on this lane, %d excluded by path"
                 % (den["scope_files"], den["instrumented_files"], len(den["excluded_files"])))
    for metric in METRICS:
        total = report["totals"][metric]
        pct = 100.0 * total["covered"] / total["count"] if total["count"] else 0.0
        lines.append("  %-9s %6d of %6d ran (%.1f%%)" % (metric, total["covered"], total["count"], pct))
    for name, group in report.get("groups", {}).items():
        lines.append("  group %s: %d of %d files instrumented; regions %d/%d, branches %d/%d" % (
            name, group["instrumented"], group["files"], group["regions"]["covered"], group["regions"]["count"],
            group["branches"]["covered"], group["branches"]["count"]))
    for outcome in report["outcomes"]:
        lines.append("  %-16s %-34s %3d of %3d" % (outcome["kind"], outcome["name"], outcome["covered"],
                                                   outcome["count"]))
    for gap in report["gaps"]:
        lines.append("GAP: " + gap)
    for item in report["stale"]:
        lines.append("STALE: " + item)
    lines.append("VERDICT %s" % report["verdict"])
    return "\n".join(lines)


# --------------------------------------------------------------------------------------------------------------------
# Lane run


def read_cache(build_dir):
    cache = {}
    path = Path(build_dir) / "CMakeCache.txt"
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise Failure("LANE", "no CMakeCache.txt in %s: %s" % (build_dir, error)) from error
    for line in text.splitlines():
        match = re.match(r"^([A-Za-z0-9_]+):[A-Z_]+=(.*)$", line)
        if match:
            cache[match.group(1)] = match.group(2)
    return cache


def find_tool(name, llvm_bin, cache):
    candidates = []
    if llvm_bin:
        candidates.append(Path(llvm_bin) / name)
    compiler = cache.get("CMAKE_CXX_COMPILER", "")
    resolved = shutil.which(compiler) if compiler else None
    if resolved:
        real = Path(os.path.realpath(resolved))
        candidates.append(real.parent / name)
        version = re.search(r"(\d+)", subprocess.run([resolved, "--version"], capture_output=True, text=True,
                                                     check=False).stdout or "")
        if version:
            found = shutil.which("%s-%s" % (name, version.group(1)))
            if found:
                candidates.append(Path(found))
    found = shutil.which(name)
    if found:
        candidates.append(Path(found))
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise Failure("TOOL_MISSING", "%s not found (pass --llvm-bin)" % name)


def select_tests(registry, build_dir, show_only):
    """CTest numbers and names of the registry's test set; every program must register at least one test."""
    build = Path(build_dir).resolve()
    programs = {}
    for program in registry["programs"]:
        programs[str((build / program).resolve())] = program
    wanted_names = set(registry.get("ctests", []))
    numbers = []
    names = []
    per_program = {p: 0 for p in registry["programs"]}
    commands = set()
    for index, test in enumerate(show_only.get("tests", []), start=1):
        command = test.get("command") or []
        if not command:
            continue
        exe = str(Path(command[0]).resolve())
        if exe in programs or test.get("name") in wanted_names:
            numbers.append(index)
            names.append(test["name"])
            commands.add(exe)
            if exe in programs:
                per_program[programs[exe]] += 1
    zero = [p for p, n in per_program.items() if n == 0]
    if zero:
        raise Failure("RUN_FAILED", "no CTest registered for %s (not built, or not a test program)" % ", ".join(zero))
    missing = wanted_names - set(names)
    if missing:
        raise Failure("RUN_FAILED", "named CTests not registered: %s" % ", ".join(sorted(missing)))
    return numbers, names, sorted(commands)


def read_junit(path, names):
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as error:
        raise Failure("RUN_FAILED", "cannot read the JUnit %s: %s" % (path, error)) from error
    seen = {}
    for case in tree.getroot().iter("testcase"):
        seen[case.get("name")] = case.get("status")
    problems = []
    for name in names:
        status = seen.get(name)
        if status is None:
            problems.append("%s: not in the JUnit" % name)
        elif status != "run":
            problems.append("%s: %s" % (name, status))
    return problems


def read_catch2_junit(path):
    """Problems in a Catch2 JUnit report: it must exist, hold at least one test case and no failure or error."""
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as error:
        return ["cannot read %s: %s" % (path, error)]
    cases = list(tree.getroot().iter("testcase"))
    if not cases:
        return ["%s holds no test case" % path]
    problems = []
    for case in cases:
        if case.find("failure") is not None or case.find("error") is not None:
            problems.append("%s: failed" % case.get("name"))
    return problems


def run_direct(registry, build, raw, out, timeout):
    """Run the registry's direct Catch2 programs once under the profile; return their resolved paths."""
    ran = []
    env = dict(os.environ)
    env["LLVM_PROFILE_FILE"] = str(raw / "%p-%m.profraw")
    for index, program in enumerate(registry.get("direct", [])):
        exe = build / program
        if not exe.is_file():
            raise Failure("RUN_FAILED", "direct program %s is not built" % program)
        report = out / ("direct-%d.xml" % index)
        try:
            result = subprocess.run([str(exe), "--reporter", "junit", "--out", str(report)], cwd=out, env=env,
                                    capture_output=True, text=True, timeout=timeout, check=False)
        except subprocess.TimeoutExpired as error:
            raise Failure("RUN_FAILED", "direct program %s timed out" % program) from error
        problems = read_catch2_junit(report)
        if result.returncode != 0 or problems:
            raise Failure("RUN_FAILED", "direct program %s did not pass (exit %d): %s" % (
                program, result.returncode, "; ".join(problems[:10])))
        ran.append(str(exe.resolve()))
    return ran


def run_lane(registry, build_dir, out_dir, llvm_bin, timeout):
    build = Path(build_dir).resolve()
    cache = read_cache(build)
    if cache.get("CRD_ENABLE_COVERAGE", "").upper() not in ("ON", "TRUE", "1", "YES"):
        raise Failure("LANE", "%s is not a coverage configuration (CRD_ENABLE_COVERAGE is not ON)" % build)
    source_root = cache.get("CMAKE_HOME_DIRECTORY") or str(ROOT)
    profdata = find_tool("llvm-profdata", llvm_bin, cache)
    llvm_cov = find_tool("llvm-cov", llvm_bin, cache)
    ctest = shutil.which("ctest")
    if not ctest:
        raise Failure("TOOL_MISSING", "ctest not found")
    out = Path(out_dir).resolve()
    if out.exists():
        shutil.rmtree(out)
    raw = out / "raw"
    raw.mkdir(parents=True)
    shown = subprocess.run([ctest, "--show-only=json-v1"], cwd=build, capture_output=True, text=True, check=False)
    if shown.returncode != 0:
        raise Failure("RUN_FAILED", "ctest --show-only failed: %s" % shown.stderr.strip())
    numbers, names, commands = select_tests(registry, build, json.loads(shown.stdout))
    env = dict(os.environ)
    env["LLVM_PROFILE_FILE"] = str(raw / "%p-%m.profraw")
    junit = out / "ctest.xml"
    log = out / "ctest.log"
    with open(log, "w", encoding="utf-8") as handle:
        subprocess.run([ctest, "-I", "0,0,0," + ",".join(str(n) for n in numbers), "--output-junit", str(junit),
                        "-j", str(os.cpu_count() or 4), "--timeout", str(timeout)],
                       cwd=build, env=env, stdout=handle, stderr=subprocess.STDOUT, check=False)
    problems = read_junit(junit, names)
    if problems:
        raise Failure("RUN_FAILED", "the test set did not pass (%d of %d): %s" % (
            len(problems), len(names), "; ".join(problems[:10])))
    direct = run_direct(registry, build, raw, out, timeout)
    profiles = sorted(raw.glob("*.profraw"))
    if not profiles:
        raise Failure("RUN_FAILED", "the run wrote no raw profile")
    merged = out / "merged.profdata"
    merge = subprocess.run([profdata, "merge", "-sparse", "--failure-mode=any", "-o", str(merged)]
                           + [str(p) for p in profiles], capture_output=True, text=True, check=False)
    if merge.returncode != 0:
        raise Failure("RUN_FAILED", "llvm-profdata merge failed: %s" % merge.stderr.strip()[:2000])
    objects = list(commands) + direct
    for pattern in registry.get("objects", []):
        matches = sorted(p for p in build.glob(pattern) if p.is_file())
        if not matches:
            raise Failure("RUN_FAILED", "object pattern %s matches no built file" % pattern)
        objects.extend(str(p) for p in matches)
    objects = sorted(set(objects))
    scope = scope_files(registry, ROOT)
    sources = [str(Path(source_root) / f) for f in sorted(scope)]
    command = [llvm_cov, "export", "-format=text", "-skip-functions", "-instr-profile", str(merged), objects[0]]
    for obj in objects[1:]:
        command += ["-object", obj]
    command += sources
    exported = out / "export.json"
    with open(exported, "w", encoding="utf-8") as handle:
        result = subprocess.run(command, stdout=handle, stderr=subprocess.PIPE, text=True, check=False)
    if result.returncode != 0:
        raise Failure("RUN_FAILED", "llvm-cov export failed: %s" % result.stderr.strip()[:2000])
    export = json.loads(exported.read_text(encoding="utf-8"))
    report = evaluate(registry, export, ROOT, source_root)
    report["run"] = {"build_dir": str(build), "tests": len(names), "direct": registry.get("direct", []),
                     "profiles": len(profiles),
                     "objects": [os.path.relpath(o, build) for o in objects],
                     "compiler": cache.get("CMAKE_CXX_COMPILER", ""), "profdata": profdata, "llvm_cov": llvm_cov,
                     "export_warnings": result.stderr.strip()[:2000]}
    return report


# --------------------------------------------------------------------------------------------------------------------
# Validation without a build


def validate(registry, root):
    """The registry against the tree: scope, enums, sites and exclusions that can be checked without a run. Every
    problem is collected; any one raises RegistryError naming them all."""
    scope = scope_files(registry, root)
    problems = []
    for outcome in registry.get("outcomes", []):
        name = outcome["name"]
        try:
            selected = outcome_files(outcome, scope)
            if outcome["kind"] == "error-path":
                header = Path(root) / outcome["header"]
                if not header.is_file():
                    raise RegistryError("%s: header %s does not exist" % (name, outcome["header"]))
                values = parse_enum(header.read_text(encoding="utf-8"), outcome["enum"])
            else:
                values = list(outcome["states"])
        except RegistryError as error:
            problems.append(str(error))
            continue
        produced = set()
        for path in selected:
            text = (Path(root) / path).read_text(encoding="utf-8", errors="replace")
            if outcome["kind"] == "error-path":
                produced.update(v for _, _, v, _ in producer_sites(text, outcome["enum"], values))
            else:
                produced.update(v for _, _, v, _ in transition_sites(text, outcome["site"], values))
        if outcome["kind"] == "state-transition" and not produced:
            problems.append("%s: the site pattern finds no transition" % name)
        mine = {e["value"]: e for e in registry.get("exclusions", []) if e.get("outcome") == name}
        for value in values:
            if value not in produced and (value not in mine or mine[value]["class"] not in NO_SITE_CLASSES):
                problems.append("%s: %s has no producing site in the scope and no %s exclusion" % (
                    name, value, " or ".join(NO_SITE_CLASSES)))
        for value, exclusion in mine.items():
            if value not in values:
                problems.append("%s: excluded value %s is not one of its values" % (name, value))
            elif exclusion["class"] in NO_SITE_CLASSES and value in produced:
                problems.append("%s: %s is excluded as %s but has a producing site" % (
                    name, value, exclusion["class"]))
    for exclusion in registry.get("exclusions", []):
        if "path" in exclusion and exclusion["path"] not in scope:
            problems.append("excluded path %s is not a scope file" % exclusion["path"])
    if problems:
        raise RegistryError("%d problem(s):\n  " % len(problems) + "\n  ".join(problems))
    return scope


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--registry", default=str(DEFAULT_REGISTRY))
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--build-dir")
    parser.add_argument("--export", help="evaluate an existing llvm-cov export instead of running the lane")
    parser.add_argument("--source-root", help="the source directory the export's file names start with")
    parser.add_argument("--out", help="working directory for profiles and the export (default <build>/diag-coverage)")
    parser.add_argument("--json", help="write the report here")
    parser.add_argument("--llvm-bin", help="directory holding llvm-profdata and llvm-cov")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TEST_TIMEOUT)
    args = parser.parse_args(argv)
    try:
        registry = load_registry(args.registry)
        if args.validate:
            scope = validate(registry, ROOT)
            print("registry ok: %d scope files, %d outcome sets, %d exclusions" % (
                len(scope), len(registry.get("outcomes", [])), len(registry.get("exclusions", []))))
            return EXIT["PASS"]
        if args.export:
            export = json.loads(Path(args.export).read_text(encoding="utf-8"))
            report = evaluate(registry, export, ROOT, args.source_root)
        elif args.build_dir:
            out = args.out or str(Path(args.build_dir) / "diag-coverage")
            report = run_lane(registry, args.build_dir, out, args.llvm_bin, args.timeout)
        else:
            parser.error("one of --validate, --build-dir or --export is required")
            return EXIT["REGISTRY"]
    except RegistryError as error:
        print("REGISTRY: %s" % error, file=sys.stderr)
        return EXIT["REGISTRY"]
    except Failure as failure:
        print("%s: %s" % (failure.verdict, failure), file=sys.stderr)
        return EXIT[failure.verdict]
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print(render(report))
    return EXIT[report["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
