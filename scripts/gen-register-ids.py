#!/usr/bin/env python3
"""Generate the wire identifiers each owning module takes from the NET-STD standards register.

cmake/standards-register.json is the one record of every Cerid and IANA codepoint (docs/design/registers.md). Code
never spells one by hand: this generator writes, for every owner in OUTPUTS, a committed header holding the codepoints
of the entries that owner owns:

  - each Cerid identifier registry (ENUMS) becomes an `enum class ... : u16` with its table of values and the
    register entry of each value: "Cerid protocol identifiers" give `ProtocolId`, `kProtocolIds` and
    `protocol_register_id`; "Cerid algorithm identifiers" give `AlgorithmId`, `kAlgorithmIds` and
    `algorithm_register_id`, whose high byte is the algorithm's class (ALGORITHM_CLASSES: `AlgorithmClass` and
    `algorithm_class`);
  - a value pattern with `?` nibbles ("0x?A?A") expands to every value with the same digit in each `?`, written as
    the reserved table of its registry (`kGreaseIds` and `is_grease`: the GREASE values, reserved as extension
    identifiers and as version majors);
  - an IANA codepoint becomes a constant of the type its registry fixes (REGISTRIES). A codepoint the register marks
    `verified: false` (taken from knowledge, not yet checked against the IANA registry) is written into the nested
    namespace `unverified`, so code using one says so where it is used; verifying it moves it out.

An owner that is a registered module with codepoints and no OUTPUTS entry, a registry this generator has no type for,
a malformed value and two codepoints with one constant name all fail, so a new owner or registry is a decision here.

  python scripts/gen-register-ids.py            # write every output
  python scripts/gen-register-ids.py --check    # byte-compare with the committed outputs; nonzero on drift or error

The crd-net-register-ids-drift CTest runs --check; scripts/generated-sources.json records each output's bytes.
"""
import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTER = 'cmake/standards-register.json'
GENERATOR = 'scripts/gen-register-ids.py'
# owner -> (header, namespace)
OUTPUTS = {
    'net': ('engine/foundation/net/include/crd/net/register_ids.hpp', 'crd::net'),
    'crypto': ('engine/foundation/crypto/include/crd/crypto/register_ids.hpp', 'crd::crypto'),
}
# Cerid identifier registry -> (enumeration, noun, what one value names)
ENUMS = {
    'Cerid protocol identifiers': ('ProtocolId', 'protocol', 'one per Cerid wire protocol'),
    'Cerid algorithm identifiers': ('AlgorithmId', 'algorithm', 'one per algorithm, its class in the high byte'),
}
ALGORITHMS = 'Cerid algorithm identifiers'
# The cryptographic functions a register entry declares in `cryptography`, in their canonical order.
CRYPTO_FUNCTIONS = ('authentication', 'confidentiality', 'integrity', 'key-exchange', 'key-derivation')
# The high byte of a Cerid algorithm identifier (docs/design/registers.md#cerid-identifiers): its enumerator, meaning
# and the cryptographic functions an algorithm of the class performs (the register's `cryptography` vocabulary, which
# the COMPLY.1 cryptography inventory reads).
ALGORITHM_CLASSES = {
    0x01: ('HybridKeyExchange', 'post-quantum/traditional hybrid key exchange', ('key-exchange',)),
    0x02: ('ClassicalKeyExchange', 'traditional elliptic-curve key exchange', ('key-exchange',)),
    0x03: ('PostQuantumSignature', 'post-quantum signature', ('authentication',)),
    0x04: ('ClassicalSignature', 'traditional signature', ('authentication',)),
    0x05: ('Aead', 'authenticated encryption with associated data', ('confidentiality', 'integrity')),
    0x06: ('Kdf', 'key derivation', ('key-derivation',)),
}
RESERVED = {'Cerid GREASE values': 'Grease'}
# registry -> (constant prefix, C++ type, value kind)
REGISTRIES = {
    'QUIC Versions': ('', 'u32', 'integer'),
    'QUIC Transport Parameters': ('QuicTp', 'u64', 'integer'),
    'TLS ALPN Protocol IDs': ('Alpn', 'std::string_view', 'alpn'),
    'TLS Supported Groups': ('Group', 'u16', 'integer'),
    'TLS SignatureScheme': ('SignatureScheme', 'u16', 'integer'),
    'TLS ExtensionType Values': ('TlsExtension', 'u16', 'integer'),
    'AEAD Algorithms': ('', 'u16', 'integer'),  # names start AEAD_
    'Object identifiers': ('Oid', 'std::string_view', 'oid'),
}
HEX = re.compile(r'^0x[0-9A-Fa-f]+$')
PATTERN = re.compile(r'^0x[0-9A-Fa-f?]*\?[0-9A-Fa-f?]*$')
# text value kind -> (form, what it is)
TEXT = {
    'alpn': (re.compile(r'^[\x21-\x7e]{1,255}$'), 'a printable ALPN identifier'),
    'oid': (re.compile(r'^[0-2](?:\.(?:0|[1-9][0-9]*))+$'), 'a dotted object identifier'),
}
LIMITS = {'u16': 0xFFFF, 'u32': 0xFFFFFFFF, 'u64': (1 << 62) - 1}  # QUIC varints stop at 2^62 - 1

