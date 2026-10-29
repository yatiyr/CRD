#!/usr/bin/env python3
"""Every environment variable is read through crd/core/environment.hpp.

The reader (engine/foundation/core/src/environment.cpp) is the one place that calls the C runtime or the OS: it copies
into caller storage with no heap and no lock, reads an empty value as unset on every platform (Windows cannot tell
the two apart) and reports a value too long for its buffer instead of cutting it. A direct `std::getenv`,
`_dupenv_s` or `GetEnvironmentVariable` elsewhere brings back what it replaced: MSVC's C4996 and the
`_CRT_SECURE_NO_WARNINGS` or pragma that hides it, platform branches, heap copies to free and pointers into an
environment block another thread may change. This guard reads every C-family source under the roots of
scripts/source-routes.json and reports each such call outside the reader, by line.

  python scripts/check-environment-reads.py [<repository root>]

Comments and string literals are not code. A tree whose reader holds no read is a failure: nothing would be checked.
"""
import argparse
import json
import re
import sys
from pathlib import Path

READER = 'engine/foundation/core/src/environment.cpp'
SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.cu', '.h', '.hpp', '.inl', '.mm'}

READ = re.compile(r'\b(?:std::)?(?:getenv|getenv_s|_wgetenv|_wgetenv_s|_dupenv_s|_wdupenv_s|secure_getenv)\s*\('
                  r'|\bGetEnvironmentVariable[AW]?\s*\(|\bGetEnvironmentStrings[AW]?\s*\('
                  r'|(?<![\w.:>])(?:_wenviron|_environ|__environ|environ)\b(?!\s*\()')


def code_only(text):
    """`text` with every comment and string or character literal blanked, newlines kept so lines still count."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            end = text.find('\n', i)
            i = n if end < 0 else end
        elif text.startswith('/*', i):
            end = text.find('*/', i + 2)
            end = n if end < 0 else end + 2
            out.append('\n' * text.count('\n', i, end))
            i = end
        elif c == 'R' and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            open_paren = text.find('(', i + 2)
            delimiter = text[i + 2:open_paren]
            end = text.find(')' + delimiter + '"', open_paren)
            end = n if end < 0 else end + len(delimiter) + 2
            out.append('""' + '\n' * text.count('\n', i, end))
            i = end
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def raw_reads(text):
    """The 1-based line numbers of direct environment reads in C-family source `text`."""
    return [number for number, line in enumerate(code_only(text).splitlines(), 1) if READ.search(line)]


def check(root):
    """Returns (reads in the reader, problems)."""
    root = Path(root)
    routes = json.loads((root / 'scripts/source-routes.json').read_text(encoding='utf-8'))
    in_reader = 0
    problems = []
    for top in routes['roots']:
        for path in sorted((root / top).rglob('*')):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            name = path.relative_to(root).as_posix()
            found = raw_reads(path.read_text(encoding='utf-8', errors='replace'))
            if name == READER:
                in_reader = len(found)
            elif found:
                lines = ', '.join(str(number) for number in found)
                problems.append(f'{name}: reads the environment directly at line(s) {lines}; use '
                                f'crd::read_environment, read_environment_text, read_environment_integer, '
                                f'environment_set or EnvironmentValue (crd/core/environment.hpp)')
    return in_reader, problems


def main(argv):
    parser = argparse.ArgumentParser(prog='check-environment-reads.py', description=__doc__.splitlines()[0])
    parser.add_argument('root', nargs='?', default=str(Path(__file__).resolve().parents[1]),
                        help='the repository root (default: this checkout)')
    root = Path(parser.parse_args(argv[1:]).root)
    in_reader, problems = check(root)
    if in_reader == 0:
        print(f'check-environment-reads: {READER} reads nothing under {root}; nothing was checked')
        return 1
    for problem in problems:
        print(f'check-environment-reads: {problem}')
    if problems:
        return 1
    print(f'check-environment-reads: every environment read goes through {READER}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
