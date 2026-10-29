#!/usr/bin/env python3
"""ISA isolation guard (ADR-0134 §6): general code holds nothing above the CPU floor, and per-level code shares no
symbol with it.

The build writes a manifest per configuration (cmake/CrdSimd.cmake, crd_add_isa_isolation_check): each kernel object
with its level, each general object with its target and archive, and each image. Three judgments:

  symbols  A kernel object defines non-local symbols only in its level namespace, apart from its table and the
           compiler's constants; a general object defines nothing in a level namespace and no level's table. This is
           the shared-COMDAT class (the 2026-10-26 LTCG inlining defect), judged on names, not disassembly.
  ceiling  A kernel object holds no instruction above its level; an avx2 unit holds VEX code, an avx512 unit EVEX
           code, so a level flag that stops reaching its units fails too.
  floor    General code holds no instruction above the baseline: on x64 no VEX, EVEX, REX2 or XOP encoding and no
           legacy instruction of a later level (SSE3 to SSE4.2, POPCNT, LZCNT, MOVBE, AES, SHA, XSAVE ...); on ARM64 no
           SVE, SME, LSE or later-revision instruction.

Objects that hold link-time IL (MSVC /GL, clang ThinLTO bitcode, GCC slim LTO) are judged on the linked images: MSVC
and clang-cl images through their /MAP map (each function's object, MSVC's jump tables in .text as data), ELF images
through their symbol tables, while the symbols of GCC's IL objects come from gcc-nm. scripts/isa-isolation.json holds
the reviewed exceptions, each naming the runtime check that guards it. Contract: docs/decisions/0134-runtime-cpu-
dispatch.md §6.

    python scripts/check-isa-isolation.py --arch x64 --os windows
        --manifest build/win-debug/isa-isolation/Debug.manifest
"""
import argparse
import bisect
import concurrent.futures
import json
import locale
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXCEPTIONS = ROOT / 'scripts/isa-isolation.json'
SCHEMA = 'cerid-isa-isolation/1'

# The level namespaces (crd/math/kernel_level.hpp) and the suffix each level's table name ends with.
SUFFIX = {'portable': 'Portable', 'sse2': 'Sse2', 'sse42': 'Sse42', 'avx2': 'Avx2', 'avx512': 'Avx512',
          'neon': 'Neon', 'avx2_avxvnni': 'Avx2AvxVnni', 'avx512_avx512vnni': 'Avx512Avx512Vnni'}
_NS = '|'.join(sorted(SUFFIX, key=len, reverse=True))


def namespace_pattern(names):
    """A level namespace in a demangled name (`::avx2::`) or an MSVC decorated one: a scope `@avx2@`, or the first
    scope after an operator's code (`??Havx2@` is operator+ in avx2)."""
    return re.compile(rf'(?:(?<![A-Za-z0-9_])(?:{names})::|@(?:{names})@|^\?\?_?[0-9A-Z](?:{names})@)')


LEVEL_NS = namespace_pattern(_NS)
TABLE_NAME = re.compile(rf'^k[A-Z]\w*?(?:{"|".join(sorted(SUFFIX.values(), key=len, reverse=True))})$')

# x86-64 instructions above v1, by the first level that has them (psABI levels); VEX, EVEX, REX2 and XOP come from the
# encoding. TZCNT is absent: every x86-64 CPU executes its encoding as BSF, which compilers rely on.
X64_V2 = frozenset('''
    addsubpd addsubps haddpd haddps hsubpd hsubps lddqu movddup movshdup movsldup fisttp monitor mwait
    pabsb pabsd pabsw palignr phaddd phaddsw phaddw phsubd phsubsw phsubw pmaddubsw pmulhrsw pshufb psignb psignd
    psignw blendpd blendps blendvpd blendvps dppd dpps extractps insertps movntdqa mpsadbw packusdw pblendvb pblendw
    pcmpeqq pextrb pextrd pextrq phminposuw pinsrb pinsrd pinsrq pmaxsb pmaxsd pmaxud pmaxuw pminsb pminsd pminud
    pminuw pmovsxbd pmovsxbq pmovsxbw pmovsxdq pmovsxwd pmovsxwq pmovzxbd pmovzxbq pmovzxbw pmovzxdq pmovzxwd pmovzxwq
    pmuldq pmulld ptest roundpd roundps roundsd roundss crc32 pcmpestri pcmpestrm pcmpgtq pcmpistri pcmpistrm popcnt
    cmpxchg16b lahf sahf'''.split())
X64_V3 = frozenset('lzcnt movbe'.split())
X64_OTHER = frozenset('''
    aesdec aesdeclast aesenc aesenclast aesimc aeskeygenassist pclmulqdq pclmullqlqdq pclmulhqlqdq pclmullqhqdq
    pclmulhqhqdq sha1msg1 sha1msg2 sha1nexte sha1rnds4 sha256msg1 sha256msg2 sha256rnds2 gf2p8affineqb
    gf2p8affineinvqb gf2p8mulb rdrand rdseed adcx adox xgetbv xsave xsave64 xsaveopt xsaveopt64 xsavec xsavec64 xsaves
    xsaves64 xrstor xrstor64 xrstors xrstors64 rdtscp rdpid clflushopt clwb prefetchw prefetchwt1'''.split())
# Windows 8.1 and later require CMPXCHG16B, PREFETCHW and LAHF/SAHF of every x64 CPU, so they are inside the floor of
# COFF code.
WINDOWS_FLOOR = frozenset('cmpxchg16b prefetchw lahf sahf'.split())
X64_LEGACY = X64_V2 | X64_V3 | X64_OTHER
# The legacy instructions each kernel level may hold; EVEX only at avx512, VEX from avx2.
LEVEL_LEGACY = {'portable': frozenset(), 'sse2': frozenset(), 'sse42': X64_V2, 'avx2': X64_V2 | X64_V3,
                'avx512': X64_V2 | X64_V3}
LEVEL_ENCODINGS = {'portable': frozenset(), 'sse2': frozenset(), 'sse42': frozenset(),
                   'avx2': frozenset({'VEX'}), 'avx512': frozenset({'VEX', 'EVEX'})}