_spec = importlib.util.spec_from_file_location('check_master_plan', ROOT / 'scripts' / 'check-master-plan.py')
_plan = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_plan)


class RegisterError(Exception):
    pass


def camel(text):
    words = [word for word in re.split(r'[^A-Za-z0-9]+', text) if word]
    return ''.join(word[:1].upper() + word[1:].lower() for word in words)


def integer(where, text, ctype):
    if not HEX.match(text) and not text.isdigit():
        raise RegisterError(f'{where}: value {text!r} is neither hexadecimal (0x...) nor decimal')
    value = int(text, 0) if text.startswith('0x') else int(text)
    if value > LIMITS[ctype]:
        raise RegisterError(f'{where}: value {text} does not fit {ctype}')
    return value


def expand(where, pattern):
    """Every value of a `?` pattern, the same hexadecimal digit in each `?` (RFC 8701's 0x?A?A gives 16 values)."""
    if not PATTERN.match(pattern):
        raise RegisterError(f'{where}: pattern {pattern!r} is not 0x followed by hexadecimal digits and ?')
    digits = pattern[2:]
    if len(digits) != 4:
        raise RegisterError(f'{where}: pattern {pattern!r} is not a 16-bit identifier')
    return [int(digits.replace('?', f'{nibble:X}'), 16) for nibble in range(16)]


def owned_codepoints(register, owner):
    for entry in register['entries']:
        if entry.get('owner') == owner:
            for point in entry.get('codepoints', []):
                yield entry, point


def model(register, owner):
    """(enums, reserved, constants) of one owner in register order; enums maps each Cerid identifier registry the
    owner uses to its (name, value, entry) members."""
    enums, reserved, constants, names = {}, [], [], {}
    for entry, point in owned_codepoints(register, owner):
        where = f'{REGISTER}: {entry["id"]} codepoint {point["name"]!r}'
        registry, verified = point['registry'], point.get('verified', True)
        if registry in ENUMS:
            name = f'{ENUMS[registry][0]}::{camel(point["name"])}'
            value = integer(where, point['value'], 'u16')
            if registry == ALGORITHMS and (value >> 8 not in ALGORITHM_CLASSES or not value & 0xFF):
                raise RegisterError(f'{where}: {point["value"]} is not a class byte of ALGORITHM_CLASSES followed by '
                                    'a nonzero number')
            enums.setdefault(registry, []).append((camel(point['name']), value, entry['id']))
        elif registry in RESERVED:
            name = RESERVED[registry]
            reserved.append((name, expand(where, point['value']), entry['id'], registry, point['value']))
        elif registry in REGISTRIES:
            prefix, ctype, kind = REGISTRIES[registry]
            name = 'k' + prefix + camel(point['name'])
            if kind in TEXT:
                form, what = TEXT[kind]
                if not form.match(point['value']):
                    raise RegisterError(f'{where}: {point["value"]!r} is not {what}')
                value = point['value']
            else:
                value = integer(where, point['value'], ctype)
            constants.append((name, ctype, kind, value, verified, entry['id'], registry, point['name']))
        else:
            raise RegisterError(f'{where}: registry {registry!r} has no type in {GENERATOR}; add it to REGISTRIES')
        if (registry in ENUMS or registry in RESERVED) and not verified:
            raise RegisterError(f'{where}: a Cerid identifier is the register\'s own value and cannot be unverified')
        if name in names:
            raise RegisterError(f'{where}: constant {name.rpartition(":")[2]} is also generated for {names[name]}')
        names[name] = entry['id']
    for registry, members in enums.items():
        values = [value for _, value, _ in members]
        if len(set(values)) != len(values):
            raise RegisterError(f'{REGISTER}: two {registry} share a value')
        if any(value in grease for value in values for _, grease, *_ in reserved):
            raise RegisterError(f'{REGISTER}: a Cerid {ENUMS[registry][1]} identifier lies in a reserved pattern')
    return enums, reserved, constants


