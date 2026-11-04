#!/usr/bin/env python3
"""The provider's symbol prefix (NET.2 decision P5, docs/design/network-foundation.md#provider).

Every symbol AWS-LC's libcrypto and libssl define for the linker carries the prefix the configure names
(BORINGSSL_PREFIX), so a program that links Cerid's package beside OpenSSL, BoringSSL or another AWS-LC binds each
call to its own library. AWS-LC renames its symbols through three headers that #define each name to its prefixed form
(C, its assembly and NASM; a name in CALL_FORM as a function-like macro); this script writes them from the committed
list cmake/aws-lc-symbols.txt in the format of AWS-LC's util/make_prefix_headers.go (Copyright (c) 2018, Google Inc.,
ISC), and reads object files itself in place of its util/read_symbols.go, so neither Go nor a toolchain's nm is
needed.

  headers  --symbols LIST --prefix P --out DIR   write DIR/openssl/boringssl_prefix_symbols{.h,_asm.h,_nasm.inc}, each
           only when its text changed (the configure runs it; AWS-LC copies them into its build)
  collect  --symbols LIST [--prefix P] ARCHIVE...  add to LIST every name the archives define that the linker would
           not merge (prefixed names are read without the prefix), for a new AWS-LC pin or a lane whose check failed
  check    --symbols LIST --prefix P ARCHIVE...    the crd-check-provider-symbols CTest: every C symbol the archives
           define carries the prefix and every C++ one lies in its namespace (bssl::P), unless the linker merges it

The linker merges a COFF COMDAT definition of selection any, same size, exact match or largest (the C runtime's inline
functions and the compiler's constants), and a weak or COMDAT-group C++ definition (inline functions and templates);
those never collide, so they are neither listed nor judged. A weak C definition is judged: another library's would
take its place silently. Archives are ar files (GNU, BSD and Microsoft's) of ELF64, COFF (bigobj too) and Mach-O 64
objects; link-time IL is refused, because its symbols cannot be read here.
"""
from pathlib import Path
import argparse
import re
import struct
import sys

HEADER = '# AWS-LC symbols renamed by the provider prefix (NET.2 decision P5): scripts/provider-prefix.py collect'
# Names AWS-LC defines to merge with another library's on purpose (util/read_symbols.go): its weak sdallocx binds to
# jemalloc's when that is linked.
INTENDED_MERGES = frozenset({'sdallocx'})
# Toolchain symbols that a compiler emits into the provider's objects (util/read_symbols.go): x86 PIC thunks and
# DWARF labels.
TOOLCHAIN_PREFIXES = ('__x86.get_pc_thunk.', 'DW.')
# Symbols a sanitizer derives from a global's name (AddressSanitizer's ODR indicator): judged by that name, whose
# prefix they carry.
DERIVED_PREFIXES = ('__odr_asan.',)
# Functions renamed only where they are called (a function-like macro): the engine uses the same names for its own
# identifiers in units that include AWS-LC's headers (AlgorithmId::X25519). A use of one without a call would keep the
# unprefixed name and fail the link, never bind silently.
CALL_FORM = frozenset({'X25519'})
IDENTIFIER = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')
BIGOBJ_CLASS = bytes.fromhex('c7a1bad1eebaa94baf20faf66aa4dcb8')
# COFF COMDAT selections whose duplicates the linker resolves silently (any, same size, exact match, largest).
MERGED_SELECTIONS = frozenset({2, 3, 4, 6})
ARCHIVE_INDEXES = frozenset({'/', '/SYM64/', '//', '__.SYMDEF', '__.SYMDEF SORTED', '__.SYMDEF_64',
                             '__.SYMDEF_64 SORTED', '/<ECSYMBOLS>/', '/<HYBRIDMAP>/'})


class SymbolError(Exception):
    pass


