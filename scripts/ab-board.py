#!/usr/bin/env python3
"""The fast A/B board: two or more arms of a runtime benchmark, interleaved, judged by paired ratios with their spread.

    python3 scripts/ab-board.py --cmd NAME=COMMAND [--cmd ...] --arm LABEL=NAME:REGEX [--arm ...] --reference LABEL
                                [--same NAME,NAME[,...]:REGEX ...] [--cpus LIST] [--warmup N] [--min-reps N]
                                [--max-reps N] [--band F] [--higher-is-better] [--log-dir DIR] [--json PATH]

A command is one process of a bench program (a shell command line, run from the current directory). An arm is the
values one command prints: REGEX, matched against every line of its output, names a `key` group (a size, a case) and
a `value` group (a time, or a rate with --higher-is-better). Several arms may read one command, so a program that
times Cerid and its peer in the same process (bench_fft_f32: the planned forward and MKL on every repetition) gives
both arms from one run; two builds of one program (HEAD and a candidate, the pre-row engine and HEAD) are two
commands. Every repetition runs each command once, pinned with `taskset -c LIST`, in an order rotated each repetition
so drift lands on every arm alike; the first --warmup repetitions are run and discarded.

Each arm is compared with the reference arm key by key, paired by repetition: the speed ratio of one repetition is the
reference's value over the arm's (the arm's over the reference's with --higher-is-better), so above 1 the arm is
faster. The board reports the median ratio and its percentile-bootstrap 95% interval (diag-overhead.py's, fixed
seed) and a verdict against the noise band F (default 0.02):
  faster      the interval's lower end is above 1 + F;
  slower      its upper end is below 1 - F;
  level       the whole interval lies within [1 - F, 1 + F];
  unresolved  it straddles a band edge: more repetitions are needed, or the effect is the size of the noise.
After --min-reps repetitions the board stops as soon as no cell is unresolved, else at --max-reps. A command that
fails, or prints no value or a key twice for an arm, stops the board: an instrument failure is never a measurement.
--same names commands that must compute the same results: REGEX's `key` and `value` groups read a digest of each
key's output (bench_fft_f32's and prof_fft_kernel's `bits`), and the board stops when two of the named commands, or
two repetitions of one, print different values at a key, or a named command prints none (a faster wrong answer is
not a win: HEAD against a candidate that claims the same bits).
The board prints a markdown table per comparison (docs/bench), saves each process's output with --log-dir
(rep<k>-<command>.log) and every value with --json (schema cerid-ab-board/1).
Method: docs/bench/README.md#fast-ab-boards.
"""
from pathlib import Path
import argparse
import datetime
import importlib.util
import json
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('diag_overhead', ROOT / 'scripts/diag-overhead.py')
_overhead = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_overhead)
median = _overhead.median
bootstrap_interval = _overhead.bootstrap_interval
arm_order = _overhead.arm_order
SCHEMA = 'cerid-ab-board/1'


class BoardError(RuntimeError):
    """The board cannot be measured: a bad argument or an instrument failure."""


def parse_assignment(text, what):
    name, separator, rest = text.partition('=')
    if not separator or not name or not rest:
        raise BoardError('%s must be NAME=VALUE: %r' % (what, text))
    return name, rest


def parse_arm(text, commands):
    label, rest = parse_assignment(text, '--arm')
    command, separator, pattern = rest.partition(':')
    if not separator or command not in commands:
        raise BoardError('--arm %s must read a --cmd by name (NAME:REGEX), not %r' % (label, rest))
    try:
        regex = re.compile(pattern, re.M)
    except re.error as error:
        raise BoardError('--arm %s: bad regular expression: %s' % (label, error))
    if not {'key', 'value'} <= set(regex.groupindex):
        raise BoardError('--arm %s: the regular expression needs the groups key and value' % label)
    return label, command, regex


def arm_values(label, regex, text):
    """{key: value} of one arm in one process's output, in output order."""
    values = {}
    for match in regex.finditer(text):
        key = match.group('key')
        if key in values:
            raise BoardError('arm %s: key %s printed twice in one run' % (label, key))
        try:
            values[key] = float(match.group('value'))
        except ValueError:
            raise BoardError('arm %s: key %s has no number: %r' % (label, key, match.group('value')))
    if not values:
        raise BoardError('arm %s: no line of its command matched' % label)
    return values


