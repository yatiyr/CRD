#!/usr/bin/env python3
"""Prove that a configure acquired its pinned packages from a cold cache (REPO.DEV.6).

Contract: docs/design/pinned-inputs.md. A complete-tier lane named in the workflow's CRD_COLD_PRESETS skips the CPM
archive cache, so its configure downloads every package it needs. CMake verifies each archive against the pinned
SHA-256 (`URL_HASH` in crd_add_pinned_package) before extracting it, and a mismatch fails the configure.

`--before CACHE` fails unless the CPM source cache is absent or empty, so a restored cache can never read as a cold
acquisition. `--after CACHE --build DIR` reads the packages that configure added (`CPM_PACKAGES` in CMakeCache.txt). It
fails unless every one is pinned in cmake/pins.json and now present in the cache, and it names the pins this
configuration did not select. The verdict is appended to $GITHUB_STEP_SUMMARY when that is set.
"""
from pathlib import Path
import argparse
import json
import os
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PINS = ROOT / 'cmake/pins.json'


def cache_is_empty(cache):
    """True when the CPM source cache does not exist or holds nothing."""
    return not cache.exists() or not any(cache.iterdir())


def added_packages(build):
    """The packages the configure added, in order, from CPM_PACKAGES in CMakeCache.txt; None when absent."""
    cache_file = build / 'CMakeCache.txt'
    if not cache_file.is_file():
        return None
    for line in cache_file.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('CPM_PACKAGES:'):
            value = line.split('=', 1)[1] if '=' in line else ''
            return [name for name in value.split(';') if name]
    return None


def check_after(cache, build, pins):
    """Return (ok, lines) for the post-configure verdict."""
    pinned = set(json.loads(Path(pins).read_text(encoding='utf-8'))['packages'])
    added = added_packages(build)
    if not added:
        return False, [f'no CPM_PACKAGES recorded in {build / "CMakeCache.txt"}; the configure acquired nothing']
    lines = []
    ok = True
    for name in added:
        if name not in pinned:
            ok = False
            lines.append(f'{name}: added by the configure but not pinned in cmake/pins.json')
            continue
        entry = cache / name.lower()
        if not entry.is_dir() or not any(entry.iterdir()):
            ok = False
            lines.append(f'{name}: not present in the CPM source cache {entry}')
        else:
            lines.append(f'{name}: acquired cold and verified against its pinned SHA-256')
    unselected = sorted(pinned - set(added))
    if unselected:
        lines.append('not selected by this configuration: ' + ', '.join(unselected))
    return ok, lines


def report(title, ok, lines):
    verdict = 'PASS' if ok else 'FAIL'
    text = [f'{title}: {verdict}'] + [f'  {line}' for line in lines]
    print('\n'.join(text))
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a', encoding='utf-8') as handle:
            handle.write(f'### {title}: {verdict}\n\n' + ''.join(f'- {line}\n' for line in lines) + '\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--before', type=Path, metavar='CACHE', help='the CPM source cache must be absent or empty')
    mode.add_argument('--after', type=Path, metavar='CACHE', help='the CPM source cache after the configure')
    parser.add_argument('--build', type=Path, help='the configured build directory (with --after)')
    parser.add_argument('--pins', type=Path, default=DEFAULT_PINS)
    args = parser.parse_args(argv)
    if args.before is not None:
        ok = cache_is_empty(args.before)
        report('Cold package cache', ok, [f'{args.before} is ' + ('empty' if ok else 'NOT empty: a cache was restored')])
        return 0 if ok else 1
    if args.build is None:
        parser.error('--after needs --build')
    ok, lines = check_after(args.after, args.build, args.pins)
    report('Cold acquisition of the pinned packages', ok, lines)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