LEVEL_PRESENT = {'avx2': 'VEX', 'avx512': 'EVEX'}
_SEGMENT = frozenset({0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x67})
_PREFIX_WORDS = r'(?:(?:lock|rep|repe|repz|repne|repnz|data16|addr32|notrack|bnd|cs|ds|es|ss|fs|gs) +)*'
_LEGACY = '|'.join(sorted(X64_LEGACY, key=len, reverse=True))

# ARM64 above the v8.0 baseline with NEON: SVE and SME registers, LSE atomics and later-revision instructions. A
# register is an operand between the operand delimiters, never part of the symbol a disassembler prints for a branch
# target (objdump's `bl 11c <ns::Rng::p3(float)>`, dumpbin's decorated names); branches and address forms name no
# vector register at all.
ARM64_SVE = re.compile(r'(?:^|(?<=[\s{\[,]))(?:z\d{1,2}|p\d{1,2}|za\w*)(?=$|[\s.,/\[\]}])')
ARM64_TARGET = re.compile(r'^(?:b|bl|b\.\w+|cbn?z|tbn?z|adrp?)$')
ARM64_ABOVE = re.compile(r'^(?:(?:ld|st)(?:add|clr|eor|set|smax|smin|umax|umin)(?:a|l|al)?(?:b|h)?|swp(?:a|l|al)?'
                         r'(?:b|h)?|casp?(?:a|l|al)?(?:b|h)?|ldapu?r\w*|stlur\w*|[su]dot|usdot|sudot|[su]mmla|usmmla|'
                         r'bf(?:dot|mmla|mlal[bt]|cvtn?2?)|fcadd|fcmla|fjcvtzs|crc32\w*|aes[edm]\w*|aesimc|sha1\w+|'
                         r'sha256\w+|sha512\w+|sm3\w+|sm4\w+|rax1|eor3|bcax|xar|smstart|smstop|frint(?:32|64)[xz])$')
ARM64_FP16 = re.compile(r'^f(?:add|sub|mul|div|madd|msub|nmadd|nmsub|abs|neg|sqrt|max|min|maxnm|minnm|cmp|cmpe|ccmp|'
                        r'mla|mls|mulx|recpe|rsqrte|cvtz[su]|cvta[su])$')
ARM64_HALF = re.compile(r'^(?:h\d+|v\d+\.[48]h)\b')

# The disassembler and symbol-table line shapes this guard reads.
DUMPBIN_FILE = re.compile(r'^Dump of file (.+?)\s*$', re.M)
DUMPBIN_ARM64 = re.compile(r'^  ([0-9A-F]{8,16}): ([0-9A-F]{8})  (\S+) *(.*)$')
DUMPBIN_SYMBOL = re.compile(r'^[0-9A-F]+ ([0-9A-F]{8}) SECT([0-9A-F]+) +\w+( \(\))? +(External|Static|Label) +\| (\S+)')
OBJDUMP_FILE = re.compile(r'^(\S.*?):\s+file format (\S+)\s*$', re.M)
OBJDUMP_LABEL = re.compile(r'^[0-9a-f]+ <(.+)>:$')
OBJDUMP_ARM64 = re.compile(r'^ *([0-9a-f]+):\t([0-9a-f]{8})\s+(?:\t)?(\S+)\s*(.*)$')
NM_LINE = re.compile(r'^(.*?):([0-9a-fA-F]+)? +([A-Za-z?]) (.+)$')
MAP_SEGMENT = re.compile(r'^ ([0-9a-f]{4}):([0-9a-f]{8}) ([0-9a-f]{8})H (\S+) +(CODE|DATA)\s*$')
MAP_SYMBOL = re.compile(r'^ ([0-9a-f]{4}):([0-9a-f]{8}) +(\S+) +([0-9a-f]{16})(?: +f)?(?: +i)? +(\S.*?)\s*$')
MAP_BASE = re.compile(r'Preferred load address is ([0-9a-f]+)')
_LEGACY_LINE_COFF = re.compile(rf'^  ([0-9A-F]{{8,16}}): (?:[0-9A-F]{{2}} )+ *{_PREFIX_WORDS}({_LEGACY})\b', re.M)
_LEGACY_LINE_ELF = re.compile(rf'^ *([0-9a-f]+):\t(?:[0-9a-f]{{2}} )+\s*\t{_PREFIX_WORDS}({_LEGACY})\b', re.M)
_ENCODED_COFF = re.compile(r'^  ([0-9A-F]{8,16}): ((?:(?:26|2E|36|3E|64|65|67) )*(?:C4|C5|62|D5|8F [0-9A-F]{2}))',
                           re.M)
_ENCODED_ELF = re.compile(r'^ *([0-9a-f]+):\t((?:(?:26|2e|36|3e|64|65|67) )*(?:c4|c5|62|d5|8f [0-9a-f]{2}))', re.M)
_LABEL_COFF = re.compile(r'^([^ \n][^\n]*):$', re.M)
_ADDRESS_COFF = re.compile(r'^  ([0-9A-F]{8,16}):', re.M)
_LABEL_ELF = re.compile(r'^[0-9a-f]+ <([^\n]+)>:$', re.M)
_NOT_LABELS = ('Dump of file', 'File Type', 'Summary')
INSTRUCTION = {('x64', True): _ADDRESS_COFF, ('x64', False): re.compile(r'^ *[0-9a-f]+:\t[0-9a-f]{2} ', re.M),
               ('arm64', True): re.compile(r'^  [0-9A-F]{8,16}: [0-9A-F]{8}  ', re.M),
               ('arm64', False): re.compile(r'^ *[0-9a-f]+:\t[0-9a-f]{8} ', re.M)}

BIGOBJ_CLASS = bytes.fromhex('c7a1bad1eebaa94baf20faf66aa4dcb8')
# Objects per decoder call, by count and bytes, and the decoder text an image is judged in at a time: the dump of an
# unoptimized object is ~10x its size, and a process per CPU must stay within the runner's memory.
BATCH = 16
BATCH_BYTES = 8 << 20
CHUNK = 32 << 20


class GuardError(Exception):
    """A tool or input problem: nothing could be judged."""


