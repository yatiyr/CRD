#!/usr/bin/env python3
"""Every allocation result in shipped code is checked before it is used.

A sub-budget allocator (linear, stack, pool, growable linear) refuses a request by returning nullptr, so code that
takes memory from a caller's IAllocator and writes through the result unchecked follows null in every build without
asserts (DIAG.3a, docs/design/runtime-diagnostics.md#diag-3a). This guard reads the C++ sources under engine/, tools/,
runtime/ and sandbox/ and reports every raw allocation whose result is not checked:

  - a call `x.allocate(...)`, `x->allocate(...)`, `try_allocate(...)` or `reallocate(...)` with arguments, and a
    `construct<T>(...)`, `construct_array<T>(...)` or `allocate_array<T>(...)` (the forms that return nullptr; the
    `*_or_fatal` forms end in the caller's named fatal and need no check);
  - checked means: the call is the operand of `return` (an allocator or adapter handing its result up, whose caller
    checks), it sits inside an `if`/`while` condition (a bool or a pointer tested where it is made), or the variable it
    is assigned to is compared with nullptr, negated or tested with `.valid()` within the next six lines;
  - a heap allocator is fatal on exhaustion by contract, so a call on `default_allocator()` is checked, and so are the
    sites named in ALLOWED below, each with the reason its allocator cannot refuse.

  python scripts/check-allocation-results.py [<repository root>]

Comment lines are skipped. A tree with no allocation call at all is a failure: nothing would have been checked.
"""
import re
import sys
from pathlib import Path

ROOTS = ('engine', 'tools', 'runtime', 'sandbox')
SUFFIXES = {'.cpp', '.hpp', '.h', '.inl', '.cc'}
WINDOW = 6

# Allocator implementations: their allocate/reallocate bodies ARE the allocation (each returns or checks its parent's
# result in its own way, covered by the memory tests and the alloc-fatal specimen).
ALLOCATOR_DIRS = ('engine/foundation/memory/src/allocators/', 'engine/foundation/memory/include/crd/memory/allocators/')
ALLOCATOR_FILES = {'engine/foundation/memory/include/crd/memory/construct.hpp'}

# path -> (exact count of unchecked calls allowed, reason). A count, not a blanket file pass: a new unchecked call in
# the same file fails the guard.
ALLOWED = {
    'engine/foundation/platform/include/crd/platform/input.hpp': (1, 'alloc is default_allocator(), fatal when out'),
    'engine/numerics/hesap-tensor/include/crd/hesap/tensor/sparse.hpp': (1, 'ScratchBlock keeps the result in m_ptr; '
                                                                          'every caller tests it, AllocFailed if null'),
    'tools/ceridc/src/main.cpp': (1, 'g_alloc is a GrowableTlsfAllocator, a heap allocator fatal on exhaustion'),
    'runtime/examples/smoke_resources.cpp': (1, 'm_alloc is a TlsfAllocator, a heap allocator fatal on exhaustion'),
    'runtime/examples/smoke_resources_async.cpp': (1, 'm_alloc is a TlsfAllocator, fatal on exhaustion'),
    'runtime/examples/smoke_resources_reload.cpp': (1, 'm_alloc is a TlsfAllocator, fatal on exhaustion'),
    'runtime/examples/smoke_resources_stream.cpp': (1, 'm_alloc is a TlsfAllocator, fatal on exhaustion'),
    'runtime/examples/bench_fft_f32.cpp': (3, 'alloc is a GrowableTlsfAllocator, fatal on exhaustion'),
    'runtime/examples/smoke_memory.cpp': (4, 'the heap default allocator, a 64 KiB scratch arena asked for 1 KiB and a '
                                             '64-slot pool asked for 2 slots'),
    'runtime/examples/smoke_virtual_memory_allocator.cpp': (3, 'a VirtualMemoryAllocator, fatal on exhaustion'),
}

CALL = re.compile(r'(?:->|\.)\s*(?:try_allocate|allocate|reallocate)\s*\(\s*(?!\))'
                  r'|\b(?:crd::memory::|memory::|mem::)?(?:construct|construct_array|allocate_array)\s*<')
OR_FATAL = re.compile(r'_or_fatal\s*<|_or_fatal\s*\(')
ASSIGNED = re.compile(r'(?:^|[\s(,])(?P<var>[A-Za-z_][\w]*(?:(?:->|\.)[A-Za-z_]\w*|\[[^\]]*\])*)\s*=[^=]')
DECLARED = re.compile(r'\b(?P<var>[A-Za-z_]\w*)\s*(?:\{|=)[^=]')
CONDITION = re.compile(r'\b(?:if|while)\s*\(')


