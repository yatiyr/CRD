#!/usr/bin/env python3
"""Two-sided gate for registered third-party failures.

The register (docs/third-party-defects.md) lists, per CI lane, the exact CTest names that fail because of a defect
reproduced outside Cerid code. This gate reads CTest's JUnit output for the lane and exits 0 only when the set of
failed tests equals the registered set: an unexpected failure, a registered test that passed, was skipped or did not
run, and a CTest error without a matching failure all fail the gate. Lanes without entries require zero failures and
propagate CTest's own exit code, so the gate never masks a CTest problem. Nothing is suppressed, filtered or skipped:
the tests run and the sanitizer stays strict; only the lane verdict consults the register.

An entry may carry a "signature", a regular expression its failing test's output (the JUnit failure and system-out)
must hold, with no failure report (Catch2's "FAILED:", a sanitizer's "==pid==ERROR") outside its matches: any other
failure of that test is unexpected. An "intermittent" entry (it requires a signature) is a defect whose Cerid-free
reproduction fails only on some runs: its test may pass or fail with the signature, but a skip or an absence still
fails the gate. A pass cannot retire it; the entry names the Cerid-free check whose clean runs do.

A lane split into shards (docs/design/ci-tiers.md#parallelism-and-shards) gates each shard on the registered tests
its own inventory selects, after checking that every registered test is in the lane's inventory: the shards'
inventories partition the lane's, so the shards' gates together hold the whole lane to its register.
"""
from pathlib import Path
import argparse
import collections
import json
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REGISTER = ROOT / 'docs/third-party-defects.md'
MARKER = '<!-- registered-failures -->'
DEFECT_ID = re.compile(r'^TP-[0-9]+$')
ENTRY_KEYS = {'test', 'defect', 'signature', 'intermittent'}
FAILURE_REPORT = re.compile(r'FAILED:|==[0-9]+==ERROR')
Result = collections.namedtuple('Result', 'state output')


class RegisterError(ValueError):
    """The register document or the JUnit evidence cannot support a verdict."""


def load_register(path):
    """Return the parsed register: {'lanes': {lane: [{'test': name, 'defect': 'TP-n'[, 'signature': regex]
    [, 'intermittent': true]}, ...]}}."""
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
            if not isinstance(entry, dict) or not {'test', 'defect'} <= set(entry) <= ENTRY_KEYS:
                raise RegisterError(f'{path}: lane {lane}: entries carry "test" and "defect", and may carry '
                                    '"signature" and "intermittent"')
            if not isinstance(entry['test'], str) or not entry['test'].strip():
                raise RegisterError(f'{path}: lane {lane}: empty test name')
            if not isinstance(entry['defect'], str) or not DEFECT_ID.match(entry['defect']):
                raise RegisterError(f'{path}: lane {lane}: defect ids look like TP-1')
            if entry['defect'].lower() not in anchors:
                raise RegisterError(f'{path}: lane {lane}: {entry["defect"]} has no <a id="{entry["defect"].lower()}"> entry')
            check_signature(path, lane, entry)
            names.append(entry['test'])
        if len(names) != len(set(names)):
            raise RegisterError(f'{path}: lane {lane}: duplicate test names')
    return data


def check_signature(path, lane, entry):
    """Refuse a signature that is empty or does not compile, and an intermittent entry without one."""
    if 'intermittent' in entry and entry['intermittent'] is not True:
        raise RegisterError(f'{path}: lane {lane}: "intermittent" is true or absent: {entry["test"]}')
    if 'signature' not in entry:
        if 'intermittent' in entry:
            raise RegisterError(f'{path}: lane {lane}: an intermittent entry needs a "signature": {entry["test"]}')
        return
    if not isinstance(entry['signature'], str) or not entry['signature'].strip():
        raise RegisterError(f'{path}: lane {lane}: empty signature: {entry["test"]}')
    try:
        re.compile(entry['signature'])
    except re.error as error:
        raise RegisterError(f'{path}: lane {lane}: signature does not compile ({error}): {entry["test"]}') from error


def registered(register, lane):
    """Map registered test name -> its entry for one lane (empty for lanes without entries)."""
    return {entry['test']: entry for entry in register['lanes'].get(lane, [])}