def parse_same(text, commands):
    """(command names, regex) of one --same NAME,NAME[,...]:REGEX."""
    names, separator, pattern = text.partition(':')
    group = names.split(',') if separator else []
    if len(group) < 2 or len(set(group)) != len(group) or any(name not in commands for name in group):
        raise BoardError('--same must name two or more distinct --cmd (NAME,NAME:REGEX), not %r' % text)
    try:
        regex = re.compile(pattern, re.M)
    except re.error as error:
        raise BoardError('--same %s: bad regular expression: %s' % (names, error))
    if not {'key', 'value'} <= set(regex.groupindex):
        raise BoardError('--same %s: the regular expression needs the groups key and value' % names)
    return group, regex


def same_values(group, regex, outputs, seen):
    """Check one repetition's outputs of a --same group against each other and every earlier repetition (`seen`,
    {key: (command, value)}, updated)."""
    printed = {}
    for name in group:
        values = printed[name] = {}
        for match in regex.finditer(outputs[name]):
            key = match.group('key')
            if key in values:
                raise BoardError('--same: command %s printed key %s twice in one run' % (name, key))
            values[key] = match.group('value')
        if not values:
            raise BoardError('--same: no line of command %s matched' % name)
        if set(values) != set(printed[group[0]]):
            raise BoardError('--same: commands %s and %s printed different keys' % (group[0], name))
    for name, values in printed.items():
        for key, value in values.items():
            first = seen.setdefault(key, (name, value))
            if first[1] != value:
                raise BoardError('--same: command %s printed %s at key %s where %s printed %s' % (
                    name, value, key, first[0], first[1]))


def speed_ratio(reference, arm, higher_is_better):
    if higher_is_better:
        return arm / reference
    return reference / arm


def verdict(low, high, band):
    if low is None:
        return 'unresolved'
    if low > 1.0 + band:
        return 'faster'
    if high < 1.0 - band:
        return 'slower'
    if low >= 1.0 - band and high <= 1.0 + band:
        return 'level'
    return 'unresolved'


def compare(reps, reference, arm, band, higher_is_better):
    """One comparison's cells: per key the paired ratios of every repetition, their median, interval and verdict."""
    cells = []
    for key in reps[0][reference]:
        ratios = [speed_ratio(rep[reference][key], rep[arm][key], higher_is_better)
                  for rep in reps if key in rep[reference] and key in rep[arm]]
        low, high = bootstrap_interval(ratios)
        cells.append({'key': key, 'reference': median([rep[reference][key] for rep in reps]),
                      'arm': median([rep[arm][key] for rep in reps if key in rep[arm]]), 'ratio': median(ratios),
                      'low': low, 'high': high, 'verdict': verdict(low, high, band), 'pairs': len(ratios)})
    return cells


def taskset_prefix(cpus):
    return 'taskset -c %s ' % shlex.quote(cpus) if cpus else ''


class Board:
    def __init__(self, commands, arms, reference, cpus=None, warmup=1, min_reps=5, max_reps=15, band=0.02,
                 higher_is_better=False, log_dir=None, runner=subprocess.run, out=sys.stdout, same=()):
        if reference not in [label for label, _, _ in arms]:
            raise BoardError('--reference %s is not an arm' % reference)
        if len(arms) < 2:
            raise BoardError('a board needs the reference and at least one other arm')
        if not 1 <= min_reps <= max_reps or min_reps < 2:
            raise BoardError('need 2 <= --min-reps <= --max-reps')
        self.commands = commands
        self.arms = arms
        self.reference = reference
        self.cpus = cpus
        self.warmup = warmup
        self.min_reps = min_reps
        self.max_reps = max_reps
        self.band = band
        self.higher_is_better = higher_is_better
        self.log_dir = Path(log_dir) if log_dir else None
        self.runner = runner
        self.out = out
        self.same = [(group, regex, {}) for group, regex in same]

    def run_command(self, name, label):
        line = taskset_prefix(self.cpus) + self.commands[name]
        result = self.runner(line, shell=True, capture_output=True, text=True)
        if self.log_dir is not None:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            (self.log_dir / ('%s-%s.log' % (label, name))).write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode != 0:
            raise BoardError('command %s exited %d: %s' % (name, result.returncode,
                                                           (result.stderr or result.stdout).strip()[-600:]))
        return result.stdout

    def repetition(self, index, label):
        outputs = {}
        for name in arm_order(list(self.commands), index):
            outputs[name] = self.run_command(name, label)
        for group, regex, seen in self.same:
            same_values(group, regex, outputs, seen)
        return {arm: arm_values(arm, regex, outputs[command]) for arm, command, regex in self.arms}

    def comparisons(self, reps):
        return {arm: compare(reps, self.reference, arm, self.band, self.higher_is_better)
                for arm, _, _ in self.arms if arm != self.reference}

    def run(self):
        for index in range(self.warmup):
            self.repetition(index, 'warmup%d' % (index + 1))
        reps = []
        stop = 'the maximum of %d repetitions' % self.max_reps
        for index in range(self.max_reps):
            reps.append(self.repetition(self.warmup + index, 'rep%d' % (index + 1)))
            if len(reps) >= self.min_reps:
                results = self.comparisons(reps)
                open_cells = sum(cell['verdict'] == 'unresolved' for cells in results.values() for cell in cells)
                print('[ab-board] repetition %d: %d unresolved cells' % (len(reps), open_cells), file=self.out)
                if open_cells == 0:
                    stop = 'every cell decided after %d repetitions' % len(reps)
                    break
        return {'schema': SCHEMA, 'date': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
                'commands': self.commands, 'arms': {arm: {'command': command, 'regex': regex.pattern}
                                                   for arm, command, regex in self.arms},
                'reference': self.reference, 'cpus': self.cpus, 'warmup': self.warmup, 'band': self.band,
                'higher_is_better': self.higher_is_better, 'stop': stop, 'repetitions': reps,
                'same': [{'commands': group, 'regex': regex.pattern, 'values': {key: value for key, (_, value)
                                                                                 in seen.items()}}
                         for group, regex, seen in self.same],
                'comparisons': self.comparisons(reps)}