def rel(path, root):
    return path.relative_to(root).as_posix()


def is_comment(line):
    stripped = line.strip()
    return stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*')


def code_part(line):
    """The line without a trailing // comment (string literals holding // are rare enough here to accept)."""
    index = line.find('//')
    return line if index < 0 else line[:index]


def assigned_variable(statement):
    """The variable an allocation's result is stored in, or None."""
    before = statement[:CALL.search(statement).start()]
    match = None
    for candidate in ASSIGNED.finditer(before):
        match = candidate
    if match is None:
        for candidate in DECLARED.finditer(before):
            match = candidate
    return None if match is None else match.group('var')


def enclosing_block(lines):
    """The lines up to the end of the block the allocation is in: a check in a later function (a destructor's
    `if (m_ptr != nullptr)`) is no check of this result."""
    depth = 0
    kept = []
    for line in lines:
        code = code_part(line)
        depth += code.count('{') - code.count('}')
        if depth < 0:
            break
        kept.append(line)
    return kept


def checked_later(var, lines):
    name = re.escape(var)
    tests = (rf'{name}\s*[!=]=\s*nullptr', rf'nullptr\s*[!=]=\s*{name}', rf'!\s*{name}\b(?!\s*[.\-\[(])',
             rf'\b{name}\s*\.\s*valid\s*\(', rf'if\s*\(\s*{name}\s*\)')
    return any(re.search(test, code_part(line)) for line in lines for test in tests)


def statement_at(lines, index):
    """The allocation's statement: its line plus continuation lines up to the first `;` or `{`."""
    text = code_part(lines[index])
    start = index
    # A call that continues a previous line (an assignment split after `=`): join the line before.
    while start > 0 and re.search(r'(=|\(|,|return)\s*$', code_part(lines[start - 1]).rstrip()):
        start -= 1
        text = code_part(lines[start]) + ' ' + text
    end = index
    while ';' not in code_part(lines[end]) and '{' not in code_part(lines[end]) and end + 1 < len(lines) and \
            end - index < 4:
        end += 1
        text += ' ' + code_part(lines[end])
    return text, end


def unchecked_calls(text):
    """The 1-based line numbers of the unchecked allocations in a source text."""
    lines = text.splitlines()
    found = []
    for index, line in enumerate(lines):
        if is_comment(line):
            continue
        code = code_part(line)
        call = CALL.search(code)
        if call is None or OR_FATAL.search(code[call.start():call.start() + 40]):
            continue
        statement, end = statement_at(lines, index)
        head = statement[:statement.find(code[call.start():call.end()].strip())] if code.strip() else statement
        if re.search(r'\breturn\b', head) or 'default_allocator()' in statement:
            continue
        if CONDITION.search(head):
            continue
        var = assigned_variable(statement) if CALL.search(statement) else None
        if var is not None and checked_later(var, [lines[end]] + enclosing_block(lines[end + 1:end + 1 + WINDOW])):
            continue
        found.append(index + 1)
    return found


def check(root):
    """Returns (number of allocation calls seen, problems)."""
    root = Path(root)
    seen = 0
    problems = []
    for top in ROOTS:
        for path in sorted((root / top).rglob('*')):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            name = rel(path, root)
            if name in ALLOCATOR_FILES or any(name.startswith(prefix) for prefix in ALLOCATOR_DIRS):
                continue
            text = path.read_text(encoding='utf-8', errors='replace')
            seen += sum(1 for line in text.splitlines() if not is_comment(line) and CALL.search(code_part(line)))
            found = unchecked_calls(text)
            allowed, reason = ALLOWED.get(name, (0, ''))
            if len(found) == allowed:
                continue
            if len(found) < allowed:
                problems.append(f'{name}: {len(found)} unchecked allocations where {allowed} are allowed ({reason}); '
                                f'lower the count in ALLOWED')
                continue
            lines = ', '.join(str(number) for number in found)
            problems.append(f'{name}: allocation result used without a check at line(s) {lines} (DIAG.3a: compare '
                            f'it with nullptr, report a status, or use an *_or_fatal helper)')
    return seen, problems


def main(argv):
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parents[1]
    seen, problems = check(root)
    if seen == 0:
        print(f'check-allocation-results: no allocation call under {root}; nothing was checked')
        return 1
    for problem in problems:
        print(f'check-allocation-results: {problem}')
    if problems:
        return 1
    print(f'check-allocation-results: {seen} allocation calls, every result checked or passed up')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