def signature_holds(entry, output):
    """Whether a failing registered test's output is its entry's failure: always without a signature; with one, at
    least one match and no failure report outside the matches."""
    if 'signature' not in entry:
        return True
    pattern = re.compile(entry['signature'])
    if pattern.search(output) is None:
        return False
    return FAILURE_REPORT.search(pattern.sub('', output)) is None


def junit_results(path):
    """Map CTest test name -> Result(state, output) from a CTest --output-junit document: the state is 'passed',
    'failed' or 'skipped', the output the failure's message and text and the system-out text."""
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
            state = 'skipped'
        elif case.find('failure') is not None or case.find('error') is not None or case.get('status') == 'fail':
            state = 'failed'
        else:
            state = 'passed'
        output = []
        for child in case:
            if child.tag in {'failure', 'error', 'system-out', 'system-err'}:
                output += [child.get('message') or '', child.text or '']
        results[name] = Result(state, '\n'.join(part for part in output if part))
    if not results:
        raise RegisterError('Zero CTest results cannot qualify a lane')
    return results


def inventory_names(path):
    """The test names of a `ctest --show-only=json-v1` inventory."""
    try:
        tests = json.loads(Path(path).read_text(encoding='utf-8'))['tests']
        return {test['name'] for test in tests}
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RegisterError(f'{path}: not a CTest json-v1 inventory: {error}') from error


def shard_share(expected, selection, lane):
    """The registered tests one shard must see fail, and a report line for each one no shard can run."""
    outside = sorted(name for name in expected if name not in lane)
    lines = [f'  UNEXPECTED ABSENT: registered {expected[name]["defect"]} is not in the lane inventory: {name}'
             for name in outside]
    return {name: entry for name, entry in expected.items() if name in selection}, lines


def evaluate(expected, results, ctest_exit, absent=()):
    """Return (exit_code, report_lines). Exit 0 only when the failing set equals the registered set, less the
    intermittent entries' tests that passed, and each registered failure holds its entry's signature."""
    states = {name: result.state for name, result in results.items()}
    failed = {name for name, state in states.items() if state == 'failed'}
    # A test that exits with its declared skip code (a capability the lane lacks, e.g. no GPU device) is counted
    # apart: it is neither a pass nor a failure, so the report shows it rather than folding it into the results.
    skipped = sum(1 for state in states.values() if state == 'skipped')
    unexpected = sorted(failed - set(expected))
    foreign = sorted(name for name in failed & set(expected)
                     if not signature_holds(expected[name], results[name].output))
    not_failing = sorted(name for name, entry in expected.items() if states.get(name) != 'failed'
                         and not (entry.get('intermittent') and states.get(name) == 'passed'))
    lines = [f'registered failures for this lane: {len(expected)}; observed failures: {len(failed)}; '
             f'skipped: {skipped}; results: {len(results)}; ctest exit: {ctest_exit}']
    for name in sorted(expected):
        kind = ' (intermittent)' if expected[name].get('intermittent') else ''
        lines.append(f'  registered {expected[name]["defect"]}: {states.get(name, "absent"):7s} {name}{kind}')
    for name in unexpected:
        lines.append(f'  UNEXPECTED FAILURE: {name}')
    for name in foreign:
        lines.append(f'  UNEXPECTED FAILURE: {name} (the output does not hold {expected[name]["defect"]}\'s '
                     'signature)')
    for name in not_failing:
        lines.append(f'  UNEXPECTED {states.get(name, "absent").upper()}: registered {expected[name]["defect"]} '
                     f'did not fail: {name}')
    lines += absent
    if unexpected or foreign or not_failing or absent:
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
    parser.add_argument('--selection', help="json-v1 inventory of the tests this shard ran (with --lane-inventory)")
    parser.add_argument('--lane-inventory', help="json-v1 inventory of the whole lane (with --selection)")
    args = parser.parse_args(argv)
    if (args.selection is None) != (args.lane_inventory is None):
        parser.error('--selection and --lane-inventory go together')
    absent = []
    try:
        expected = registered(load_register(args.register), args.lane)
        results = junit_results(args.junit)
        if args.selection is not None:
            expected, absent = shard_share(expected, inventory_names(args.selection),
                                           inventory_names(args.lane_inventory))
    except RegisterError as error:
        print(f'gate: ERROR {error}', file=sys.stderr)
        return 2
    code, lines = evaluate(expected, results, args.ctest_exit, absent)
    print('\n'.join(lines))
    return code


if __name__ == '__main__':
    sys.exit(main())
