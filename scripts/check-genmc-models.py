#!/usr/bin/env python3
"""Run the DIAG.4b GenMC weak-memory model in its three variants and hold each run to its expected verdict.

The model (tests/support/diag/models/counter_publish_reclaim.c) is checked with the GenMC that
scripts/install-genmc.py built from the pinned source, under GenMC's default RC11 model, without estimation:
  repaired       `genmc --disable-estimation -- <model>`
                 must exit 0 with "No errors were detected" and a positive count of complete executions;
  handshake      `genmc --disable-estimation -print-error-trace -- -DCRD_MODEL_BROKEN_HANDSHAKE <model>`
                 (park_finalized removed) must fail with an access to freed memory or the failed generation
                 assertion `... == snapshot`;
  early-drained  `genmc --disable-estimation -print-error-trace -- -DCRD_MODEL_EARLY_DRAINED <model>`
                 (drained stored before the drain loop) must fail with an access to freed memory.
The two seeded variants are the teeth: a model that cannot find a seeded handshake bug proves nothing about the
repaired one, so a seeded variant that verifies, or fails for another reason, fails this check. Prints every verdict
with the explored executions and wall time; `--json` writes them for the lane evidence.
Contract: docs/design/runtime-diagnostics.md (DIAG.4b).
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / 'tests/support/diag/models/counter_publish_reclaim.c'
CLEAN = 'No errors were detected'
UNSUCCESSFUL = 'Verification unsuccesful'  # GenMC's own spelling
FREED = re.compile(r'^Error(?: detected)?: Attempt to access freed memory', re.M)
GENERATION = re.compile(r'^Error(?: detected)?: Safety violation[\s\S]*assert\(.*==\s*snapshot\)', re.M)
EXECUTIONS = re.compile(r'Number of complete executions explored:\s*(\d+)')
BLOCKED = re.compile(r'Number of blocked executions seen:\s*(\d+)')
WALL = re.compile(r'Total wall-clock time:\s*([0-9.]+)s')
ERROR_LINE = re.compile(r'^Error(?: detected)?:\s*(.+)$', re.M)

# variant -> (defines, the seeded defects it must report; empty for the repaired model)
VARIANTS = {
    'repaired': ((), ()),
    'handshake': (('-DCRD_MODEL_BROKEN_HANDSHAKE',), (('freed memory', FREED), ('generation assert', GENERATION))),
    'early-drained': (('-DCRD_MODEL_EARLY_DRAINED',), (('freed memory', FREED),)),
}


def summarize(output):
    """Pull the counts and the first reported error out of one GenMC transcript."""
    executions = EXECUTIONS.search(output)
    blocked = BLOCKED.search(output)
    wall = WALL.search(output)
    error = ERROR_LINE.search(output)
    return {
        'executions': int(executions.group(1)) if executions else None,
        'blocked': int(blocked.group(1)) if blocked else 0,
        'genmc_seconds': float(wall.group(1)) if wall else None,
        'error': error.group(1).strip() if error else None,
    }


def judge(variant, exit_code, output):
    """Return (passed, reason) for one variant's exit code and transcript."""
    facts = summarize(output)
    defects = VARIANTS[variant][1]
    if not defects:
        if exit_code != 0:
            return False, f'exit {exit_code}; expected 0 (error: {facts["error"]})'
        if CLEAN not in output:
            return False, f'"{CLEAN}" is missing'
        if not facts['executions']:
            return False, 'no complete execution was explored'
        return True, f'no error in {facts["executions"]} complete executions'
    if exit_code == 0:
        return False, 'exit 0: the seeded defect was not found (the model has no teeth)'
    if UNSUCCESSFUL not in output:
        return False, f'exit {exit_code} without "{UNSUCCESSFUL}" (a tool failure, not a verdict)'
    found = [name for name, pattern in defects if pattern.search(output)]
    if not found:
        return False, f'an error other than the seeded defect was reported: {facts["error"]}'
    return True, f'seeded defect found: {found[0]} ({facts["error"]})'


def command_for(genmc, variant, model):
    defines = VARIANTS[variant][0]
    command = [str(genmc), '--disable-estimation']
    if defines:
        command.append('-print-error-trace')
    return command + ['--', *defines, str(model)]


def run_variant(genmc, variant, model, timeout):
    command = command_for(genmc, variant, model)
    started = time.monotonic()
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        exit_code, output = completed.returncode, completed.stdout + completed.stderr
    except subprocess.TimeoutExpired as expired:
        exit_code = None
        parts = (expired.stdout or '', expired.stderr or '')
        output = ''.join(p.decode(errors='replace') if isinstance(p, bytes) else p for p in parts)
    seconds = time.monotonic() - started
    if exit_code is None:
        passed, reason = False, f'timed out after {timeout} s'
    else:
        passed, reason = judge(variant, exit_code, output)
    record = {'variant': variant, 'command': command, 'exit': exit_code, 'wall_seconds': round(seconds, 2),
              'passed': passed, 'reason': reason}
    record.update(summarize(output))
    return record, output


def genmc_version(genmc):
    result = subprocess.run([str(genmc), '--version'], capture_output=True, text=True)
    return ' '.join((result.stdout + result.stderr).split())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--genmc', default=os.environ.get('GENMC') or shutil.which('genmc'),
                        help='genmc binary (default: $GENMC, then PATH)')
    parser.add_argument('--model', type=Path, default=MODEL)
    parser.add_argument('--timeout', type=int, default=600, help='seconds allowed per variant')
    parser.add_argument('--json', type=Path, help='write every record here')
    parser.add_argument('--log-dir', type=Path, help='write each full GenMC transcript here')
    args = parser.parse_args(argv)
    if not args.genmc or not Path(args.genmc).is_file():
        print('FAIL: no genmc binary; build it with scripts/install-genmc.py')
        return 2
    version = genmc_version(args.genmc)
    print(f'GenMC: {version}')
    records = []
    for variant in VARIANTS:
        record, output = run_variant(args.genmc, variant, args.model, args.timeout)
        records.append(record)
        if args.log_dir:
            args.log_dir.mkdir(parents=True, exist_ok=True)
            (args.log_dir / f'genmc-{variant}.log').write_text(output, encoding='utf-8')
        print(f'{"PASS" if record["passed"] else "FAIL"}: {variant}: {record["reason"]} '
              f'(exit {record["exit"]}, {record["executions"]} complete and {record["blocked"]} blocked '
              f'executions, {record["wall_seconds"]} s)')
        if not record['passed']:
            sys.stdout.write(output[-4000:])
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({'genmc': version, 'model': args.model.name, 'runs': records}, indent=2)
                             + '\n', encoding='utf-8')
    if all(record['passed'] for record in records):
        print('PASS')
        return 0
    return 1


if __name__ == '__main__':
    sys.exit(main())