def encoding(first):
    """VEX, EVEX, REX2 or XOP for an x64 instruction's leading bytes in 64-bit mode, else None.

    Only segment overrides and 67 may precede a VEX, EVEX or REX2 prefix (66, F2, F3, F0 or REX before one is #UD,
    and no compiler emits it). C4/C5, 62 and D5 are LES/LDS, BOUND and AAD outside 64-bit mode; 8F is POP r/m unless
    its ModRM reg field is nonzero, which marks XOP.
    """
    i = 0
    while i < len(first) and first[i] in _SEGMENT:
        i += 1
    if i >= len(first):
        return None
    lead = first[i]
    if lead in (0xC4, 0xC5):
        return 'VEX'
    if lead == 0x62:
        return 'EVEX'
    if lead == 0xD5:
        return 'REX2'
    if lead == 0x8F and i + 1 < len(first) and (first[i + 1] >> 3) & 7:
        return 'XOP'
    return None


def arm64_above(mnemonic, operands):
    """The ARM64 extension an instruction needs beyond v8.0 with NEON, else None."""
    if mnemonic.startswith('.') or mnemonic in ('dcd', 'dcq', 'udf'):
        return None
    if not ARM64_TARGET.match(mnemonic) and ARM64_SVE.search(operands):
        return 'SVE/SME'
    if ARM64_ABOVE.match(mnemonic):
        return mnemonic
    if ARM64_FP16.match(mnemonic) and ARM64_HALF.match(operands):
        return 'FP16'
    return None


def object_format(path):
    """coff, elf, ltcg (MSVC /GL IL), bitcode (LLVM) or gnu-lto (GCC slim LTO) for an object file."""
    with open(path, 'rb') as stream:
        head = stream.read(64)
        if head[:4] in (b'BC\xc0\xde', b'\xde\xc0\x17\x0b'):
            return 'bitcode'
        if head[:4] == b'\x7fELF':
            return _elf_format(stream, head)
    if head[:4] == b'\x00\x00\xff\xff':
        return 'coff' if head[12:28] == BIGOBJ_CLASS else 'ltcg'
    if head[:2] in (b'\x64\x86', b'\x64\xaa'):
        return 'coff'
    raise GuardError(f'unknown object format: {path}')


def _elf_format(stream, head):
    """elf, or gnu-lto when the object holds GCC LTO sections and no code."""
    lto, code = _elf_contents(stream, head)
    return 'gnu-lto' if lto and not code else 'elf'


def elf_has_code(path):
    """Whether an ELF object holds a non-empty executable section."""
    with open(path, 'rb') as stream:
        return _elf_contents(stream, stream.read(64))[1]


def _elf_contents(stream, head):
    """(GCC LTO sections present, a non-empty executable section present) for an ELF64 object."""
    if head[4] != 2 or head[5] != 1:
        raise GuardError('only little-endian ELF64 objects are judged')
    shoff, = struct.unpack_from('<Q', head, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', head, 0x3A)
    stream.seek(shoff)
    table = stream.read(shentsize * shnum)
    sections = [struct.unpack_from('<IIQQQQ', table, i * shentsize) for i in range(shnum)]
    name_offset, size = sections[shstrndx][4], sections[shstrndx][5]
    stream.seek(name_offset)
    names = stream.read(size)
    lto = code = False
    for name, _type, flags, _addr, _offset, length in sections:
        text = names[name:names.index(b'\0', name)]
        lto |= text.startswith(b'.gnu.lto_')
        code |= bool(flags & 0x4) and length > 0
    return lto, code


def _run(command, empty_ok=False):
    try:
        done = subprocess.run(command, capture_output=True, text=True, errors='replace', check=False)
    except OSError as error:
        raise GuardError(f'{command[0]}: {error}') from error
    if done.returncode != 0:
        raise GuardError(f'{Path(command[0]).name} exited {done.returncode}: {done.stderr.strip()[:400]}')
    if not empty_ok and not done.stdout.strip():
        raise GuardError(f'{Path(command[0]).name} printed nothing for {command[-1]}')
    return done.stdout


def _stream(command, label):
    """A decoder's output in chunks of about CHUNK characters ending at a line; a chunk after the first starts with
    the last `label` line before it, so each instruction keeps its function. The pipe is read as bytes and decoded a
    block at a time: large reads of a text-mode pipe crawl on Windows (16 s for a 20 MB dump, 0.3 s in bytes)."""
    encoding = locale.getpreferredencoding(False)

    def decode(data):
        return data.replace(b'\r\n', b'\n').decode(encoding, errors='replace')
    carry, pending, printed = '', b'', False
    with tempfile.TemporaryFile() as errors:
        try:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors)
        except OSError as error:
            raise GuardError(f'{command[0]}: {error}') from error
        with process:
            while block := process.stdout.read(CHUNK):
                data = pending + block
                cut = data.rfind(b'\n') + 1
                pending = data[cut:]
                text = decode(data[:cut])
                if not text:
                    continue
                printed = printed or bool(text.strip())
                chunk = carry + text
                labels = list(label.finditer(chunk))
                if labels:
                    carry = labels[-1].group(0) + '\n'
                yield chunk
        tail = decode(pending)
        if tail.strip():
            printed = True
            yield carry + tail
        if process.returncode != 0:
            errors.seek(0)
            message = errors.read().decode(errors='replace').strip()[:400]
            raise GuardError(f'{Path(command[0]).name} exited {process.returncode}: {message}')
    if not printed:
        raise GuardError(f'{Path(command[0]).name} printed nothing for {command[-1]}')


def _split_files(text, header, paths):
    """Each input file's part of a multi-file dump, by the decoder's file headers."""
    starts = [(match.start(), match.group(1)) for match in header.finditer(text)]
    by_name = {os.path.normcase(os.path.abspath(path)): path for path in paths}
    parts = {}
    for index, (start, name) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(text)
        path = by_name.get(os.path.normcase(os.path.abspath(name)))
        if path is None:
            raise GuardError(f'the decoder named a file it was not given: {name}')
        parts[path] = text[start:end]
    missing = [path for path in paths if path not in parts]
    if missing:
        raise GuardError(f'the decoder printed nothing for {missing[0]}')
    return parts


def _function_name(label, coff):
    """A dump label's function: dumpbin follows a decorated name with ` (<undecorated>)`; objdump -C prints the
    demangled name alone, whose ` (anonymous namespace)` or return type is part of it."""
    return label.split(' (')[0] if coff else label


