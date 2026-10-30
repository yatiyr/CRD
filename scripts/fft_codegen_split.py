"""Move the FFT generators' straight-line codelets out of their header into bounded codelet units.

MSVC's optimizer is superlinear in a function's size and in a unit's: one N=256 codelet took 207 to 588 s at /O2
/arch:AVX2 and an N=128 one 14.7 GB at the SSE2 level, where every vector value is two registers; the whole engine
unit took 799 s and 4.9 GB at SSE2 even with split bodies (2026-10-28 SIMD-DISPATCH). This pass does two things:

- It cuts each codelet's tile loop body, in its scheduled order, into parts of at most `budget` lines. Each part is
  a non-inlined internal function of the codelet's parameters, the tile index and a slot array; the values that
  cross a cut go through the slots, which reuse their storage once read for the last time. No operation changes or
  moves past another, so every level keeps its bits. Cuts sit where the fewest values cross. The slots cost a
  register-wide lane up to a fifth of a deep-split or four-step transform (f64 128K to 8M at avx2, 2026-10-30), so
  each unit also holds every codelet whole, one part, under CRD_FFT_GEN_WHOLE_BODY: codelet_lanes.hpp defines it
  where a lane is one register (avx2 and avx512, optimized), and MSVC builds such a unit in about 140 s and 2.2 GB.
- It leaves the header with the codelets' declarations and spreads their definitions over `units` codelet units by
  line count (largest first, each to the unit with the fewest lines so far), so no unit outgrows a compiler and the
  engine unit that calls them parses declarations only. One call per codelet covers a whole batch.

The grammar is the canonical one fft_codegen_style.py leaves: a codelet is `[CRD_FFT_*_INLINE ]void name(...)
noexcept` at column 0 inside the header's one namespace; its body is `using V = crd::hesap::fft::lanes::Lane<T>;`,
`namespace simd = crd::hesap::fft::lanes;`, invariant locals, one tile loop
`for (crd::usize t = 0; t + kTile <= X; t += kTile)` and an optional scalar tail; values are `n<id>r`/`n<id>i` and
the loop body is lines at one depth plus `{ ... }` blocks.

A codelet computes on codelet_lanes.hpp's lanes: its parts, at least one, are templates of the lane, run on `Lane<T>`
over the whole tiles it holds (one tile above the portable level, two at avx512, a column on scalars) and on
`TileLane<T>` over an odd last tile, so every column sees the same operations on every level.

  python scripts/fft_codegen_split.py [--lanes] <header> <budget> <units> <unit-stem>

rewrites a generator-absent header in place and writes `<unit-stem>_<k>.cpp`; --lanes first retypes a header's
Vec4d/Vec8f bodies onto the lanes. gen_fft_batched.py and gen_fft_codelets.py call split_kernels() themselves.
"""
import math
from pathlib import Path
import re
import sys

_KERNEL = re.compile(r'^(?:CRD_FFT_(?:GEN|CODELET)_INLINE )?void (\w+)\(')
_LOOP = re.compile(r'^    for \(crd::usize t = 0; t \+ \d+ <= \w+; t \+= \d+\)$')
_VALUE = re.compile(r'\bn\d+[ri]\b')
_VALUE_DEF = re.compile(r'(?:^\s*|[;{]\s*)(?:const )?V (n\d+[ri])\b', re.M)
_LOCAL_DEF = re.compile(r'^\s*const (?!V )[^=;]*?\b(\w+) = ')
_WORD = re.compile(r'\b[A-Za-z_]\w*\b')
_SUFFIX = '_part'
_WHOLE = 'CRD_FFT_GEN_WHOLE_BODY'
_LANES = 'crd::hesap::fft::lanes'
_RETYPE = {'    using V = crd::math::simd::Vec4d;': f'    using V = {_LANES}::Lane<crd::f64>;',
           '    using V = crd::math::simd::Vec8f;': f'    using V = {_LANES}::Lane<crd::f32>;',
           '    namespace simd = crd::math::simd;': f'    namespace simd = {_LANES};'}
_TILE_LOOP = re.compile(r'^    for \(crd::usize t = 0; t \+ (\d+) <= (\w+); t \+= \1\)$')
_LANE_OF = re.compile(rf'^    using V = {_LANES}::Lane<(crd::f(?:32|64))>;$')
_TILE = {'crd::f64': 4, 'crd::f32': 8}  # codelet_lanes.hpp's kTile: 32 bytes of columns
_MASK = re.compile(r'\bt & (\d+)\b')
_SHIFT = re.compile(r'\bt >> (\d+)\b')


