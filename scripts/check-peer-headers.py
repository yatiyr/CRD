#!/usr/bin/env python3
"""Only the board peer libraries include a peer header library.

A header library a board measures against (Eigen, Boost, pocketfft) is compiled in a peer library of its own under
runtime/examples/peers/ at the measuring host's best ISA (ADR-0134 §8, crd_add_bench_peer in runtime/CMakeLists.txt).
Every other unit builds at the baseline, so a bench that included the library itself would time the peer at the
baseline ISA, and an inline body of the peer at another ISA could reach a baseline unit. This guard reads every
C-family source under the first-party roots and reports each include of such a library outside the peers directory.

  python scripts/check-peer-headers.py [<repository root>]

Comments and string literals are not code. Compiled libraries whose declarations a bench may include (cblas.h,
fftw3.h, mkl*.h, SUNDIALS, liquid-dsp, FINUFFT, ggml) are not header libraries, and the GPU boards' device-side
header libraries (cub/, vkFFT.h) compile for the device, outside the CPU-ISA rule; neither is checked. HPTT is
compiled from source too, but its headers define the inline members of its Transpose class templates, so only its peer
includes them. A tree whose peers directory includes no header library is a failure: the guard would be judging
renamed paths.
"""
import argparse
import importlib.util
import re
import sys
from pathlib import Path

PEERS = 'runtime/examples/peers/'
# The first-party source roots (scripts/source-routes.json), in path order so problems list by path.
ROOTS = ('bench', 'engine', 'runtime', 'sandbox', 'scripts', 'tests', 'tools')
SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.cu', '.h', '.hpp', '.inl', '.mm'}
# The header libraries the boards build from source, by the header names they are included with.
LIBRARIES = (
    ('Eigen', re.compile(r'(unsupported/)?Eigen/')),
    ('Boost', re.compile(r'boost/')),
    ('pocketfft', re.compile(r'pocketfft(_hdronly)?\.h(pp)?$')),
    ('HPTT', re.compile(r'hptt(_types)?\.h$')),
)
DIRECTIVE = re.compile(r'^\s*#\s*include\b')
HEADER = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

_spec = importlib.util.spec_from_file_location('check_environment_reads',
                                               Path(__file__).resolve().parent / 'check-environment-reads.py')
_reads = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_reads)
code_only = _reads.code_only


def library_of(header):
    """The peer header library `header` belongs to, or None."""
    for name, pattern in LIBRARIES:
        if pattern.match(header):
            return name
    return None


def peer_includes(text):
    """(1-based line, library, header) of each code include of a peer header library in C-family source `text`."""
    raw = text.splitlines()
    found = []
    for number, line in enumerate(code_only(text).splitlines(), 1):
        if not DIRECTIVE.match(line):
            continue
        header = HEADER.match(raw[number - 1])
        library = library_of(header.group(1)) if header else None
        if library:
            found.append((number, library, header.group(1)))
    return found


def check(root):
    """Returns (peer header-library includes in the peers directory, problems)."""
    root = Path(root)
    in_peers = 0
    problems = []
    for top in ROOTS:
        for path in sorted((root / top).rglob('*')):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            name = path.relative_to(root).as_posix()
            found = peer_includes(path.read_text(encoding='utf-8', errors='replace'))
            if name.startswith(PEERS):
                in_peers += len(found)
                continue
            for number, library, header in found:
                problems.append(f'{name}:{number}: includes {library} ({header}); only a peer library under '
                                f'{PEERS} includes it, at the host\'s ISA (crd_add_bench_peer), and the bench calls '
                                f'the peer\'s exported operations')
    return in_peers, problems


def main(argv):
    parser = argparse.ArgumentParser(prog='check-peer-headers.py', description=__doc__.splitlines()[0])
    parser.add_argument('root', nargs='?', default=str(Path(__file__).resolve().parents[1]),
                        help='the repository root (default: this checkout)')
    root = Path(parser.parse_args(argv[1:]).root)
    in_peers, problems = check(root)
    if in_peers == 0:
        print(f'check-peer-headers: {PEERS} includes no peer header library under {root}; nothing was checked')
        return 1
    for problem in problems:
        print(f'check-peer-headers: {problem}')
    if problems:
        return 1
    print(f'check-peer-headers: only {PEERS} includes a peer header library ({in_peers} includes)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