def archive_members(data, label):
    """[(member name, member bytes)] of an ar archive's objects, its indexes and long-name table left out."""
    if not data.startswith(b'!<arch>\n'):
        raise SymbolError(f'{label}: not an ar archive')
    members, names, offset = [], b'', 8
    while offset < len(data):
        if offset + 60 > len(data) or data[offset + 58:offset + 60] != b'`\n':
            raise SymbolError(f'{label}: malformed member header at offset {offset}')
        raw = data[offset:offset + 16].decode('latin-1').rstrip(' ')
        try:
            size = int(data[offset + 48:offset + 58].decode('ascii').strip())
        except ValueError as error:
            raise SymbolError(f'{label}: malformed member size at offset {offset}') from error
        start = offset + 60
        body = data[start:start + size]
        if len(body) != size:
            raise SymbolError(f'{label}: member at offset {offset} runs past the end')
        offset = start + size + (size & 1)
        if raw == '//':
            names = body
            continue
        if raw in ARCHIVE_INDEXES:
            continue
        if raw.startswith('#1/'):
            length = int(raw[3:])
            name, body = body[:length].rstrip(b'\0').decode('utf-8', 'replace'), body[length:]
        elif re.match(r'^/\d+$', raw):
            at = int(raw[1:])
            end = min(i for i in (names.find(b'\n', at), names.find(b'\0', at), len(names)) if i >= 0)
            name = names[at:end].decode('utf-8', 'replace').rstrip('/')
        else:
            name = raw.rstrip('/')
        if name in ARCHIVE_INDEXES:  # a BSD archive's index under a #1/ name, as macOS's ar writes it
            continue
        members.append((name, body))
    return members


def _strings_at(table, offset):
    end = table.find(b'\0', offset)
    return table[offset:end if end >= 0 else len(table)].decode('utf-8', 'replace')


def elf_symbols(data, label):
    """[(name, kind)] of an ELF64 little-endian object's defined non-local symbols: kind strong, weak or merged (a
    weak C++ definition)."""
    if data[4] != 2 or data[5] != 1:
        raise SymbolError(f'{label}: only little-endian ELF64 objects are read')
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3A)
    if shoff and shnum == 0:
        shnum = struct.unpack_from('<Q', data, shoff + 32)[0]
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    found = []
    for _name, kind, _flags, _addr, offset, size, link, _info, _align, entsize in sections:
        if kind != 2:  # SHT_SYMTAB
            continue
        strings = data[sections[link][4]:sections[link][4] + sections[link][5]]
        for at in range(offset + entsize, offset + size, entsize):  # entry 0 is the null symbol
            name_offset, info, _other, shndx = struct.unpack_from('<IBBH', data, at)
            binding = info >> 4
            if binding == 0 or shndx == 0:  # STB_LOCAL, SHN_UNDEF
                continue
            name = _strings_at(strings, name_offset)
            if binding in (2, 10):  # STB_WEAK, STB_GNU_UNIQUE
                found.append((name, 'merged' if name.startswith('_Z') else 'weak'))
            else:
                found.append((name, 'strong'))
    return found


def coff_symbols(data, label):
    """[(name, kind)] of a COFF object's defined external symbols: kind strong, or merged when its COMDAT section's
    selection lets the linker keep any one definition."""
    if data[:4] == b'\x00\x00\xff\xff':
        if struct.unpack_from('<H', data, 4)[0] == 0:  # an import object names a DLL's export and defines nothing
            return []
        if data[12:28] != BIGOBJ_CLASS:
            raise SymbolError(f'{label}: holds link-time IL (an MSVC /GL object), whose symbols cannot be read')
        nsections, symtab, nsymbols = struct.unpack_from('<III', data, 44)
        sections_at, entry, number_format = 56, 20, '<i'
    else:
        nsections, = struct.unpack_from('<H', data, 2)
        symtab, nsymbols, optional = struct.unpack_from('<IIH', data, 8)
        sections_at, entry, number_format = 20 + optional, 18, '<h'
    comdat = {index + 1 for index in range(nsections)
              if struct.unpack_from('<I', data, sections_at + index * 40 + 36)[0] & 0x1000}
    strings = data[symtab + nsymbols * entry:]
    selection, defined, index = {}, [], 0
    while index < nsymbols:
        at = symtab + index * entry
        raw = data[at:at + 8]
        name = _strings_at(strings, struct.unpack_from('<I', raw, 4)[0]) if raw[:4] == b'\0\0\0\0' else \
            raw.rstrip(b'\0').decode('utf-8', 'replace')
        section, = struct.unpack_from(number_format, data, at + 12)
        storage, aux = data[at + entry - 2], data[at + entry - 1]
        if storage == 3 and aux and section in comdat and section not in selection:  # the section's definition
            selection[section] = data[at + entry + 14]
        elif storage == 2 and section != 0:  # IMAGE_SYM_CLASS_EXTERNAL, defined
            defined.append((name, section))
        index += 1 + aux
    return [(name, 'merged' if selection.get(section) in MERGED_SELECTIONS else 'strong')
            for name, section in defined]