def _items(lines):
    """Group loop-body lines into statements: one line, a `V x;` line with the next, or a `{ ... }` block."""
    items = []
    pending = []
    depth = 0
    for line in lines:
        pending.append(line)
        depth += line.count('{') - line.count('}')
        if depth < 0:
            raise ValueError('Unbalanced codelet body')
        if depth == 0 and not re.match(r'^\s*V n\d+[ri];$', line):
            items.append(pending)
            pending = []
    if pending or depth:
        raise ValueError('Codelet body ends inside a statement')
    return items


def _params(signature):
    text = ' '.join(part.strip() for part in signature)
    inner = text[text.index('(') + 1:text.rindex(') noexcept')]
    params = [' '.join(param.split()) for param in inner.split(',')]
    return params, [re.findall(r'\w+', param)[-1] for param in params]


def _wrap(opening, params, closing, width=120):
    """`opening` + the comma-separated `params` + `closing`, continued on lines indented four spaces."""
    lines = [opening]
    for index, param in enumerate(params):
        piece = param + (closing if index == len(params) - 1 else ',')
        joint = '' if lines[-1].endswith('(') else ' '
        if len(lines[-1]) + len(joint) + len(piece) > width and lines[-1].strip() != opening.strip():
            lines.append('    ' + piece)
        else:
            lines[-1] += joint + piece
    return lines


def _lane_driver(scalar, loop, calls, slots):
    """The tiles of `loop` on the lanes: the widest Lane while it holds whole tiles, then an odd last tile on a
    TileLane; `calls` are the part calls of one step at column `{col}` with the lane `{lane}` and slots `{slots}`."""
    width, bound = _TILE_LOOP.match(loop).groups()
    if int(width) != _TILE[scalar]:
        raise ValueError(f'A tile of {scalar} is {_TILE[scalar]} columns, not {width}')
    if bound.isdigit():
        tiles = str(int(bound) - int(bound) % int(width))
    else:
        tiles = f'{bound} - {bound} % lanes::kTile<{scalar}>'

    def step(indent, lane, col, array):
        return [indent + call.format(lane=lane, col=col, slots=array) for call in calls]

    out = [f'    namespace lanes = {_LANES};', f'    using V = lanes::Lane<{scalar}>;']
    out += [f'    alignas(64) V sp[{slots}];'] if slots else []
    out += [f'    const crd::usize tiles = {tiles};', '    const crd::usize wide = tiles - tiles % lanes::kWidth<V>;',
            '    for (crd::usize t = 0; t < wide; t += lanes::kWidth<V>)', '    {']
    out += step('        ', 'V', 't', 'sp')
    out += ['    }', f'    if constexpr (lanes::kPairedTiles<{scalar}>)', '    {',
            f'        using H = lanes::TileLane<{scalar}>;']
    out += [f'        alignas(64) H sh[{slots}];'] if slots else []
    out += ['        if (wide < tiles)', '        {'] + step('            ', 'H', 'wide', 'sh')
    return out + ['        }', '    }']


def _closure(text, locals_):
    """The local definitions `text` needs, transitively, in their original order."""
    needed = set(_WORD.findall(text))
    chosen = set()
    for name, line in reversed(locals_):
        if name in needed:
            chosen.add(name)
            needed |= set(_WORD.findall(line))
    return [line for name, line in locals_ if name in chosen]