def _labels(text, pattern, layout=None):
    """(functions, data): (offset, function) for each function label of a dump, and the [start, end) text ranges that
    hold data. A $ label (MSVC's $LN) belongs to its function; it starts data when the object's symbol table makes it
    a jump table (`layout`: (section, offset) of each Static $LN symbol, and each function's section), which runs to
    the next function, since MSVC puts a function's tables after its code."""
    functions, data = [], []
    tables, sections = layout or (set(), {})
    section, open_at = None, None
    for match in pattern.finditer(text):
        name = _function_name(match.group(1), pattern is _LABEL_COFF)
        if name.startswith(_NOT_LABELS):
            continue
        if not name.startswith('$'):
            if open_at is not None:
                data.append((open_at, match.start()))
                open_at = None
            functions.append((match.start(), name))
            section = sections.get(name)
            continue
        address = _ADDRESS_COFF.search(text, match.end())
        if open_at is None and address and (section, int(address.group(1), 16)) in tables:
            open_at = match.start()
    if open_at is not None:
        data.append((open_at, len(text)))
    return functions, data


def _in_ranges(ranges, offset):
    index = bisect.bisect_right(ranges, (offset, float('inf'))) - 1
    return index >= 0 and offset < ranges[index][1]


def _owner_at(labels, offset):
    index = bisect.bisect_right(labels, (offset, '\uffff')) - 1
    return labels[index][1] if index >= 0 else ''


def scan_x64(text, coff, layout=None):
    """Every above-floor instruction of an x64 dump: (function, address, category, line). `layout` locates an MSVC
    object's jump tables (_labels)."""
    labels, data = _labels(text, _LABEL_COFF if coff else _LABEL_ELF, layout)
    hits = []
    for match in (_ENCODED_COFF if coff else _ENCODED_ELF).finditer(text):
        kind = encoding([int(byte, 16) for byte in match.group(2).split()])
        if kind and not _in_ranges(data, match.start()):
            hits.append((_owner_at(labels, match.start()), int(match.group(1), 16), kind, _line(text, match.start())))
    for match in (_LEGACY_LINE_COFF if coff else _LEGACY_LINE_ELF).finditer(text):
        mnemonic = match.group(2)
        if coff and mnemonic in WINDOWS_FLOOR or _in_ranges(data, match.start()):
            continue
        hits.append((_owner_at(labels, match.start()), int(match.group(1), 16), mnemonic, _line(text, match.start())))
    return hits


def arm64_literal(word):
    """(offset, size) of the data an LDR (literal) instruction word loads, relative to the instruction, else None:
    opc 011 V 00 imm19 Rt, 4 bytes for W, S and LDRSW, 8 for X and D, 16 for Q; PRFM (literal) loads nothing."""
    if word & 0x3B000000 != 0x18000000:
        return None
    size = {(0, 0): 4, (1, 0): 8, (2, 0): 4, (0, 1): 4, (1, 1): 8, (2, 1): 16}.get((word >> 30, (word >> 26) & 1))
    if size is None:
        return None
    offset = (word >> 5) & 0x7FFFF
    return (offset - (1 << 19) if offset & (1 << 18) else offset) * 4, size


def arm64_adr(word):
    """(Rd, offset) of an ADR instruction word, the address it forms relative to itself, else None (ADRP forms a
    page): op 0 immlo 10000 immhi Rd."""
    if word & 0x9F000000 != 0x10000000:
        return None
    offset = ((word >> 5) & 0x7FFFF) << 2 | (word >> 29) & 3
    return word & 31, offset - (1 << 21) if offset & (1 << 20) else offset


def arm64_indexed_load(word):
    """Rn of a general-register load whose address is a base register plus an index register (LDR, LDRB, LDRH and
    their sign-extending forms, register offset), else None: size 111 0 00 opc 1 Rm option S 10 Rn Rt, where opc 00
    is a store and size 11 with opc 10 is PRFM."""
    if word & 0x3F200C00 != 0x38200800:
        return None
    opc = (word >> 22) & 3
    if opc == 0 or (word >> 30 == 3 and opc == 2):
        return None
    return (word >> 5) & 31


# A switch reads its table through the register an ADR set this many instructions before at most.
_ARM64_TABLE_READ = 4


def scan_arm64(text, coff):
    """Every ARM64 instruction above v8.0 with NEON in a dump. MSVC puts a function's constants in .text after its
    code, where dumpbin decodes them as instructions: the words an LDR (literal) of the same section loads, and a
    switch table to the next function, are data. A table is under a __swt label or, in its compact byte and halfword
    forms, the address an ADR forms for an indexed load (`adr x9,T; ldrb w8,[x9,w8 uxtw #0]`); the switch's second ADR
    forms the address of its cases' code, which no load reads. Sections start at 0 in an object's dump, so a falling
    address starts the next section's ranges. objdump prints ELF data (its $d mapping symbols) as .word."""
    hits, pending, data = [], [], []
    function, table, last = '', False, -1
    adrs, tables, closing = {}, [], False
    line_shape = DUMPBIN_ARM64 if coff else OBJDUMP_ARM64

    def close_tables(end):
        data.extend((start, end) for start in tables)
        tables.clear()

    def settle():
        close_tables(float('inf'))
        hits.extend(hit for hit in pending if not any(start <= hit[1] < end for start, end in data))
        pending.clear()
        data.clear()
    for line in text.splitlines():
        match = line_shape.match(line)
        if match:
            address = int(match.group(1), 16)
            if address <= last:
                settle()
                adrs.clear()
            elif closing:
                close_tables(address)
            closing = False
            last = address
            if table:
                continue
            word = int(match.group(2), 16)
            if coff:
                literal = arm64_literal(word)
                if literal:
                    data.append((address + literal[0], address + literal[0] + literal[1]))
                adr, base = arm64_adr(word), arm64_indexed_load(word)
                if adr:
                    adrs[adr[0]] = (address + adr[1], address)
                elif base is not None and base in adrs:
                    start, formed = adrs.pop(base)
                    if address - formed <= 4 * _ARM64_TABLE_READ and start > address:
                        tables.append(start)
            kind = arm64_above(match.group(3).lower(), match.group(4))
            if kind:
                pending.append((function, address, kind, line.strip()))
            continue
        label = (re.match(r'^([^ ].*):$', line) if coff else OBJDUMP_LABEL.match(line))
        if label and not label.group(1).startswith(_NOT_LABELS) and not label.group(1).startswith('$'):
            name = _function_name(label.group(1), coff)
            table = coff and name.startswith('__swt')
            function = function if table else name
            if not table:
                closing = True
                adrs.clear()
    settle()
    return hits


