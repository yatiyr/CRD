#!/usr/bin/env python3
"""Engine libraries never reset the crd::jobs frame arenas.

`crd::jobs::frame_reset()` wipes every thread's frame arena (engine/foundation/jobs/include/crd/jobs/jobs.hpp). Only
a frame owner may call it, at a boundary where no job is in flight and no caller still holds frame memory: a frame
loop, a request loop, a test or a bench iteration. A library cannot know either, so a reset there frees memory its
caller or a waiting job still uses. The jobs module's parallel ranges reclaim their own job arrays, so no library
needs one. This guard reads every C-family source under engine/ and reports each call outside the jobs module.

  python scripts/check-frame-resets.py [<repository root>]

Comments and string literals are not code. A tree whose jobs module defines no frame_reset is a failure: the guard
would be judging a renamed API.
"""
import argparse
import importlib.util
import re
import sys
from pathlib import Path

OWNER = 'engine/foundation/jobs/'
SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.cu', '.h', '.hpp', '.inl', '.mm'}
RESET = re.compile(r'\bframe_reset\s*\(')

_spec = importlib.util.spec_from_file_location('check_environment_reads',
                                               Path(__file__).resolve().parent / 'check-environment-reads.py')
_reads = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_reads)
code_only = _reads.code_only


def resets(text):
    """The 1-based line numbers of frame_reset calls (or declarations) in C-family source `text`."""
    return [number for number, line in enumerate(code_only(text).splitlines(), 1) if RESET.search(line)]


def check(root):
    """Returns (frame_reset sites in the jobs module, problems)."""
    root = Path(root)
    in_owner = 0
    problems = []
    for path in sorted((root / 'engine').rglob('*')):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        name = path.relative_to(root).as_posix()
        found = resets(path.read_text(encoding='utf-8', errors='replace'))
        if name.startswith(OWNER):
            in_owner += len(found)
        elif found:
            lines = ', '.join(str(number) for number in found)
            problems.append(f'{name}: calls crd::jobs::frame_reset at line(s) {lines}; only a frame owner (a frame or '
                            f'request loop, a test, a bench) resets the arenas, and parallel ranges reclaim their own '
                            f'job arrays')
    return in_owner, problems


def main(argv):
    parser = argparse.ArgumentParser(prog='check-frame-resets.py', description=__doc__.splitlines()[0])
    parser.add_argument('root', nargs='?', default=str(Path(__file__).resolve().parents[1]),
                        help='the repository root (default: this checkout)')
    root = Path(parser.parse_args(argv[1:]).root)
    in_owner, problems = check(root)
    if in_owner == 0:
        print(f'check-frame-resets: {OWNER} holds no frame_reset under {root}; nothing was checked')
        return 1
    for problem in problems:
        print(f'check-frame-resets: {problem}')
    if problems:
        return 1
    print('check-frame-resets: no engine library resets the frame arenas')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
