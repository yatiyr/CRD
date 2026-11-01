#!/usr/bin/env python3
"""Run the pinned Windows Vulkan SDK installer under one arm of REPO.3's option experiment, or judge the arms.

Contract: docs/design/test-instruments.md (the Vulkan SDK installer leg). The cold Windows lane's run of the pinned
installer (install-vulkan-sdk.py) prints Qt's `QFont::setPixelSize: Pixel size <= 0 (-1)` warning and fetches update
metadata from LunarG's remote repository (docs/sessions/2026-11-01-repo-3d-scoped-checks.md). The Qt Installer
Framework has no offline switch, so each arm adds candidate options to the production arguments on a fresh hosted
machine and records the installer's exit, both findings, the hosts its output names, its metadata cache and a
manifest of the installed tree. `judge` compares every arm with the baseline, discounting the paths on which two
baseline runs disagree; an arm whose options belong in install-vulkan-sdk.py exits 0, prints neither finding and
installs the baseline's tree. `trace` runs the production install under the Windows debugger with a breakpoint on
the installer's QFont::setPixelSize and names the caller of every call that passes a size <= 0 (the warning's
emitter) from the static census of its callers (docs/sessions/2026-11-01-repo-3-qfont-emitter.md).

    python scripts/vulkan-sdk-installer-check.py run --arm <name> --download-dir <dir> --evidence <dir>
    python3 scripts/vulkan-sdk-installer-check.py judge --evidence <dir> [--summary <file>]
    python scripts/vulkan-sdk-installer-check.py trace --download-dir <dir> --evidence <dir>
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import struct
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
# Updates.xml lists no package (install-vulkan-sdk.py's, which the experiment's verdict made production).
EMPTY, UPDATES = 'empty-repository', 'updates-repository'
BASELINES = ('baseline', 'baseline-repeat')
# Arm name -> the options added to sdk.INSTALL_ARGUMENTS, the arguments before the verdict (EMPTY and UPDATES stand
# for those repositories' URIs).
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
# The framework prints `Fetching latest update information...` for any repository, a local one included (run
# 41298875777); only this line marks the fetch from LunarG's remote repository.
REMOTE = re.compile(r'meta information from remote repository')
URL = re.compile(r'\b(?:https?|ftp)://[^\s"\'<>]+')
TIMEOUT_SECONDS = 1500
LISTED = 20

# The static census of the pinned installer (Qt Installer Framework 4.9.0 on a static Qt 6.7.2; only for the file of
# this digest): QFont::setPixelSize is the function at SET_PIXEL_SIZE, the only one that loads the warning's format
# string, and its 15 direct calls return to these RVAs. Three callers are named by their strings and their source.
CENSUS_SHA256 = 'bcf2d75aa9556889ab974858666e20b3655b6055a0db704ccb47279ff33b5bfe'
SET_PIXEL_SIZE = 0x806a90
CALLERS = {
    0x209c3a: "the framework's ComponentSelectionPagePrivate: the caption font's setPixelSize(16)",
    0x3a3640: "Qt SVG's text layout ('qt.svg.draw')",
    0xa623f5: "Qt's QTextMarkdownImporter::import: the mono font takes the document default font's pixelSize()",
    **{rva: 'an unnamed Qt function' for rva in (0x2e864b, 0x36fad8, 0x3a1b8a, 0x3a1eca, 0x803003, 0x91cb52,
                                                   0x91deda, 0x91def1, 0x963175, 0x9b900d, 0x9b946c, 0xa56bb1)},
}
CDB_DEFAULT = Path('Windows Kits/10/Debuggers/x64/cdb.exe')
REGISTER = re.compile(r'^edx=([0-9a-f]{8})$')
# A `k` frame: child stack pointer, return address, call site (`module+0x...` without symbols, `module!name+0x...`
# with exports).
FRAME = re.compile(r'^[0-9a-f]{8}`[0-9a-f]{8} [0-9a-f]{8}`[0-9a-f]{8} (\S+)')
IMAGE_FRAME = re.compile(r'^[^!\s]+\+0x([0-9a-f]+)$')


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
    return {EMPTY: (scratch / EMPTY).resolve().as_uri(), UPDATES: sdk.local_repository(scratch / UPDATES)}


def cached_repositories(cache):
    """Relative path -> text of every repository.txt in the metadata cache: the repository each entry came from."""
    cache = Path(cache)
    return {relative: (cache / relative).read_text(encoding='utf-8', errors='replace').strip()
            for relative in listing(cache) if relative.endswith('repository.txt')}


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
              'cache_repositories': cached_repositories(cache),
              'layout_missing': [relative for relative in sdk.LAYOUT if relative not in tree],
              'tree': {'files': len(tree), 'bytes': sum(size for size, _ in tree.values()),
                       'digest': hashlib.sha256(canonical).hexdigest()}}
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n', encoding='utf-8')
    print(output)
    print(json.dumps({key: result[key] for key in ('arm', 'arguments', 'exit', 'qt_font', 'remote', 'hosts',
                                                   'cache_after', 'cache_repositories', 'layout_missing', 'tree')},
                     indent=1))
    return result


def entry_rva(image):
    """The AddressOfEntryPoint of a PE image, so a breakpoint can be placed by RVA wherever the image loads."""
    with Path(image).open('rb') as stream:
        header = stream.read(4096)
    pe = struct.unpack_from('<I', header, 0x3c)[0]
    if header[:2] != b'MZ' or header[pe:pe + 4] != b'PE\0\0':
        raise pins.PinError(f'{image} is not a PE image')
    return struct.unpack_from('<I', header, pe + 24 + 16)[0]


def find_cdb(which=shutil.which, program_files=None):
    """The console debugger: on PATH, or the Windows SDK's Debugging Tools (installed on the hosted images)."""
    found = which('cdb')
    if found:
        return Path(found)
    root = Path(program_files or os.environ.get('ProgramFiles(x86)') or r'C:\Program Files (x86)')
    if (root / CDB_DEFAULT).is_file():
        return root / CDB_DEFAULT
    raise pins.PinError(f'cdb is neither on PATH nor at {root / CDB_DEFAULT}; install the Debugging Tools for Windows')


