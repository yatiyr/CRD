#!/usr/bin/env python3
"""Strict LLVM-20 analysis of specific C++ files with the configured build's real compile flags, on any host.

Exit code: the number of files not gated clean (issues + ungated + missing); 99 when clang-tidy 20 or the compile
database is unavailable, so an unavailable gate can never read as a pass.
"""
from pathlib import Path
import argparse
import json
import os
import sys

from cerid_dev.selection import SelectionError
from cerid_dev.tidy import DEFAULT_BUILD, UNAVAILABLE_EXIT, analyse, report_lines


def parser():
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    result.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    result.add_argument('--build', type=Path, default=Path(DEFAULT_BUILD),
                        help=f'Configured build with compile_commands.json (default {DEFAULT_BUILD})')
    result.add_argument('--clang-tidy', help='Explicit clang-tidy executable; must report LLVM 20')
    result.add_argument('--plan', type=Path, help='dev.py plan JSON whose owners map headers to their targets')
    result.add_argument('--summary', type=Path, help='Write the JSON summary here')
    result.add_argument('--export-fixes-directory', type=Path)
    result.add_argument('--scratch', type=Path, help='Directory for the mirrored compile database (default: temporary)')
    result.add_argument('--timeout', type=float, default=600, help='Per-file budget in seconds')
    result.add_argument('--json', action='store_true')
    result.add_argument('files', nargs='*', help='C++ sources and headers, relative to the root or absolute')
    return result


def main(arguments=None):
    args = parser().parse_args(arguments)
    root = args.root.resolve(strict=True)
    build = args.build if args.build.is_absolute() else root / args.build
    owners = None
    if args.plan:
        owners = json.loads(args.plan.read_text(encoding='utf-8')).get('owners') or {}
    try:
        summary = analyse(root, build, args.files, dict(os.environ), owners=owners, tool=args.clang_tidy,
                          scratch=args.scratch, export_fixes=args.export_fixes_directory, timeout=args.timeout)
    except (SelectionError, OSError, ValueError) as error:
        summary = {'version': 1, 'kind': 'tidy', 'root': str(root), 'build': str(build), 'status': 'instrument_failure',
                   'exit_code': UNAVAILABLE_EXIT, 'error': str(error), 'files': [],
                   'counts': {'clean': 0, 'issues': 0, 'ungated': 0, 'missing': 0},
                   'qualification': 'no strict analysis ran; changed C++ remains ungated'}
    if args.summary:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(summary, indent=2), encoding='utf-8')
    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        if summary.get('error'):
            print('ERROR: ' + summary['error'], file=sys.stderr)
        print('\n'.join(report_lines(summary)))
    return summary['exit_code']


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    raise SystemExit(main())
