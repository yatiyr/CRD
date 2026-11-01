#!/usr/bin/env python3
"""Run the pinned Windows Vulkan SDK installer under one arm of REPO.3's option experiment, or judge the arms.

Contract: docs/design/test-instruments.md (the Vulkan SDK installer leg). The cold Windows lane's run of the pinned
installer (install-vulkan-sdk.py) prints Qt's `QFont::setPixelSize: Pixel size <= 0 (-1)` warning and fetches update
metadata from LunarG's remote repository (docs/sessions/2026-11-01-repo-3d-scoped-checks.md). The Qt Installer
Framework has no offline switch, so each arm adds candidate options to the production arguments on a fresh hosted
machine and records the installer's exit, both findings, the hosts its output names, its metadata cache and a
manifest of the installed tree. `judge` compares every arm with the baseline, discounting the paths on which two
baseline runs disagree; an arm whose options belong in install-vulkan-sdk.py exits 0, prints neither finding and
installs the baseline's tree.

    python scripts/vulkan-sdk-installer-check.py run --arm <name> --download-dir <dir> --evidence <dir>
    python3 scripts/vulkan-sdk-installer-check.py judge --evidence <dir> [--summary <file>]
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

_spec = importlib.util.spec_from_file_location('install_vulkan_sdk', Path(__file__).resolve().parent /
                                               'install-vulkan-sdk.py')
sdk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sdk)

# The two local repositories an arm may make the only one the fetch uses: an empty directory, and one whose
# Updates.xml lists no package.
EMPTY, UPDATES = 'empty-repository', 'updates-repository'
UPDATES_XML = ('<Updates>\n <ApplicationName>{AnyApplication}</ApplicationName>\n'
               ' <ApplicationVersion>1.0.0</ApplicationVersion>\n <Checksum>true</Checksum>\n</Updates>\n')
BASELINES = ('baseline', 'baseline-repeat')
# Arm name -> the options added to sdk.INSTALL_ARGUMENTS (EMPTY and UPDATES stand for those repositories' URIs).
ARMS = {
    'baseline': (),
    'baseline-repeat': (),
    'verbose': ('--verbose',),
    'platform-minimal': ('--platform', 'minimal'),
    'platform-offscreen': ('--platform', 'offscreen'),
    'temp-repository-empty': ('--set-temp-repository', EMPTY),
    'temp-repository-updates': ('--set-temp-repository', UPDATES),
    'minimal-updates': ('--platform', 'minimal', '--set-temp-repository', UPDATES),
    'offscreen-updates': ('--platform', 'offscreen', '--set-temp-repository', UPDATES),
}
QT_FONT = re.compile(r'QFont::setPixelSize')
REMOTE = re.compile(r'Fetching latest update information|meta information from remote repository')
URL = re.compile(r'\b(?:https?|ftp)://[^\s"\'<>]+')
TIMEOUT_SECONDS = 1500
LISTED = 20


def default_cache():
    return Path(os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData/Local') / 'cache/qt-installer-framework'


def listing(directory):
    """Relative path -> size of every file under `directory` (empty when it does not exist)."""
    directory = Path(directory)
    if not directory.is_dir():
        return {}
    return {path.relative_to(directory).as_posix(): path.stat().st_size
            for path in sorted(directory.rglob('*')) if path.is_file()}


def manifest(root):
    """Relative path -> [size, SHA-256] of every file of an installed tree."""
    return {relative: [size, pins.sha256(Path(root) / relative)] for relative, size in listing(root).items()}


def local_repositories(scratch):
    scratch = Path(scratch)
    (scratch / EMPTY).mkdir(parents=True, exist_ok=True)
    (scratch / UPDATES).mkdir(parents=True, exist_ok=True)
    (scratch / UPDATES / 'Updates.xml').write_text(UPDATES_XML, encoding='utf-8')
    return {name: (scratch / name).resolve().as_uri() for name in (EMPTY, UPDATES)}


def findings(output):
    lines = output.splitlines()
    return {'qt_font': [line for line in lines if QT_FONT.search(line)],
            'remote': [line for line in lines if REMOTE.search(line)],
            'hosts': sorted({urlsplit(url).netloc for url in URL.findall(output)} - {''})}


def run_arm(arm, download_dir, evidence, root=None, cache=None, archive=None, run=subprocess.run):
    """Install the pinned SDK with the arm's options on a machine that has none and write its evidence to
    `<evidence>/<arm>/` (log.txt, result.json, manifest.json). Returns the result."""
    if arm not in ARMS:
        raise pins.PinError(f'unknown arm {arm}; the arms are {", ".join(ARMS)}')
    pin = pins.entry('tools', 'vulkan-sdk-windows')
    root = Path(root or pin['install_root'])
    cache = Path(cache or default_cache())
    if root.exists():
        raise pins.PinError(f'{root} exists before the {arm} arm; an arm needs a machine without the SDK')
    out = Path(evidence) / arm
    out.mkdir(parents=True, exist_ok=True)
    uris = local_repositories(Path(download_dir) / 'installer-repositories')
    options = [uris.get(option, option) for option in ARMS[arm]]
    installer = pins.fetch(pin, download_dir, archive, label='Vulkan SDK installer')
    cache_before = listing(cache)
    started = time.monotonic()
    try:
        completed = run([str(installer), *sdk.INSTALL_ARGUMENTS, *options], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=TIMEOUT_SECONDS, check=False)
        exit_code, raw = completed.returncode, completed.stdout or b''
    except subprocess.TimeoutExpired as expired:
        exit_code, raw = 'timeout', expired.output or b''
    output = raw.decode('utf-8', errors='replace')
    (out / 'log.txt').write_text(output, encoding='utf-8')
    tree = manifest(root)
    (out / 'manifest.json').write_text(json.dumps(tree, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    canonical = json.dumps(tree, sort_keys=True).encode('utf-8')
    result = {'arm': arm, 'version': pin['version'], 'options': list(ARMS[arm]), 'arguments': options,
              'exit': exit_code, 'seconds': round(time.monotonic() - started, 1), **findings(output),
              'cache_before': cache_before, 'cache_after': listing(cache),
              'layout_missing': [relative for relative in sdk.LAYOUT if relative not in tree],
              'tree': {'files': len(tree), 'bytes': sum(size for size, _ in tree.values()),
                       'digest': hashlib.sha256(canonical).hexdigest()}}
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n', encoding='utf-8')
    print(output)
    print(json.dumps({key: result[key] for key in ('arm', 'arguments', 'exit', 'qt_font', 'remote', 'hosts',
                                                   'cache_after', 'layout_missing', 'tree')}, indent=1))
    return result


def differences(base, other, variable):
    """Paths added, removed and changed from `base` to `other`, leaving out the `variable` ones."""
    return {'added': sorted(set(other) - set(base) - variable),
            'removed': sorted(set(base) - set(other) - variable),
            'changed': sorted(path for path in (set(base) & set(other)) - variable if base[path] != other[path])}


def load(evidence, arm):
    folder = Path(evidence) / arm
    if not (folder / 'result.json').is_file() or not (folder / 'manifest.json').is_file():
        return None, None
    return (json.loads((folder / 'result.json').read_text(encoding='utf-8')),
            json.loads((folder / 'manifest.json').read_text(encoding='utf-8')))


def judge(evidence):
    """Judge every arm against the baseline. Returns (report lines, judgement), or raises when the baselines that
    calibrate the comparison are missing or failed."""
    loaded = {arm: load(evidence, arm) for arm in ARMS}
    for arm in BASELINES:
        result = loaded[arm][0]
        if result is None:
            raise pins.PinError(f'the {arm} arm left no evidence; the arms cannot be judged')
        if result['exit'] != 0 or result['layout_missing']:
            raise pins.PinError(f'the {arm} arm did not install the SDK (exit {result["exit"]}, missing '
                                f'{result["layout_missing"]}); the arms cannot be judged')
    base = loaded['baseline'][1]
    repeat = loaded['baseline-repeat'][1]
    variable = set().union(*differences(base, repeat, set()).values())
    judgement = {'variable': sorted(variable), 'arms': {}}
    lines = ['### Vulkan SDK installer options', '',
             '| Arm | Options | Exit | Qt font lines | Remote lines | Hosts | Cache files | Tree | Verdict |',
             '|---|---|---|---|---|---|---|---|---|']
    details = []
    for arm in ARMS:
        result, tree = loaded[arm]
        if result is None:
            judgement['arms'][arm] = {'verdict': 'no evidence'}
            lines.append(f'| {arm} | {" ".join(ARMS[arm]) or "(production)"} | | | | | | | no evidence |')
            continue
        diff = differences(base, tree, variable)
        same = not any(diff.values())
        clean = result['exit'] == 0 and not result['layout_missing']
        if not clean:
            verdict = 'install failed'
        elif same and not result['qt_font'] and not result['remote']:
            verdict = 'removes both, same tree'
        elif not same:
            verdict = 'tree differs'
        else:
            verdict = 'findings remain'
        judgement['arms'][arm] = {'verdict': verdict, **diff}
        counts = tuple(len(diff[key]) for key in ('added', 'removed', 'changed'))
        tree_cell = 'same' if same else '+%d -%d ~%d' % counts
        lines.append(f'| {arm} | {" ".join(ARMS[arm]) or "(production)"} | {result["exit"]} | '
                     f'{len(result["qt_font"])} | {len(result["remote"])} | {", ".join(result["hosts"]) or "none"} | '
                     f'{len(result["cache_after"])} | {tree_cell} | {verdict} |')
        for key in ('added', 'removed', 'changed'):
            if diff[key]:
                shown = diff[key][:LISTED]
                more = len(diff[key]) - len(shown)
                details.append(f'- {arm}, {key}: ' + ', '.join(f'`{path}`' for path in shown) +
                               (f' and {more} more' if more else ''))
    lines += ['', f'Paths the two baseline runs disagree on (left out of every comparison): '
              + (', '.join(f'`{path}`' for path in sorted(variable)) or 'none'), '']
    lines += details
    return lines, judgement


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)
    run_parser = commands.add_parser('run', help='install the SDK under one arm and record its evidence')
    run_parser.add_argument('--arm', required=True, choices=sorted(ARMS))
    run_parser.add_argument('--download-dir', type=Path, required=True)
    run_parser.add_argument('--evidence', type=Path, required=True)
    run_parser.add_argument('--archive', type=Path, help='an already downloaded official installer (checksum required)')
    judge_parser = commands.add_parser('judge', help='compare every arm with the baseline')
    judge_parser.add_argument('--evidence', type=Path, required=True)
    judge_parser.add_argument('--summary', type=Path, help='file the Markdown report is appended to')
    args = parser.parse_args(argv)
    try:
        if args.command == 'run':
            run_arm(args.arm, args.download_dir, args.evidence, archive=args.archive)
            return 0
        lines, judgement = judge(args.evidence)
    except pins.PinError as error:
        print(f'vulkan-sdk-installer-check: {error}', file=sys.stderr)
        return 1
    (args.evidence / 'judgement.json').write_text(json.dumps(judgement, indent=1) + '\n', encoding='utf-8')
    report = '\n'.join(lines) + '\n'
    print(report)
    if args.summary:
        with args.summary.open('a', encoding='utf-8') as summary:
            summary.write(report)
    return 0


if __name__ == '__main__':
    sys.exit(main())