def macho_symbols(data, label):
    """[(name, kind)] of a Mach-O 64 object's defined external symbols, the leading underscore removed: kind strong,
    weak, or merged (a weak C++ definition)."""
    ncmds, = struct.unpack_from('<I', data, 16)
    at, found = 32, []
    for _ in range(ncmds):
        command, size = struct.unpack_from('<II', data, at)
        if command == 0x2:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', data, at + 8)
            strings = data[stroff:stroff + strsize]
            for entry in range(symoff, symoff + nsyms * 16, 16):
                strx, kind, _sect, desc = struct.unpack_from('<IBBH', data, entry)
                if kind & 0xE0 or not kind & 0x01 or kind & 0x0E == 0:  # stab, not external, undefined
                    continue
                name = _strings_at(strings, strx)
                if not name.startswith('_'):
                    raise SymbolError(f'{label}: symbol without the leading underscore: {name}')
                name = name[1:]
                weak = bool(desc & 0x0080)  # N_WEAK_DEF
                found.append((name, ('merged' if name.startswith('_Z') else 'weak') if weak else 'strong'))
        at += size
    return found


def object_symbols(data, label):
    """[(name, kind)] of one object file's definitions the linker sees outside it."""
    if data[:4] == b'\x7fELF':
        return elf_symbols(data, label)
    if data[:4] == b'\xcf\xfa\xed\xfe':
        return macho_symbols(data, label)
    if data[:4] in (b'BC\xc0\xde', b'\xde\xc0\x17\x0b'):
        raise SymbolError(f'{label}: holds LLVM bitcode (link-time IL), whose symbols cannot be read')
    if data[:4] == b'\x00\x00\xff\xff' or (len(data) >= 20 and data[:2] in (b'\x64\x86', b'\x64\xaa', b'\x4c\x01')):
        return coff_symbols(data, label)
    raise SymbolError(f'{label}: unknown object format')


def archive_symbols(path):
    """[(member, name, kind)] of every definition in the archive at `path`, a sanitizer's derived name read as the
    global's own."""
    data = Path(path).read_bytes()
    found = []
    for member, body in archive_members(data, str(path)):
        if not body:
            continue
        label = f'{path}({member})'
        try:
            for name, kind in object_symbols(body, label):
                for derived in DERIVED_PREFIXES:
                    name = name.removeprefix(derived)
                found.append((member, name, kind))
        except struct.error as error:
            raise SymbolError(f'{label}: truncated object ({error})') from error
    return found


def is_cxx(name):
    return name.startswith(('_Z', '?'))


def judged(name, kind):
    """Whether a definition must carry the prefix: the linker would not merge it, and it is no toolchain symbol."""
    return kind != 'merged' and name not in INTENDED_MERGES and not name.startswith(TOOLCHAIN_PREFIXES)


def cxx_scopes(name):
    """The outermost two names of a mangled C++ symbol's qualified name (Itanium or Microsoft mangling): its
    namespace or class, then the next one; [] when the form is not one read here."""
    if name.startswith('?'):
        body = name[1:]
        if body.startswith('?$'):  # a template's name: its arguments come first
            return []
        if body.startswith('?'):  # a special member: ??0 constructor, ??1 destructor, ??_G deleting destructor
            body = body[3:] if body.startswith('?_') else body[2:]
        qualified = body.split('@@', 1)[0]
        if '$' in qualified or '?' in qualified:
            return []
        return qualified.split('@')[::-1][:2]
    rest = name[2:]
    if rest.startswith(('TV', 'TI', 'TS', 'GV')):  # vtable, typeinfo, its name, a guard variable
        rest = rest[2:]
    rest = rest.removeprefix('Z')  # a local entity: its function's name comes first
    nested = rest.startswith('N')
    if nested:
        rest = rest[1:].lstrip('rVKRO')
    if rest.startswith('St'):
        return ['std']
    scopes = []
    while len(scopes) < (2 if nested else 1):
        match = re.match(r'\d+', rest)
        if not match:
            break
        length = int(match.group())
        scopes.append(rest[match.end():match.end() + length])
        rest = rest[match.end() + length:]
    return scopes


