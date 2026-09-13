#!/usr/bin/env python3
"""Two-sided gate for registered third-party failures.

The register (docs/third-party-defects.md) lists, per CI lane, the exact CTest names that fail because of a defect
reproduced outside Cerid code. This gate reads CTest's JUnit output for the lane and exits 0 only when the set of
failed tests equals the registered set: an unexpected failure, a registered test that passed, was skipped or did not
run, and a CTest error without a matching failure all fail the gate. Lanes without entries require zero failures and
propagate CTest's own exit code, so the gate never masks a CTest problem. Nothing is suppressed, filtered or skipped:
the tests run and the sanitizer stays strict; only the lane verdict consults the register.
"""
from pathlib import Path
import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REGISTER = ROOT / 'docs/third-party-defects.md'
MARKER = '<!-- registered-failures -->'
DEFECT_ID = re.compile(r'^TP-[0-9]+$')


class RegisterError(ValueError):
    """The register document or the JUnit evidence cannot support a verdict."""


def load_register(path):
    """Return the parsed register: {'lanes': {lane: [{'test': name, 'defect': 'TP-n'}, ...]}}."""
    text = Path(path).read_text(encoding='utf-8')
    if text.count(MARKER) != 1:
        raise RegisterError(f'{path}: expected exactly one {MARKER} marker')
    after = text.split(MARKER, 1)[1]
    match = re.match(r'\s*```json\r?\n(.*?)\r?\n```', after, re.S)
    if match is None:
        raise RegisterError(f'{path}: the marker must be followed by a fenced json block')
    try:
        data = json.loads(match.group(1))
    except json.JSONDecodeError as error:
        raise RegisterError(f'{path}: registered-failures block is not valid JSON: {error}') from error
    lanes = data.get('lanes') if isinstance(data, dict) else None
    if not isinstance(lanes, dict):
        raise RegisterError(f'{path}: registered-failures block must be an object with a "lanes" object')
    anchors = set(re.findall(r'<a id="(tp-[0-9]+)"></a>', text))
    for lane, entries in lanes.items():
        if not isinstance(lane, str) or not lane or not isinstance(entries, list):
            raise RegisterError(f'{path}: lane keys must be strings and their values lists')
        names = []
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != {'test', 'defect'}:
                raise RegisterError(f'{path}: lane {lane}: entries carry exactly "test" and "defect"')
            if not isinstance(entry['test'], str) or not entry['test'].strip():
                raise RegisterError(f'{path}: lane {lane}: empty test name')
            if not isinstance(entry['defect'], str) or not DEFECT_ID.match(entry['defect']):
                raise RegisterError(f'{path}: lane {lane}: defect ids look like TP-1')
            if entry['defect'].lower() not in anchors:
                raise RegisterError(f'{path}: lane {lane}: {entry["defect"]} has no <a id="{entry["defect"].lower()}"> entry')
            names.append(entry['test'])
        if len(names) != len(set(names)):
            raise RegisterError(f'{path}: lane {lane}: duplicate test names')
    return data


def registered(register, lane):
    """Map registered test name -> defect id for one lane (empty for lanes without entries)."""
    return {entry['test']: entry['defect'] for entry in register['lanes'].get(lane, [])}


def junit_results(path):
    """Map CTest test name -> 'passed' | 'failed' | 'skipped' from a CTest --output-junit document."""
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as error:
        raise RegisterError(f'CTest did not produce valid JUnit evidence: {error}') from error
    results = {}
    for case in tree.getroot().iter('testcase'):
        name = case.get('name')
        if not name or name in results:
            raise RegisterError('JUnit evidence has a missing or duplicate test name')
        if case.find('skipped') is not None or case.get('status') in {'notrun', 'disabled'}:
            results[name] = 'skipped'
        elif case.find('failure') is not None or case.find('error') is not None or case.get('status') == 'fail':
            results[name] = 'failed'
        else:
            results[name] = 'passed'
    if not results:
        raise RegisterError('Zero CTest results cannot qualify a lane')
    return results


def evaluate(expected, results, ctest_exit):
    """Return (exit_code, report_lines). Exit 0 only when the failing set equals the registered set."""
    failed = {name for name, state in results.items() if state == 'failed'}
    unexpected = sorted(failed - set(expected))
    not_failing = sorted(name for name in expected if results.get(name) != 'failed')
    lines = [f'registered failures for this lane: {len(expected)}; observed failures: {len(failed)}; '
             f'results: {len(results)}; ctest exit: {ctest_exit}']
    for name in sorted(expected):
        state = results.get(name, 'absent')
        lines.append(f'  registered {expected[name]}: {state:7s} {name}')
    for name in unexpected:
        lines.append(f'  UNEXPECTED FAILURE: {name}')
    for name in not_failing:
        lines.append(f'  UNEXPECTED {results.get(name, "absent").upper()}: registered {expected[name]} did not fail: {name}')
    if unexpected or not_failing:
        lines.append('gate: FAIL (the failing set differs from the register; fix the defect, or retire the entry)')
        return 1, lines
    if not expected:
        lines.append('gate: no registered failures for this lane; CTest exit propagated')
        return ctest_exit, lines
    if ctest_exit not in (0, 8):
        lines.append(f'gate: FAIL (CTest exit {ctest_exit} is not a plain test failure)')
        return ctest_exit, lines
    lines.append('gate: PASS (exactly the registered third-party failures, sanitizer unsuppressed)')
    return 0, lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--register', default=str(DEFAULT_REGISTER), help='register document (default: %(default)s)')
    parser.add_argument('--lane', required=True, help='CI lane key, e.g. the CMake preset name')
    parser.add_argument('--junit', required=True, help='CTest --output-junit document for the lane')
    parser.add_argument('--ctest-exit', type=int, default=0, help='exit code CTest returned for the lane')
    args = parser.parse_args(argv)
    try:
        expected = registered(load_register(args.register), args.lane)
        results = junit_results(args.junit)
    except RegisterError as error:
        print(f'gate: ERROR {error}', file=sys.stderr)
        return 2
    code, lines = evaluate(expected, results, args.ctest_exit)
    print('\n'.join(lines))
    return code


if __name__ == '__main__':
    sys.exit(main())