def pattern_test(pattern):
    """The body lines of a constexpr test for membership in a `?` pattern (validated by expand())."""
    digits = pattern[2:]
    shifts = [4 * (len(digits) - 1 - index) for index in range(len(digits))]
    mask = sum(0xF << shift for digit, shift in zip(digits, shifts) if digit != '?')
    fixed = sum(int(digit, 16) << shift for digit, shift in zip(digits, shifts) if digit != '?')
    wild = [shift for digit, shift in zip(digits, shifts) if digit == '?']
    terms = [f'(value & 0x{mask:04X}U) == 0x{fixed:04X}U'] if mask else []
    terms += [f'((value >> {wild[0]}U) & 0x0FU) == ((value >> {shift}U) & 0x0FU)' for shift in wild[1:]]
    if not terms:
        return ['    return true;']
    lines = ['    return ' + terms[0]]
    for term in terms[1:]:
        lines[-1] += ' &&'
        lines.append('           ' + term)
    lines[-1] += ';'
    return lines


def literal(ctype, kind, value):
    if kind in TEXT:
        return f'"{value}"'
    width = {'u16': 4, 'u32': 8, 'u64': 4}[ctype]
    return f'0x{value:0{width}X}U' if ctype != 'u64' else f'0x{value:0{width}X}ULL'


def render_enum(registry, members):
    """One Cerid identifier registry: its enumeration, its table and the register entry of each value."""
    enum, noun, what = ENUMS[registry]
    width = max(len(name) for name, _, _ in members)
    lines = [f'// {registry}: {what}; never reused. 16 bits on the wire.',
             '// NOLINTNEXTLINE(performance-enum-size)', f'enum class {enum} : u16', '{']
    lines += [f'    {name:<{width}} = 0x{value:04X}U, // {entry}' for name, value, entry in members]
    lines += ['};', '', f'inline constexpr {enum} k{enum[:-2]}Ids[] = {{']
    lines += [f'    {enum}::{name},' for name, _, _ in members]
    lines += ['};', '', '// The register entry that defines `id` ("" for a value outside the enumeration).',
              f'[[nodiscard]] constexpr const char* {noun}_register_id({enum} id) noexcept', '{',
              '    switch (id)', '    {']
    for index, (name, _, entry) in enumerate(members):
        lines.append(f'    case {enum}::{name}:')
        if index + 1 == len(members) or members[index + 1][2] != entry:
            lines.append(f'        return "{entry}";')  # consecutive values of one entry share the return
    lines += ['    }', '    return "";', '}', '']
    if registry == ALGORITHMS:
        width = max(len(name) for name, _, _ in ALGORITHM_CLASSES.values())
        lines += ['// The class of an algorithm: the high byte of its identifier. An identifier decoded from bytes may',
                  '// carry a class byte outside this enumeration; no code accepts one.',
                  'enum class AlgorithmClass : u8', '{']
        lines += [f'    {name:<{width}} = 0x{value:02X}U, // {meaning}'
                  for value, (name, meaning, _) in ALGORITHM_CLASSES.items()]
        lines += ['};', '',
                  '[[nodiscard]] constexpr AlgorithmClass algorithm_class(AlgorithmId id) noexcept', '{',
                  '    return static_cast<AlgorithmClass>(static_cast<u16>(id) >> 8U);', '}', '']
    return lines