def cxx_prefixed(name, prefix):
    """Whether a C++ definition carries the prefix: it lies in bssl's inline namespace `prefix`, or its outermost name
    (a global struct whose members it defines, a global function) was renamed by the prefix headers."""
    scopes = cxx_scopes(name)
    if not scopes:
        return f'@{prefix}@bssl@@' in name or f'4bssl{len(prefix)}{prefix}' in name
    return scopes[0].startswith(f'{prefix}_') or scopes[:2] == ['bssl', prefix]


def read_list(path):
    """The names of a symbol list; a malformed list is refused."""
    names = []
    for number, line in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        text = line.split('#', 1)[0].strip()
        if not text:
            continue
        if not IDENTIFIER.match(text):
            raise SymbolError(f'{path}:{number}: not a C identifier: {text}')
        names.append(text)
    if names != sorted(set(names)):
        raise SymbolError(f'{path}: the names are not sorted and unique (rewrite it with collect)')
    return names


def write_list(path, names):
    Path(path).write_text('\n'.join([HEADER] + sorted(names)) + '\n', encoding='utf-8', newline='\n')


def render(names, prefix):
    """{file name: text} of the three prefix headers, in make_prefix_headers.go's format."""
    note = 'Generated by scripts/provider-prefix.py from cmake/aws-lc-symbols.txt (NET.2 decision P5), in the format ' \
           'of AWS-LC\'s util/make_prefix_headers.go (Copyright (c) 2018, Google Inc., ISC).'
    c_lines = [f'// {note}', '', '#ifndef BORINGSSL_PREFIX_SYMBOLS_H', '#define BORINGSSL_PREFIX_SYMBOLS_H', '',
               '#ifndef BORINGSSL_PREFIX', f'#define BORINGSSL_PREFIX {prefix}', '#endif // BORINGSSL_PREFIX', '',
               '#define BORINGSSL_ADD_PREFIX(a, b) BORINGSSL_ADD_PREFIX_INNER(a, b)',
               '#define BORINGSSL_ADD_PREFIX_INNER(a, b) a ## _ ## b', '']
    c_lines += [f'#define {name}(...) BORINGSSL_ADD_PREFIX(BORINGSSL_PREFIX, {name})(__VA_ARGS__)'
                if name in CALL_FORM else f'#define {name} BORINGSSL_ADD_PREFIX(BORINGSSL_PREFIX, {name})'
                for name in names]
    c_lines += ['', '#endif // BORINGSSL_PREFIX_SYMBOLS_H']
    asm_lines = [f'// {note}', '', '#if !defined(__APPLE__)', '#include <openssl/boringssl_prefix_symbols.h>',
                 '#else', '#ifndef BORINGSSL_PREFIX_SYMBOLS_ASM_H', '#define BORINGSSL_PREFIX_SYMBOLS_ASM_H', '',
                 '#ifndef BORINGSSL_PREFIX', f'#define BORINGSSL_PREFIX {prefix}', '#endif // BORINGSSL_PREFIX', '',
                 '// Apple\'s assembly names C symbols with a leading underscore.',
                 '#define BORINGSSL_ADD_PREFIX_MAC_ASM(a, b) BORINGSSL_ADD_PREFIX_INNER_MAC_ASM(a, b)',
                 '#define BORINGSSL_ADD_PREFIX_INNER_MAC_ASM(a, b) _ ## a ## _ ## b', '']
    asm_lines += [f'#define _{name} BORINGSSL_ADD_PREFIX_MAC_ASM(BORINGSSL_PREFIX, {name})' for name in names]
    asm_lines += ['', '#endif // BORINGSSL_PREFIX_SYMBOLS_ASM_H', '#endif // !defined(__APPLE__)']
    nasm_lines = [f'; {note}', '', '%ifndef BORINGSSL_PREFIX_SYMBOLS_NASM_INC',
                  '%define BORINGSSL_PREFIX_SYMBOLS_NASM_INC', '', '%ifndef BORINGSSL_PREFIX',
                  f'%define BORINGSSL_PREFIX {prefix}', '%endif ; BORINGSSL_PREFIX', '',
                  '; 32-bit Windows adds underscores to C functions, while 64-bit Windows does not.',
                  '%ifidn __OUTPUT_FORMAT__, win32']
    nasm_lines += [f'%xdefine _{name} _ %+ BORINGSSL_PREFIX %+ _{name}' for name in names]
    nasm_lines += ['%else']
    nasm_lines += [f'%xdefine {name} BORINGSSL_PREFIX %+ _{name}' for name in names]
    nasm_lines += ['%endif ; __OUTPUT_FORMAT__', '%endif ; BORINGSSL_PREFIX_SYMBOLS_NASM_INC']
    return {'boringssl_prefix_symbols.h': '\n'.join(c_lines) + '\n',
            'boringssl_prefix_symbols_asm.h': '\n'.join(asm_lines) + '\n',
            'boringssl_prefix_symbols_nasm.inc': '\n'.join(nasm_lines) + '\n'}