def _line(text, start):
    end = text.find('\n', start)
    return text[start:end if end >= 0 else len(text)].strip()


def _references(text, coff, symbol):
    """The functions of a dump whose instructions name `symbol`."""
    labels, _data = _labels(text, _LABEL_COFF if coff else _LABEL_ELF)
    return {_owner_at(labels, match.start()) for match in re.finditer(re.escape(symbol), text)}


def disassemble(tools, paths, arch, coff, layouts):
    """{path: (hits, references, decoded)}: each object's above-floor instructions, its guard-symbol functions and its
    number of decoded instructions."""
    if coff:
        text = _run([tools['dumpbin'], '/nologo', '/disasm'] + paths)
        parts = _split_files(text, DUMPBIN_FILE, paths)
    else:
        command = [tools['objdump'], '-d', '-w', '-C'] + (['-M', 'intel'] if arch == 'x64' else []) + paths
        text = _run(command)
        parts = _split_files(text, OBJDUMP_FILE, paths)
    results = {}
    for path, part in parts.items():
        hits = scan_x64(part, coff, layouts.get(path)) if arch == 'x64' else scan_arm64(part, coff)
        guarded = _references(part, coff, '__isa_available') if arch == 'x64' and coff else set()
        results[path] = (hits, guarded, len(INSTRUCTION[(arch, coff)].findall(part)))
    return results


def coff_symbols(part):
    """A COFF symbol table: the defined [(name, external)], and the layout _labels reads (each Static $LN symbol's
    (section, offset), each function's section; an object without functions has none)."""
    defined, tables, sections = [], set(), {}
    for line in part.splitlines():
        match = DUMPBIN_SYMBOL.match(line)
        if not match:
            continue
        value, section, function, storage, name = match.groups()
        if storage == 'Static' and re.match(r'^\$LN\d+$', name):
            tables.add((section, int(value, 16)))
        elif function and storage != 'Label':
            sections[name] = section
        if storage != 'Label':
            defined.append((name, storage == 'External'))
    return defined, (tables, sections)


def symbols(tools, paths, coff, reader='nm'):
    """{path: [(name, global)]}: the symbols each object defines, {path: layout} for COFF objects, and the objects
    that define a function."""
    found = {path: [] for path in paths}
    layouts, code = {}, set()
    if coff:
        text = _run([tools['dumpbin'], '/nologo', '/symbols'] + paths)
        for path, part in _split_files(text, DUMPBIN_FILE, paths).items():
            found[path], layouts[path] = coff_symbols(part)
            if layouts[path][1]:
                code.add(path)
        return found, layouts, code
    by_name = {os.path.normcase(os.path.abspath(path)): path for path in paths}
    # A unit that only declares (a header check of templates and inline functions) defines no symbol, and nm then
    # prints nothing; an object holding code always defines one.
    text = _run([tools[reader], '-A', '-C', '--defined-only'] + paths, empty_ok=True)
    if not text.strip() and any(elf_has_code(path) for path in paths):
        raise GuardError(f'{Path(tools[reader]).name} printed nothing for {paths[-1]}')
    for line in text.splitlines():
        match = NM_LINE.match(line)
        if not match:
            continue
        path = by_name.get(os.path.normcase(os.path.abspath(match.group(1))))
        if path is None:
            raise GuardError(f'{tools[reader]} named a file it was not given: {match.group(1)}')
        kind = match.group(3)
        found[path].append((match.group(4), kind.isupper() or kind in 'uvw'))
        if kind in 'TtWw':
            code.add(path)
    return found, layouts, code


def unqualified(name):
    """The identifier a (decorated or demangled) data symbol names."""
    if name.startswith('?'):
        match = re.match(r'^\?([A-Za-z_]\w*)@', name)
        return match.group(1) if match else ''
    match = re.search(r'(?:^|::)([A-Za-z_]\w*)$', name)
    return match.group(1) if match else ''


def judge_kernel_symbols(namespace, defined, allowed):
    """Failures of a kernel object's symbols, and the number of tables it defines."""
    own = namespace_pattern(namespace)
    failures, tables = [], 0
    for name, external in defined:
        if not external or own.search(name):
            continue
        base = unqualified(name)
        if TABLE_NAME.match(base) and base.endswith(SUFFIX[namespace]):
            tables += 1
            continue
        if not any(pattern.search(name) for pattern in allowed):
            failures.append(f'defines {name} outside namespace {namespace}')
    return failures, tables


def judge_general_symbols(defined):
    failures = []
    for name, _external in defined:
        if name.startswith('??_C@'):
            continue
        if LEVEL_NS.search(name):
            failures.append(f'defines the per-level symbol {name}')
        elif TABLE_NAME.match(unqualified(name)):
            failures.append(f'defines the level table {name}')
    return failures


def code_failures(prefix, hits):
    """One failure per function: its above-floor categories and count, with its first such instruction."""
    grouped = {}
    for function, address, kind, line in hits:
        entry = grouped.setdefault(function, [0, set(), address, line])
        entry[0] += 1
        entry[1].add(kind)
    return [f'{prefix}{"/".join(sorted(kinds))} x{count} in {function or "?"}, first at {address:#x}: {line}'
            for function, (count, kinds, address, line) in grouped.items()]


def object_library(path):
    """The object library (CMake's <target>.dir) a kernel object belongs to: one family at one level."""
    match = re.search(r'[^\\/]+\.dir(?=[\\/])', path)
    return match.group(0) if match else str(Path(path).parent)


def judge_families(families, arch):
    """Failures of each family level ({(object library, level): [tables, encodings]}): it defines its table, and an
    avx2 (avx512) unit holds VEX (EVEX) code, so a level whose flags stopped reaching its sources fails. One source of
    a family may hold no vector code (unoptimized FFT codelets run on scalars)."""
    failures = []
    for (library, level), (tables, kinds) in sorted(families.items()):
        wanted = LEVEL_PRESENT.get(level.split('_')[0]) if arch == 'x64' else None
        if tables == 0:
            failures.append(f'{library}: the {level} unit defines no level table')
        if wanted and wanted not in kinds:
            failures.append(f'{library}: the {level} unit holds no {wanted} instruction: its flags did not reach it')
    return failures