def traced_calls(output):
    """Each setPixelSize call the breakpoint logged: its size (edx, signed), its frames and its caller."""
    calls = []
    for line in output.splitlines():
        line = line.strip()
        register = REGISTER.match(line)
        if register:
            value = int(register.group(1), 16)
            calls.append({'pixel_size': value - (1 << 32) if value >= 1 << 31 else value, 'frames': []})
            continue
        frame = FRAME.match(line)
        if frame and calls:
            calls[-1]['frames'].append(frame.group(1))
    for call in calls:
        # Frame 0 is setPixelSize itself; frame 1's call site is the return address in its caller.
        image = IMAGE_FRAME.match(call['frames'][1]) if len(call['frames']) > 1 else None
        rva = int(image.group(1), 16) if image else None
        call['caller_rva'] = None if rva is None else '%#x' % rva
        call['caller'] = (CALLERS.get(rva, f'outside the census (RVA {rva:#x})') if rva is not None
                          else 'no caller frame in the image')
    return calls


def trace(download_dir, evidence, archive=None, root=None, run=subprocess.run, which=shutil.which,
          program_files=None):
    """Install the pinned SDK with the production arguments under cdb, logging edx and the stack at every
    QFont::setPixelSize call; writes `<evidence>/trace/` (log.txt, result.json) and returns the result."""
    pin = pins.entry('tools', 'vulkan-sdk-windows')
    if pin['sha256'] != CENSUS_SHA256:
        raise pins.PinError(f'the setPixelSize census is for the installer {CENSUS_SHA256}, the pin is now '
                            f'{pin["sha256"]}: take the census again first')
    root = Path(root or pin['install_root'])
    cdb = find_cdb(which, program_files)
    out = Path(evidence) / 'trace'
    out.mkdir(parents=True, exist_ok=True)
    installer = pins.fetch(pin, download_dir, archive, label='Vulkan SDK installer')
    commands = out / 'commands.txt'
    commands.write_text('bp @$exentry-%#x+%#x "r edx; k 12; gc"\ng\nq\n' % (entry_rva(installer), SET_PIXEL_SIZE),
                        encoding='ascii')
    command = [str(cdb), '-G', '-cf', str(commands), str(installer), *sdk.arguments(download_dir)]
    try:
        completed = run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        timeout=TIMEOUT_SECONDS, check=False)
        exit_code, raw = completed.returncode, completed.stdout or b''
    except subprocess.TimeoutExpired as expired:
        exit_code, raw = 'timeout', expired.output or b''
    output = raw.decode('utf-8', errors='replace')
    (out / 'log.txt').write_text(output, encoding='utf-8')
    calls = traced_calls(output)
    # cdb's exit is the debugger's; the install's own outcome is its layout.
    result = {'version': pin['version'], 'command': command[1:], 'exit': exit_code,
              'layout_missing': [relative for relative in sdk.LAYOUT if not (root / relative).is_file()],
              'qt_font': findings(output)['qt_font'], 'calls': calls,
              'emitters': sorted({call['caller'] for call in calls if call['pixel_size'] <= 0})}
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n', encoding='utf-8')
    print(output)
    print(json.dumps({key: result[key] for key in ('exit', 'layout_missing', 'qt_font', 'emitters')}, indent=1))
    return result


def differences(base, other, variable):
    """Paths added, removed and changed from `base` to `other`, leaving out the `variable` ones."""
    return {'added': sorted(set(other) - set(base) - variable),
            'removed': sorted(set(base) - set(other) - variable),
            'changed': sorted(path for path in (set(base) & set(other)) - variable if base[path] != other[path])}


def load(evidence, arm):
    """An arm's result, its findings read again from its log (so recorded evidence is judged by the current
    patterns), and its manifest."""
    folder = Path(evidence) / arm
    if not all((folder / name).is_file() for name in ('result.json', 'manifest.json', 'log.txt')):
        return None, None
    result = json.loads((folder / 'result.json').read_text(encoding='utf-8'))
    return ({**result, **findings((folder / 'log.txt').read_text(encoding='utf-8'))},
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
    trace_parser = commands.add_parser('trace', help='install under cdb and name the setPixelSize warning\'s caller')
    trace_parser.add_argument('--download-dir', type=Path, required=True)
    trace_parser.add_argument('--evidence', type=Path, required=True)
    trace_parser.add_argument('--archive', type=Path,
                              help='an already downloaded official installer (checksum required)')
    args = parser.parse_args(argv)
    try:
        if args.command == 'run':
            run_arm(args.arm, args.download_dir, args.evidence, archive=args.archive)
            return 0
        if args.command == 'trace':
            result = trace(args.download_dir, args.evidence, archive=args.archive)
            # The trace answers a question only when the install ran to its end and the breakpoint saw the warning.
            return 0 if result['exit'] == 0 and not result['layout_missing'] and result['emitters'] else 1
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
    # The installer's output is echoed after its evidence is written; a console code page must not fail the arm.
    sys.stdout.reconfigure(errors='replace')
    sys.exit(main())