def _split_kernel(name, signature, prologue, loop, body, budget):
    items = _items(body)
    head = []
    while items and not _VALUE.search(''.join(items[0])):
        if len(items[0]) != 1 or not _LOCAL_DEF.match(items[0][0]):
            raise ValueError(f'{name}: unexpected tile prologue {items[0]!r}')
        head.append(items.pop(0)[0])
    weights = [len(item) for item in items]
    # A step of the widest Lane must not straddle a column block the body indexes by t.
    width = 2 * _TILE[_LANE_OF.match(prologue[0]).group(1)]
    whole = '\n'.join(body)
    blocks = [int(m) + 1 for m in _MASK.findall(whole)] + [1 << int(s) for s in _SHIFT.findall(whole)]
    if any(block % width for block in blocks):
        raise ValueError(f'{name}: a {width}-column step straddles a column block of {min(blocks)}')
    defs = []
    uses = []
    for item in items:
        text = '\n'.join(item)
        d = set(_VALUE_DEF.findall(text))
        defs.append(d)
        uses.append(set(_VALUE.findall(text)) - d)
    def_at = {}
    last_use = {}
    for index, (d, u) in enumerate(zip(defs, uses)):
        for value in d:
            def_at[value] = index
        for value in u:
            if value not in def_at:
                raise ValueError(f'{name}: {value} is used before its definition')
            last_use[value] = index
    # crossing[p] = values defined before item p and used at or after it.
    delta = [0] * (len(items) + 1)
    for value, start in def_at.items():
        end = last_use.get(value, start)
        if end > start:
            delta[start + 1] += 1
            delta[end + 1] -= 1
    crossing = []
    running = 0
    for step in delta:
        running += step
        crossing.append(running)
    cuts = []
    start = 0
    while True:
        total = 0
        stop = start
        while stop < len(items) and total + weights[stop] <= budget:
            total += weights[stop]
            stop += 1
        if stop == len(items):
            break
        stop = max(stop, start + 1)
        low = start + max(1, (stop - start) * 3 // 4)
        cut = min(range(low, stop + 1), key=lambda p: (crossing[p], -p))
        cuts.append(cut)
        start = cut
    bounds = [0] + cuts + [len(items)]
    part_of = []
    for part, (lo, hi) in enumerate(zip(bounds, bounds[1:])):
        part_of += [part] * (hi - lo)
    # A crossing value holds one slot from the end of its defining part to its last reading part.
    spans = []
    for value, start_item in def_at.items():
        first = part_of[start_item]
        last = part_of[last_use.get(value, start_item)]
        if last > first:
            spans.append((first, last, value))
    spans.sort()
    slot_of = {}
    free = []
    busy = []
    slots = 0
    for first, last, value in spans:
        for entry in [entry for entry in busy if entry[0] <= first]:
            busy.remove(entry)
            free.append(entry[1])
        free.sort()
        slot = free.pop(0) if free else slots
        slots = max(slots, slot + 1)
        slot_of[value] = slot
        busy.append((last, slot))
    # Every body runs in parts, templates of the lane, so one text serves a Lane and a TileLane.
    params, names = _params(signature)
    part_params = [f'[[maybe_unused]] {param}' for param in params]
    invariant = [(m.group(1), line) for line in prologue[2:] if (m := _LOCAL_DEF.match(line))]
    if len(invariant) != len(prologue) - 2:
        raise ValueError(f'{name}: unexpected kernel prologue')
    tile_locals = [(_LOCAL_DEF.match(line).group(1), line) for line in head]
    out = []
    for part, (lo, hi) in enumerate(zip(bounds, bounds[1:])):
        body_text = [line for item in items[lo:hi] for line in item]
        loads = sorted({v for i in range(lo, hi) for v in uses[i] if part_of[def_at[v]] < part},
                       key=lambda v: slot_of[v])
        stores = sorted({v for i in range(lo, hi) for v in defs[i] if v in slot_of}, key=lambda v: slot_of[v])
        text = '\n'.join(body_text)
        locals_text = _closure(text, invariant + tile_locals)
        out.append('// NOLINTNEXTLINE(readability-function-size) -- One part of a generated scheduled DAG.')
        slot_param = [f'{"" if stores else "const "}V* sp'] if slots else []
        out += _wrap(f'template <typename V> CRD_NOINLINE static void {name}{_SUFFIX}{part}(',
                     part_params + ['[[maybe_unused]] crd::usize t'] + slot_param, ') noexcept')
        out.append('{')
        if 'simd::' in text:
            out.append(prologue[1])
        out += [line[4:] if line.startswith('        ') else line for line in locals_text]
        out += [f'    const V {v} = sp[{slot_of[v]}];' for v in loads]
        out += [line[4:] for line in body_text]
        out += [f'    sp[{slot_of[v]}] = {v};' for v in stores]
        out.append('}')
    args = ', '.join(names)
    slot_arg = ', {slots}' if slots else ''
    calls = [f'{name}{_SUFFIX}{part}<{{lane}}>({args}, {{col}}{slot_arg});' for part in range(len(bounds) - 1)]
    return out, _lane_driver(_LANE_OF.match(prologue[0]).group(1), loop, calls, slots)


def split_kernels(source, budget, units):
    """Return (header, unit bodies): `source` with each codelet replaced by its declaration, and `units` texts, each
    the header's namespace holding the definitions of that unit's codelets twice: whole under CRD_FFT_GEN_WHOLE_BODY,
    else with tile bodies longer than `budget` lines in parts. The split edition's lines balance the units."""
    lines = source.split('\n')
    head = []
    kernels = []
    tail = []
    index = 0
    while index < len(lines):
        match = _KERNEL.match(lines[index])
        if not match:
            (tail if kernels else head).append(lines[index])
            index += 1
            continue
        if tail:
            raise ValueError('Text between codelets other than their comments')
        name = match.group(1)
        comment = []
        while head and head[-1].startswith('//') and not kernels:
            comment.insert(0, head.pop())
        while kernels and kernels[-1]['trailing']:
            comment.insert(0, kernels[-1]['trailing'].pop())
        start = index
        while not lines[index].endswith(') noexcept'):
            index += 1
        signature = [re.sub(r'^CRD_FFT_(?:GEN|CODELET)_INLINE ', '', lines[start])] + lines[start + 1:index + 1]
        if lines[index + 1] != '{':
            raise ValueError(f'{name}: expected the body')
        index += 2
        prologue = []
        while not _LOOP.match(lines[index]):
            prologue.append(lines[index])
            index += 1
        loop = lines[index]
        if lines[index + 1] != '    {':
            raise ValueError(f'{name}: expected the tile loop body')
        index += 2
        body = []
        while lines[index] != '    }':
            body.append(lines[index])
            index += 1
        index += 1
        rest = []
        while lines[index] != '}':
            rest.append(lines[index])
            index += 1
        rest.append(lines[index])
        index += 1
        if len(prologue) < 2 or not _LANE_OF.match(prologue[0]) or prologue[1] != f'    namespace simd = {_LANES};' or (
                not _TILE_LOOP.match(loop)):
            raise ValueError(f'{name}: unexpected kernel prologue')
        # The invariant locals a scalar tail reads.
        invariant = [(_LOCAL_DEF.match(line).group(1), line) for line in prologue[2:]]
        tail_locals = _closure('\n'.join(rest), invariant)
        editions = []
        for edition_budget in (budget, math.inf):
            parts, driver = _split_kernel(name, signature, prologue, loop, body, edition_budget)
            editions.append(parts + comment + signature + ['{'] + driver + tail_locals + rest)
        declaration = [line for line in comment if 'NOLINT' not in line] + signature[:-1] + [signature[-1] + ';']
        # Comments and blank lines after a codelet belong to the next one; the last codelet's go to the tail.
        kernels.append({'declaration': declaration, 'definition': editions[0], 'whole': editions[1], 'trailing': []})
        while index < len(lines) and not _KERNEL.match(lines[index]) and (
                lines[index].startswith('//') or not lines[index].strip()):
            kernels[-1]['trailing'].append(lines[index])
            index += 1
    if not kernels or len(kernels) < units:
        raise ValueError(f'{len(kernels)} codelets cannot fill {units} units')
    tail = kernels[-1]['trailing'] + tail
    opening = head[-1] if head else ''
    closing = next((line for line in tail if line.strip()), '')
    if not opening.startswith('namespace ') or not opening.endswith(' {') or not closing.startswith('} // namespace'):
        raise ValueError('The codelets must sit in one namespace that closes the header')
    load = [0] * units
    unit_of = {}
    for k in sorted(range(len(kernels)), key=lambda k: (-len(kernels[k]['definition']), k)):
        unit = min(range(units), key=lambda u: (load[u], u))
        unit_of[k] = unit
        load[unit] += len(kernels[k]['definition'])
    header = head + [line for kernel in kernels for line in kernel['declaration']] + tail
    bodies = []
    for unit in range(units):
        mine = [kernel for k, kernel in enumerate(kernels) if unit_of[k] == unit]
        whole = [line for kernel in mine for line in kernel['whole']]
        split = [line for kernel in mine for line in kernel['definition']]
        bodies.append('\n'.join([opening, f'#ifdef {_WHOLE}'] + whole + ['#else'] + split + ['#endif', closing, '']))
    return '\n'.join(header), bodies


def main(argv):
    lanes = argv[:1] == ['--lanes']
    argv = argv[1:] if lanes else argv
    if len(argv) != 4:
        raise SystemExit(__doc__)
    header, budget, units, stem = Path(argv[0]), int(argv[1]), int(argv[2]), argv[3]
    source = header.read_text(encoding='utf-8')
    if lanes:
        source = '\n'.join(_RETYPE.get(line, line) for line in source.split('\n'))
    origin = re.search(r'GENERATED by (\S+)', source).group(1)
    declarations, bodies = split_kernels(source, budget, units)
    header.write_text(declarations, encoding='utf-8', newline='\n')
    include = header.as_posix().split('/include/', 1)[1]
    for unit, body in enumerate(bodies):
        text = (f'// GENERATED by {origin} — codelet unit {unit} of {units}: definitions of {header.name}, moved here\n'
                '// by scripts/fft_codegen_split.py.\n'
                f'#include <{include}>\n#include <crd/core/platform.hpp>\n'
                '#include <crd/hesap/fft/detail/codelet_lanes.hpp>\n' + body)
        Path(f'{stem}_{unit}.cpp').write_text(text, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main(sys.argv[1:])