def judge_kernel_code(level, hits, coff, reviewed=None):
    """Failures of a kernel object's instructions against its level; `reviewed(hit)` holds the reviewed runtime
    dispatch (the compiler's own, which tests __isa_available)."""
    base = level.split('_')[0]
    above = [hit for hit in hits if not (hit[2] in LEVEL_ENCODINGS[base] or hit[2] in LEVEL_LEGACY[base]
                                          or coff and hit[2] in WINDOWS_FLOOR or reviewed and reviewed(hit))]
    return code_failures(f'above level {level}: ', above)


class Exceptions:
    """The reviewed above-floor code (scripts/isa-isolation.json)."""

    def __init__(self, path=EXCEPTIONS):
        data = json.loads(Path(path).read_text(encoding='utf-8'))
        if data.get('schema') != SCHEMA:
            raise GuardError(f'{path}: schema is not {SCHEMA}')
        self.kernel_symbols = []
        for entry in data.get('kernel_symbols', []):
            if not entry.get('pattern') or not entry.get('reason'):
                raise GuardError(f'{path}: a kernel_symbols entry needs pattern and reason')
            self.kernel_symbols.append(re.compile(entry['pattern']))
        self.floor = []
        for entry in data.get('floor', []):
            if not entry.get('allows') or not entry.get('guard') or not entry.get('reason'):
                raise GuardError(f'{path}: a floor entry needs allows, guard and reason')
            if not (entry.get('symbol') or entry.get('owner') or entry.get('guard_symbol')):
                raise GuardError(f'{path}: a floor entry needs a symbol, owner or guard_symbol scope')
            self.floor.append({
                'allows': frozenset(entry['allows']),
                'symbol': re.compile(entry['symbol']) if entry.get('symbol') else None,
                'owner': re.compile(entry['owner']) if entry.get('owner') else None,
                'guard_symbol': entry.get('guard_symbol')})

    def allows(self, kind, function, owner, guarded):
        """Whether a reviewed entry covers `kind` in `function` of `owner`; `guarded` holds the functions that read a
        guard symbol. `legacy` in an entry's allows stands for every legacy-encoded instruction above the floor."""
        for entry in self.floor:
            if kind not in entry['allows'] and not ('legacy' in entry['allows'] and kind in X64_LEGACY):
                continue
            if entry['symbol'] and not entry['symbol'].search(function):
                continue
            if entry['owner'] and not entry['owner'].search(owner):
                continue
            if entry['guard_symbol'] and function not in guarded:
                continue
            return True
        return False


def read_manifest(path):
    kernels, generals, images = {}, {}, {}
    text = Path(path).read_text(encoding='utf-8')
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split('\t')
        if fields[0] == 'kernel' and len(fields) == 5:
            level = fields[1] + (f'_{fields[2]}' if fields[2] else '')
            if level not in SUFFIX:
                raise GuardError(f'{path}:{number}: unknown level {level}')
            kernels[os.path.normpath(fields[4])] = (level, fields[3])
        elif fields[0] == 'general' and len(fields) == 4:
            generals.setdefault(os.path.normpath(fields[3]), (fields[1], fields[2]))
        elif fields[0] == 'image' and len(fields) == 3:
            images[os.path.normpath(fields[2])] = fields[1]
        else:
            raise GuardError(f'{path}:{number}: malformed line')
    for kernel in kernels:
        generals.pop(kernel, None)
    if not kernels or not generals:
        raise GuardError(f'{path}: the manifest lists no kernel or no general object')
    return kernels, generals, images


def _stem(name):
    return Path(name).stem.lower() if name else ''


class Owners:
    """Which code a map's object column names: a kernel object, general code, or a library outside the build."""

    def __init__(self, kernels, generals):
        self.kernel_members = {Path(path).name.lower() for path in kernels}
        self.general_members = {Path(path).name.lower() for path in generals}
        clash = sorted(self.kernel_members & self.general_members)
        if clash:
            raise GuardError(f'a kernel object shares its name with a general object: {clash[0]} (rename the kernel '
                             f'source: a map names objects by file name)')
        self.archives = ({_stem(archive) for _, archive in kernels.values()}
                         | {_stem(artifact) for _, artifact in generals.values()}) - {''}

    def classify(self, column, image):
        """(kind, owner) for a map's object column: kind is kernel, general or library."""
        if column.startswith('<'):
            return 'library', column
        prefix = f'{Path(image).name}.lto.'
        lto = column.lower().startswith(prefix.lower())
        rest = column[len(prefix):] if lto else column
        archive, member = None, rest
        if ':' in rest:
            archive, member = rest.split(':', 1)
        elif lto and '.lib' in rest.lower():
            cut = rest.lower().rfind('.lib')
            archive, member = rest[:cut], rest[cut + 4:].strip('()')
        if archive is not None and _stem(archive + '.lib') not in self.archives:
            return 'library', column
        kind = 'kernel' if member.lower() in self.kernel_members else 'general'
        return kind, column


def read_map(path):
    """The image base, its code symbols [(address, name, column)] sorted, and every symbol."""
    text = Path(path).read_text(encoding='utf-8', errors='replace')
    base = MAP_BASE.search(text)
    if not base:
        raise GuardError(f'{path}: no preferred load address')
    code_segments = set()
    entries = []
    for line in text.splitlines():
        segment = MAP_SEGMENT.match(line)
        if segment:
            if segment.group(5) == 'CODE':
                code_segments.add(segment.group(1))
            continue
        symbol = MAP_SYMBOL.match(line)
        if symbol:
            entries.append((symbol.group(1), int(symbol.group(4), 16), symbol.group(3), symbol.group(5)))
    if not code_segments or not entries:
        raise GuardError(f'{path}: no code segment or no symbol')
    code = sorted((address, name, column) for segment, address, name, column in entries if segment in code_segments)
    return int(base.group(1), 16), code, entries