def write_headers(names, prefix, out):
    """Writes each header whose text changed; returns the names written."""
    directory = Path(out) / 'openssl'
    directory.mkdir(parents=True, exist_ok=True)
    written = []
    for file_name, text in render(names, prefix).items():
        path = directory / file_name
        data = text.encode('utf-8')
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
            written.append(file_name)
    return written


def collect(names, prefix, archives):
    """The list `names` with every judged C name the archives define added (their prefix removed)."""
    found = set(names)
    lead = f'{prefix}_' if prefix else None
    for archive in archives:
        for _member, name, kind in archive_symbols(archive):
            if not judged(name, kind):
                continue
            if is_cxx(name):
                # A C++ definition outside bssl's namespace is renamed through its outermost name.
                scopes = cxx_scopes(name)
                if not scopes or scopes[0] in ('bssl', 'std') or (prefix and cxx_prefixed(name, prefix)):
                    continue
                name = scopes[0]
            found.add(name[len(lead):] if lead and name.startswith(lead) else name)
    return sorted(found)


def check(names, prefix, archives):
    """Failures: a judged definition without the prefix (C) or outside bssl::prefix (C++); an archive without a
    prefixed definition."""
    failures = []
    listed = set(names)
    lead = f'{prefix}_'
    for archive in archives:
        prefixed = 0
        for member, name, kind in archive_symbols(archive):
            if not judged(name, kind):
                continue
            if is_cxx(name):
                if not cxx_prefixed(name, prefix):
                    failures.append(f'{archive}({member}): defines the C++ symbol {name} outside bssl::{prefix} '
                                    'and without the prefix (collect adds its outermost name)')
                continue
            if name.startswith(lead):
                prefixed += 1
            elif name in listed:
                failures.append(f'{archive}({member}): defines {name}, which the list names, without the prefix '
                                '(its headers did not reach this unit)')
            else:
                failures.append(f'{archive}({member}): defines {name} without the prefix; add it to the list '
                                f'(python scripts/provider-prefix.py collect --symbols LIST --prefix {prefix} '
                                'ARCHIVE...) and build again')
        if not prefixed:
            failures.append(f'{archive}: defines no symbol with the prefix {lead}')
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)
    headers = commands.add_parser('headers')
    headers.add_argument('--symbols', required=True)
    headers.add_argument('--prefix', required=True)
    headers.add_argument('--out', required=True)
    gather = commands.add_parser('collect')
    gather.add_argument('--symbols', required=True)
    gather.add_argument('--prefix')
    gather.add_argument('archives', nargs='+')
    judge = commands.add_parser('check')
    judge.add_argument('--symbols', required=True)
    judge.add_argument('--prefix', required=True)
    judge.add_argument('archives', nargs='+')
    args = parser.parse_args(argv)
    try:
        if args.command != 'collect' or Path(args.symbols).exists():
            names = read_list(args.symbols)
        else:
            names = []
        if getattr(args, 'prefix', None) is not None and not IDENTIFIER.match(args.prefix):
            raise SymbolError(f'the prefix is not a C identifier: {args.prefix}')
        if args.command == 'headers':
            written = write_headers(names, args.prefix, args.out)
            print(f'provider prefix {args.prefix}: {len(names)} symbols, {len(written)} header(s) written')
            return 0
        if args.command == 'collect':
            updated = collect(names, args.prefix, args.archives)
            write_list(args.symbols, updated)
            print(f'{args.symbols}: {len(updated)} symbols ({len(updated) - len(names)} added)')
            return 0
        failures = check(names, args.prefix, args.archives)
    except (OSError, SymbolError) as error:
        print(f'FAIL: {error}')
        return 1
    for failure in failures:
        print(f'FAIL: {failure}')
    if failures:
        return 1
    print(f'OK: {len(args.archives)} archive(s) define only {args.prefix}_ symbols')
    return 0


if __name__ == '__main__':
    sys.exit(main())