def number(value):
    """Four significant digits, never in exponent form (a time in ns stays readable)."""
    if value is None:
        return '-'
    if abs(value) >= 1000:
        return '%.0f' % value
    return '%.4g' % value


def render(document):
    """Markdown: the method line, then one table per comparison."""
    unit = 'rate' if document['higher_is_better'] else 'time'
    lines = ['%d repetitions (%s), %d warm-up, CPUs %s, band %.0f%%; speed ratio = reference %s / arm %s, median '
             '[bootstrap 95%%].' % (len(document['repetitions']), document['stop'], document['warmup'],
                                    document['cpus'] or 'unpinned', document['band'] * 100, unit, unit)]
    if document['higher_is_better']:
        lines[0] = lines[0].replace('reference rate / arm rate', 'arm rate / reference rate')
    for same in document.get('same', []):
        lines[0] += ' %s printed the same results at every key of every repetition (%d keys).' % (
            ', '.join(same['commands']), len(same['values']))
    for arm, cells in document['comparisons'].items():
        lines += ['', '%s against %s:' % (arm, document['reference']), '',
                  '| key | %s | %s | speed ratio | verdict |' % (document['reference'], arm), '|---|---|---|---|---|']
        for cell in cells:
            lines.append('| %s | %s | %s | %s [%s-%s] | %s |' % (
                cell['key'], number(cell['reference']), number(cell['arm']), number(cell['ratio']),
                number(cell['low']), number(cell['high']), cell['verdict']))
    return '\n'.join(lines) + '\n'


def main(argv=None, out=sys.stdout):
    parser = argparse.ArgumentParser(prog='ab-board.py', description=__doc__.splitlines()[0])
    parser.add_argument('--cmd', action='append', default=[], required=True, help='NAME=COMMAND')
    parser.add_argument('--arm', action='append', default=[], required=True, help='LABEL=NAME:REGEX')
    parser.add_argument('--reference', required=True)
    parser.add_argument('--same', action='append', default=[], help='NAME,NAME[,...]:REGEX')
    parser.add_argument('--cpus', help='the taskset CPU list every process is pinned to')
    parser.add_argument('--warmup', type=int, default=1)
    parser.add_argument('--min-reps', type=int, default=5)
    parser.add_argument('--max-reps', type=int, default=15)
    parser.add_argument('--band', type=float, default=0.02)
    parser.add_argument('--higher-is-better', action='store_true')
    parser.add_argument('--log-dir')
    parser.add_argument('--json')
    args = parser.parse_args(argv)
    try:
        commands = dict(parse_assignment(text, '--cmd') for text in args.cmd)
        if len(commands) != len(args.cmd):
            raise BoardError('two --cmd share a name')
        arms = [parse_arm(text, commands) for text in args.arm]
        if len({label for label, _, _ in arms}) != len(arms):
            raise BoardError('two --arm share a label')
        board = Board(commands, arms, args.reference, cpus=args.cpus, warmup=args.warmup, min_reps=args.min_reps,
                      max_reps=args.max_reps, band=args.band, higher_is_better=args.higher_is_better,
                      log_dir=args.log_dir, out=out, same=[parse_same(text, commands) for text in args.same])
        document = board.run()
    except BoardError as error:
        print('ab-board: %s' % error, file=sys.stderr)
        return 1
    if args.json:
        Path(args.json).write_text(json.dumps(document, indent=1) + '\n', encoding='utf-8')
    out.write(render(document))
    return 0


if __name__ == '__main__':
    sys.exit(main())