def pe_reader(path):
    """A function reading `size` bytes at a virtual address of a PE image."""
    data = Path(path).read_bytes()
    offset, = struct.unpack_from('<I', data, 0x3C)
    if data[offset:offset + 4] != b'PE\0\0':
        raise GuardError(f'{path}: not a PE image')
    count, optional = struct.unpack_from('<H', data, offset + 6)[0], struct.unpack_from('<H', data, offset + 20)[0]
    magic, = struct.unpack_from('<H', data, offset + 24)
    base, = struct.unpack_from('<Q', data, offset + 48) if magic == 0x20B else (struct.unpack_from('<I', data,
                                                                                                    offset + 52)[0],)
    sections = []
    table = offset + 24 + optional
    for index in range(count):
        size, rva, raw_size, raw = struct.unpack_from('<IIII', data, table + index * 40 + 8)
        sections.append((rva, max(size, raw_size), raw))

    def read(address, size):
        rva = address - base
        for start, length, raw in sections:
            if start <= rva < start + length:
                return data[raw + rva - start:raw + rva - start + size]
        return b''
    return read


def jump_tables(base, starts, code, read):
    """[start, end) ranges of MSVC jump tables in .text: a $LN code symbol (link.exe lists a table's label among the
    static symbols, never a case label) whose first entry is the image-relative address of a case in the function
    before it. A table, and any table after it, runs to the next function symbol."""
    ranges = []
    for address, name, _column in code:
        if not re.match(r'^\$LN\d+$', name):
            continue
        index = bisect.bisect_right(starts, address) - 1
        if index < 0 or index + 1 >= len(starts):
            continue
        entry = read(address, 4)
        if len(entry) == 4 and starts[index] <= base + struct.unpack('<I', entry)[0] < address:
            ranges.append((address, starts[index + 1]))
    return sorted(ranges)


def judge_map_image(tools, image, map_path, owners, exceptions):
    """Failures of one MSVC or clang-cl image: per-level symbols from general objects and above-floor code."""
    base, code, entries = read_map(map_path)
    failures = []
    for _segment, _address, name, column in entries:
        if name.startswith(('$', '??_C@')) or not LEVEL_NS.search(name):
            continue
        kind, owner = owners.classify(column, image)
        if kind == 'general':
            failures.append(f'{image}: per-level symbol {name} comes from general object {owner}')
    # One function per address; /OPT:ICF folds identical bodies under several names, which then share one owner.
    aliases = {}
    for address, name, column in code:
        if not name.startswith('$'):
            aliases.setdefault(address, []).append((name, column))
    starts = sorted(aliases)
    tables = jump_tables(base, starts, code, pe_reader(image))
    table_starts = [start for start, _ in tables]
    isa = next((address for _segment, address, name, _ in entries if name == '__isa_available'), None)

    def function_at(address):
        index = bisect.bisect_right(starts, address) - 1
        return aliases[starts[index]] if index >= 0 else None

    # Functions that read __isa_available, by name with the PDB beside the image and by address without it.
    guarded, hits, decoded = set(), [], 0
    reference = re.compile(r'__isa_available' + (rf'|\[0*{isa:X}h\]' if isa is not None else ''))
    for text in _stream([tools['dumpbin'], '/nologo', '/disasm', image], _LABEL_COFF):
        for match in reference.finditer(text):
            line_start = text.rfind('\n', 0, match.start()) + 1
            address = re.match(r'^  ([0-9A-F]{8,16}):', text[line_start:match.start()])
            names = function_at(int(address.group(1), 16)) if address else None
            guarded.update(name for name, _ in names or [])
        hits += scan_x64(text, True)
        decoded += len(INSTRUCTION[('x64', True)].findall(text))
    if not decoded:
        failures.append(f'{image}: dumpbin decoded no instruction')
    above = []
    for _label, address, kind, line in hits:
        table = bisect.bisect_right(table_starts, address) - 1
        if table >= 0 and address < tables[table][1]:
            continue
        names = function_at(address)
        if names is None:
            failures.append(f'{image}: {kind} before the first symbol at {address:#x}: {line}')
            continue
        if any(LEVEL_NS.search(name) for name, _ in names):
            continue
        name, column = names[0]
        kind_of, owner = owners.classify(column, image)
        if kind_of == 'kernel' or exceptions.allows(kind, name, owner, guarded):
            continue
        above.append((f'{name} ({owner})', address, kind, line))
    return failures + code_failures(f'{image}: ', above)


def judge_elf_image(tools, image, arch, exceptions):
    """Failures of one ELF image: above-floor code outside the level namespaces."""
    command = [tools['objdump'], '-d', '-w', '-C'] + (['-M', 'intel'] if arch == 'x64' else []) + [image]
    hits, decoded = [], 0
    for text in _stream(command, _LABEL_ELF):
        hits += scan_x64(text, False) if arch == 'x64' else scan_arm64(text, False)
        decoded += len(INSTRUCTION[(arch, False)].findall(text))
    above = [hit for hit in hits if not (LEVEL_NS.search(hit[0]) or exceptions.allows(hit[2], hit[0], '', set()))]
    return ([f'{image}: objdump decoded no instruction'] if not decoded else []) + code_failures(f'{image}: ', above)


def _batches(items):
    """Objects grouped for one decoder call each: at most BATCH of them and BATCH_BYTES together, or one larger."""
    batches, batch, size = [], [], 0
    for path in sorted(items):
        length = os.path.getsize(path)
        if batch and (len(batch) == BATCH or size + length > BATCH_BYTES):
            batches.append(batch)
            batch, size = [], 0
        batch.append(path)
        size += length
    return batches + ([batch] if batch else [])


def _object_work(args):
    """One batch of machine-code objects: symbols and disassembly."""
    tools, paths, arch, coff = args
    defined, layouts, code = symbols(tools, paths, coff)
    return defined, code, disassemble(tools, paths, arch, coff, layouts)


def _il_work(args):
    tools, paths, reader = args
    return symbols(tools, paths, False, reader)[0]


def resolve_tools(args, coff):
    """dumpbin on Windows; GNU binutils' objdump and nm elsewhere (their output shapes are the ones this guard reads,
    so another decoder fails), gcc-nm for GCC's IL objects and llvm-nm for LLVM's."""
    tools = {}
    if coff:
        tools['dumpbin'] = shutil.which('dumpbin') or _vswhere_dumpbin()
        if not tools['dumpbin']:
            raise GuardError('dumpbin not found (PATH or the Visual Studio installation)')
        return tools
    for name in ('objdump', 'nm'):
        tools[name] = shutil.which(name)
        if not tools[name]:
            raise GuardError(f'{name} not found')
        version = _run([tools[name], '--version']).splitlines()[0]
        if 'GNU' not in version:
            raise GuardError(f'{tools[name]} is not GNU binutils: {version}')
    tools['gcc-nm'] = args.gcc_nm or shutil.which('gcc-nm')
    tools['llvm-nm'] = shutil.which('llvm-nm')
    return tools


