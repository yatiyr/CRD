#!/usr/bin/env python3
"""One optimization level per MSVC-style compile command.

cl.exe keeps the last of /O1, /O2, /Ox and /Od on a command line and reports the earlier one as command-line warning
D9025 ("overriding '/O2' with '/Od'"), which /WX does not make an error; clang-cl keeps the last one silently. Either
way the command compiles at a level its configuration does not name, and the build stays green. This guard reads a
configuration's compile commands (CMAKE_EXPORT_COMPILE_COMMANDS) and reports every cl.exe or clang-cl command that
carries more than one distinct level. The crd-jobs /Od pin compiles in engine/foundation/jobs/unoptimized/ for this
reason: a per-source /Od after the configuration's /O2 was two levels on one command.

  python scripts/check-compile-commands.py <build dir>/compile_commands.json

A file that holds no MSVC-style command is a failure as well: nothing would have been checked.
"""
import json
import re
import shlex
import sys
from pathlib import Path

MSVC_STYLE = {'cl', 'clang-cl'}
LEVEL = re.compile(r'^[/-]O([12xd])$')
# A compiler launcher may precede the compiler; the compiler is one of the first few words.
COMPILER_WORDS = 3


def tokens(entry):
    if 'arguments' in entry:
        return list(entry['arguments'])
    return shlex.split(entry.get('command', ''), posix=False)


def compiler_name(word):
    name = re.split(r'[\\/]', word.strip('"'))[-1].lower()
    return name[:-4] if name.endswith('.exe') else name


def levels(words):
    """The optimization levels of an MSVC-style command in order, or None for another compiler."""
    if not any(compiler_name(word) in MSVC_STYLE for word in words[:COMPILER_WORDS]):
        return None
    return ['/O' + match.group(1) for match in (LEVEL.match(word.strip('"')) for word in words) if match]


def check(entries):
    """Returns (number of MSVC-style commands, problems)."""
    checked = 0
    problems = []
    for entry in entries:
        found = levels(tokens(entry))
        if found is None:
            continue
        checked += 1
        if len(set(found)) > 1:
            problems.append(f"{entry.get('file', '?')}: optimization levels {' then '.join(found)} on one command "
                            f"(cl.exe D9025; the last one wins)")
    return checked, problems


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = Path(argv[1])
    try:
        entries = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        print(f'ERROR: cannot read {path}: {error}', file=sys.stderr)
        return 1
    checked, problems = check(entries)
    if checked == 0:
        problems.append(f'{path} holds no cl.exe or clang-cl compile command; nothing was checked')
    for problem in problems:
        print(f'ERROR: {problem}')
    if problems:
        return 1
    print(f'Checked {checked} MSVC-style compile commands in {path}: one optimization level each.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