def render(owner, register, namespace):
    enums, reserved, constants = model(register, owner)
    lines = [
        '#pragma once',
        '',
        f'// GENERATED by {GENERATOR} from {REGISTER} - DO NOT EDIT. Regenerate with',
        f'// `python {GENERATOR}`; the crd-{owner}-register-ids-drift CTest fails on drift.',
        '//',
        f'// The codepoints the NET-STD standards register assigns to crd-{owner}',
        '// (docs/design/registers.md#cerid-identifiers). A constant in `unverified` was taken from knowledge and is',
        '// not yet checked against its IANA registry; an entry that uses one cannot become implemented until the',
        '// register marks it verified and this header moves it out.',
        '',
        '#include <crd/core/types.hpp>',
        '',
    ]
    if any(kind in TEXT for _, _, kind, *_ in constants):
        lines += ['#include <string_view>', '']
    lines += [f'namespace {namespace}', '{']
    for registry in ENUMS:
        if registry in enums:
            lines += render_enum(registry, enums[registry])
    for name, values, entry, registry, pattern in reserved:
        lines += [f'// {registry} reserved by {entry}: never assigned, sent by every build so that peers which',
                  '// refuse unknown values fail in testing (RFC 8701 practice).',
                  f'inline constexpr u16 k{name}Ids[] = {{']
        for start in range(0, len(values), 8):
            lines.append('    ' + ' '.join(f'0x{value:04X}U,' for value in values[start:start + 8]))
        lines += ['};', '',
                  f'// Whether `value` is one of k{name}Ids: the fixed digits of {pattern} '
                  'and the same digit in every `?`.',
                  f'[[nodiscard]] constexpr bool is_{name.lower()}(u16 value) noexcept', '{',
                  *pattern_test(pattern), '}', '']
    for verified in (True, False):
        group = [constant for constant in constants if constant[4] is verified]
        if not group:
            continue
        nested = 'iana' if verified else 'iana::unverified'
        lines += [f'namespace {nested}', '{']
        for name, ctype, kind, value, _, entry, registry, point in group:
            lines += [f'// {entry}: {registry}, {point}',
                      f'inline constexpr {ctype} {name} = {literal(ctype, kind, value)};']
        lines += [f'}} // namespace {nested}', '']
    lines += [f'}} // namespace {namespace}', '']
    return '\n'.join(lines)


def outputs(root):
    register = json.loads((root / REGISTER).read_text(encoding='utf-8'))
    registered = _plan.registered_modules((root / 'CMakeLists.txt').read_text(encoding='utf-8'))
    owners = {entry.get('owner') for entry in register['entries'] if entry.get('codepoints')}
    missing = sorted(owner for owner in owners & registered if owner not in OUTPUTS)
    if missing:
        raise RegisterError(f'registered owners {", ".join(missing)} own codepoints and have no output in OUTPUTS')
    return {header: render(owner, register, namespace)
            for owner, (header, namespace) in OUTPUTS.items() if owner in registered}


def main(argv=None):
    parser = argparse.ArgumentParser(description='Generate register identifiers for their owning modules')
    parser.add_argument('--check', action='store_true', help='compare with the committed outputs instead of writing')
    parser.add_argument('--root', default=str(ROOT), help='repository root (tests use a fixture)')
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    try:
        generated = outputs(root)
    except (RegisterError, OSError, ValueError, KeyError) as error:
        print(f'FAIL: {error}')
        return 1
    drift = False
    for relative, text in generated.items():
        path = root / relative
        data = text.encode('utf-8')
        if args.check:
            current = path.read_bytes().replace(b'\r\n', b'\n') if path.exists() else None
            if current != data:
                print(f'FAIL: {relative} differs from {GENERATOR}\'s output; run python {GENERATOR}')
                drift = True
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            print(f'wrote {relative}')
    if args.check and not drift:
        print(f'register identifiers current: {len(generated)} header(s)')
    return 1 if drift else 0


if __name__ == '__main__':
    sys.exit(main())