def _vswhere_dumpbin():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                   'Microsoft Visual Studio/Installer/vswhere.exe')
    if not vswhere.exists():
        return None
    host = 'Hostarm64/arm64' if os.environ.get('PROCESSOR_ARCHITECTURE', '').upper() == 'ARM64' else 'Hostx64/x64'
    for pattern in (f'VC/Tools/MSVC/*/bin/{host}/dumpbin.exe', 'VC/Tools/MSVC/*/bin/*/*/dumpbin.exe'):
        done = subprocess.run([str(vswhere), '-latest', '-products', '*', '-find', pattern], capture_output=True,
                              text=True, check=False)
        found = sorted(line for line in done.stdout.splitlines() if line.strip())
        if found:
            return found[-1]
    return None


def run(args):
    exceptions = Exceptions(args.exceptions)
    kernels, generals, images = read_manifest(args.manifest)
    failures, unbuilt = [], []
    present = {}
    # An object is judged when it exists; a built archive or program whose object is missing is a stale tree.
    for path, artifact in [(p, a) for p, (_t, a) in generals.items()] + [(p, a) for p, (_l, a) in kernels.items()]:
        if os.path.exists(path):
            present[path] = True
        elif artifact and os.path.exists(artifact):
            failures.append(f'{path}: missing, yet {artifact} is built')
        else:
            unbuilt.append(path)
    formats = {path: object_format(path) for path in present if not path.lower().endswith('.res')}
    coff = args.os == 'windows'
    tools = resolve_tools(args, coff)
    machine = [path for path, fmt in formats.items() if fmt in ('coff', 'elf')]
    il = [path for path, fmt in formats.items() if fmt not in ('coff', 'elf')]
    for path in il:
        if path in kernels:
            failures.append(f'{path}: a kernel unit holds link-time IL (ADR-0134 §5: machine code on every lane)')
    jobs = max(1, args.jobs or os.cpu_count() or 1)
    counts = {'kernel': 0, 'general': 0, 'il': len(il), 'images': 0}
    # Per family level (one object library, all its sources): its tables and the encodings its code holds.
    families = {}
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        work = [(tools, batch, args.arch, coff) for batch in _batches(machine)]
        for defined, code, disassembly in pool.map(_object_work, work):
            for path, names in defined.items():
                hits, guarded, decoded = disassembly[path]
                if path in code and not decoded:
                    failures.append(f'{path}: defines functions, yet the decoder printed no instruction it reads')
                owner = f'{generals[path][0] if path in generals else Path(kernels[path][1]).stem}/{Path(path).name}'

                def reviewed(hit, owner=owner, guarded=guarded):
                    return exceptions.allows(hit[2], hit[0], owner, guarded)
                if path in kernels:
                    level = kernels[path][0]
                    counts['kernel'] += 1
                    found, tables = judge_kernel_symbols(level, names, exceptions.kernel_symbols)
                    found += (judge_kernel_code(level, hits, coff, reviewed) if args.arch == 'x64'
                              else code_failures('', hits))
                    family = families.setdefault((object_library(path), level), [0, set()])
                    family[0] += tables
                    family[1].update(kind for _, _, kind, _ in hits)
                    failures += [f'{path}: {failure}' for failure in found]
                    continue
                counts['general'] += 1
                found = judge_general_symbols(names)
                found += code_failures('', [hit for hit in hits if not reviewed(hit)])
                failures += [f'{path}: {failure}' for failure in found]
        failures += judge_families(families, args.arch)
        gnu = [path for path in il if formats[path] == 'gnu-lto']
        bitcode_elf = [path for path in il if formats[path] == 'bitcode' and not coff]
        for paths, reader in ((gnu, 'gcc-nm'), (bitcode_elf, 'llvm-nm')):
            if paths and not tools.get(reader):
                raise GuardError(f'{reader} not found for {len(paths)} IL objects')
            for defined in pool.map(_il_work, [(tools, batch, reader) for batch in _batches(paths)]):
                for path, names in defined.items():
                    failures += [f'{path}: {failure}' for failure in judge_general_symbols(names)]
        if il:
            linked = sorted(path for path in images if os.path.exists(path))
            if not linked:
                failures.append(f'{len(il)} objects hold link-time IL and no image was built to judge their code')
            owners = Owners(kernels, generals) if coff else None
            futures = []
            for image in linked:
                if coff:
                    map_path = str(Path(image).with_suffix('.map'))
                    if not os.path.exists(map_path):
                        failures.append(f'{image}: no linker map (the IPO links take /MAP)')
                        continue
                    futures.append(pool.submit(judge_map_image, tools, image, map_path, owners, exceptions))
                else:
                    futures.append(pool.submit(judge_elf_image, tools, image, args.arch, exceptions))
            for future in futures:
                counts['images'] += 1
                failures += future.result()
    print(f'[isa-isolation] {counts["kernel"]} kernel objects, {counts["general"]} general objects, {counts["il"]} '
          f'IL objects, {counts["images"]} images judged; {len(unbuilt)} objects of unbuilt targets')
    if counts['kernel'] == 0 or counts['general'] + counts['il'] == 0:
        failures.append('nothing was judged: build the tree first')
    for failure in failures[:args.limit]:
        print(f'[isa-isolation] FAIL {failure}')
    if len(failures) > args.limit:
        print(f'[isa-isolation] ... {len(failures) - args.limit} more failures')
    if failures:
        print(f'[isa-isolation] {len(failures)} failures (ADR-0134 §6)')
        return 1
    print('[isa-isolation] PASS')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--arch', choices=('x64', 'arm64'), required=True)
    parser.add_argument('--os', choices=('windows', 'linux'), required=True)
    parser.add_argument('--exceptions', default=str(EXCEPTIONS))
    parser.add_argument('--gcc-nm', dest='gcc_nm', help="GCC's nm wrapper with the LTO plugin (default: gcc-nm)")
    parser.add_argument('--jobs', type=int)
    parser.add_argument('--limit', type=int, default=60)
    args = parser.parse_args(argv)
    try:
        return run(args)
    except GuardError as error:
        print(f'[isa-isolation] FAIL {error}')
        return 2


if __name__ == '__main__':
    sys.exit(main())
