#!/usr/bin/env python3
"""Generate Cerid's wire formats from their authored schemas (NET.3; docs/design/network-foundation.md#wire-formats).

Every entry of OUTPUTS is one authored schema (JSON, format cerid-wire-schema/1) and the committed files it generates:
a C++ header (the message structs, their encoders and bounds-checked zero-copy decoders, the S18 constants and the
descriptions admission reads) and a fuzz target; a schema that sets `vectors` also gets known answers computed here, by
an interpreter of the schema that shares nothing with the C++ it checks (the Ref* methods), so the generated code is
held to a second implementation of the layout on every lane.

What a schema may not do is refused here, before any code exists: recursion, a list of elements of no bits, a bound
that does not fit, a duplicate identifier across schemas, and any change to the layout of a revision already recorded
in its `history` (a minor revision only appends; anything else takes a new major).

  python scripts/gen-wire-schemas.py            # write every output
  python scripts/gen-wire-schemas.py --check    # byte-compare with the committed outputs; nonzero on drift or error

The crd-net-wire-schemas-drift CTest runs --check; scripts/generated-sources.json records each output's bytes;
scripts/test-wire-schemas.py holds the refusals.
"""
import argparse
import hashlib
import json
import random
import re
import struct
import sys
import textwrap
from pathlib import Path

FORMAT = 'cerid-wire-schema/1'
GENERATOR = 'scripts/gen-wire-schemas.py'
MAX_DEPTH = 8
MAX_MESSAGE_BYTES = 0xFFFFFFFF
MINORS_PAST_SCHEMA = 2  # the fuzz target and the vectors also read as writers this far ahead

OUTPUTS = [
    {
        'schema': 'engine/foundation/net/schemas/session_control.json',
        'header': 'engine/foundation/net/include/crd/net/wire/session_control.hpp',
        'include': 'crd/net/wire/session_control.hpp',
        'fuzz': 'tests/foundation/net/fuzz/fuzz_wire_session_control.cpp',
    },
    {
        'schema': 'tests/foundation/net/schemas/wire_test.json',
        'header': 'tests/foundation/net/generated/wire_test.hpp',
        'include': 'generated/wire_test.hpp',
        'fuzz': 'tests/foundation/net/fuzz/fuzz_wire_test.cpp',
        'vectors': 'tests/foundation/net/generated/wire_test_vectors.inc',
    },
]

SCOPE_KINDS = {'transport': 0, 'World': 1, 'Project': 2, 'Room': 3, 'Upload': 4, 'Tool': 5, 'Service': 6}
CHANNEL_CLASSES = ('Control', 'Input', 'State', 'Event', 'Transaction', 'Bulk', 'Media')
NAME = re.compile(r'^[a-z][a-z0-9_]*$')
TYPE_NAME = re.compile(r'^[A-Z][A-Za-z0-9]*$')
CPP_KEYWORDS = {'alignas', 'alignof', 'auto', 'bool', 'break', 'case', 'catch', 'char', 'class', 'const', 'continue',
                'default', 'delete', 'do', 'double', 'else', 'enum', 'explicit', 'export', 'extern', 'false', 'float',
                'for', 'friend', 'goto', 'if', 'inline', 'int', 'long', 'mutable', 'namespace', 'new', 'noexcept',
                'nullptr', 'operator', 'private', 'protected', 'public', 'register', 'return', 'short', 'signed',
                'sizeof', 'static', 'struct', 'switch', 'template', 'this', 'throw', 'true', 'try', 'typedef',
                'typename', 'union', 'unsigned', 'using', 'virtual', 'void', 'volatile', 'while', 'error', 'value',
                'position', 'minor', 'major', 'w', 'r'}


class SchemaError(Exception):
    pass


class RefFail(Exception):
    """The reference reader refused the bytes."""


def require(condition, message):
    if not condition:
        raise SchemaError(message)


def bit_width(value):
    return value.bit_length()


def wrap_lines(lines, indent='    '):
    return [indent + line if line else line for line in lines]


def guard(expression):
    if len(expression) <= 48:
        return [f'if (const WireError error = {expression}; error != WireError::Ok)', '{', '    return error;', '}']
    return [f'if (const WireError error = {expression};', '    error != WireError::Ok)', '{', '    return error;', '}']


def refuse_if(condition, error):
    return [f'if ({condition})', '{', f'    return WireError::{error};', '}']


def int_literal(value, cpp):
    if value == -(1 << 63):
        return 'kWireMinI64'
    if cpp == 'i64':
        return f'{value}LL'
    if value < 0:
        return f'static_cast<{cpp}>({value}LL)'
    return f'static_cast<{cpp}>({value}ULL)'


def unsigned_cpp(maximum):
    for bits, name in ((8, 'u8'), (16, 'u16'), (32, 'u32'), (64, 'u64')):
        if maximum < (1 << bits):
            return name, bits
    raise SchemaError(f'{maximum} needs more than 64 bits')


def integer_cpp(minimum, maximum):
    if minimum >= 0:
        return unsigned_cpp(maximum)[0]
    for bits, name in ((8, 'i8'), (16, 'i16'), (32, 'i32'), (64, 'i64')):
        if minimum >= -(1 << (bits - 1)) and maximum < (1 << (bits - 1)):
            return name
    raise SchemaError(f'[{minimum}, {maximum}] needs more than 64 bits')


def float_literal(value):
    return f'{float(value)!r}F'


# ---- The reference: bits as an integer list, nothing shared with the C++ emitters. ----


class RefWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        for index in range(count - 1, -1, -1):
            self.bits.append((value >> index) & 1)

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def data(self):
        self.align()
        out = bytearray()
        for at in range(0, len(self.bits), 8):
            byte = 0
            for bit in self.bits[at:at + 8]:
                byte = (byte << 1) | bit
            out.append(byte)
        return bytes(out)


class RefReader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def left(self):
        return len(self.data) * 8 - self.at

    def take(self, count):
        if self.left() < count:
            raise RefFail('truncated')
        value = 0
        for _ in range(count):
            value = (value << 1) | ((self.data[self.at // 8] >> (7 - self.at % 8)) & 1)
            self.at += 1
        return value

    def align(self):
        while self.at % 8:
            if self.take(1):
                raise RefFail('padding')

    def take_bytes(self, count):
        if self.left() // 8 < count:
            raise RefFail('truncated')
        start = self.at // 8
        self.at += count * 8
        return bytes(self.data[start:start + count])


def ref_put_varuint(writer, value, group):
    mask = (1 << group) - 1
    while value > mask:
        writer.put((1 << group) | (value & mask), group + 1)
        value >>= group
    writer.put(value, group + 1)


def ref_take_varuint(reader, group, maximum):
    limit = max(1, (bit_width(maximum) + group - 1) // group)
    value = 0
    index = 0
    while True:
        chunk = reader.take(group + 1)
        more = chunk >> group
        chunk &= (1 << group) - 1
        if index >= limit:
            raise RefFail('too many groups')
        value |= chunk << (index * group)
        if not more:
            if chunk == 0 and index != 0:
                raise RefFail('overlong')
            break
        index += 1
    if value > maximum:
        raise RefFail('above the bound')
    return value


def ref_edge_int(rng, low, high):
    choice = rng.randrange(6)
    if choice == 0:
        return low
    if choice == 1:
        return high
    if choice == 2:
        return min(high, low + 1)
    if choice == 3:
        return max(low, high - 1)
    return rng.randint(low, high)


def utf8_text(rng, maximum):
    pieces = bytearray()
    for _ in range(rng.randrange(6)):
        piece = rng.choice(['a', 'Z', '0', ' ', 'é', 'İ', '€', '中', '\U0001f600', '\U0010ffff'])
        encoded = piece.encode('utf-8')
        if len(pieces) + len(encoded) <= maximum:
            pieces += encoded
    return bytes(pieces)


# ---- Types ----


class Type:
    lossy = False
    scalar = True

    def layout(self, minor):
        raise NotImplementedError

    def member_init(self, default):
        return ' = {}'.format(self.default_literal(default)) if default is not None else self.zero_init()

    def zero_init(self):
        return ''

    def default_literal(self, default):
        raise SchemaError(f'{type(self).__name__} takes no default')

    def differs(self, ref, default):
        raise SchemaError(f'{type(self).__name__} cannot compare with a default')

    def same(self, a, b):
        return f'wire_same({a}, {b})'

    def ref_default(self, default):
        return default if default is not None else 0

    def cpp_assign(self, emit, ref, value):
        emit.line(f'{ref} = {int_literal(value, self.cpp())};')

    def nested_codecs(self, emit_codec):
        """Emit whatever element codecs this type needs before the struct that holds it."""

    def children(self):
        return ()


class BoolType(Type):
    min_bits = max_bits = 1

    def layout(self, minor):
        return ['bool']

    def cpp(self):
        return 'bool'

    def zero_init(self):
        return ' = false'

    def default_literal(self, default):
        require(isinstance(default, bool), 'a bool default must be true or false')
        return 'true' if default else 'false'

    def differs(self, ref, default):
        return f'{ref}' if not default else f'!{ref}'

    def put(self, ref):
        return [f'w.put({ref} ? 1U : 0U, 1U);']

    def end(self, ref):
        return ['position += 1U;']

    def take(self, ref):
        return guard(f'wire_take_bool(r, {ref})')

    def ref_default(self, default):
        return bool(default)

    def ref_put(self, writer, value, minor):
        writer.put(1 if value else 0, 1)

    def ref_take(self, reader, writer_minor):
        return bool(reader.take(1))

    def ref_gen(self, rng, minor):
        return rng.random() < 0.5

    def cpp_assign(self, emit, ref, value):
        emit.line(f'{ref} = {"true" if value else "false"};')


class UintType(Type):
    def __init__(self, spec):
        self.bits = spec.get('bits')
        require(isinstance(self.bits, int) and 1 <= self.bits <= 64, f'uint bits must be 1 to 64: {spec}')
        self.min_bits = self.max_bits = self.bits
        self.top = (1 << self.bits) - 1
        self.cpp_name, self.cpp_bits = unsigned_cpp(self.top)

    def layout(self, minor):
        return ['uint', self.bits]

    def cpp(self):
        return self.cpp_name

    def zero_init(self):
        return ' = 0U'

    def default_literal(self, default):
        require(isinstance(default, int) and 0 <= default <= self.top, f'default {default} outside the uint')
        return int_literal(default, self.cpp_name)

    def differs(self, ref, default):
        return f'{ref} != {int_literal(default or 0, self.cpp_name)}'

    def put(self, ref):
        lines = []
        if self.bits != self.cpp_bits:
            lines += refuse_if(f'(static_cast<u64>({ref}) >> {self.bits}U) != 0U', 'OutOfRange')
        return lines + [f'w.put({ref}, {self.bits}U);']

    def end(self, ref):
        return [f'position += {self.bits}U;']

    def take(self, ref):
        return guard(f'wire_take_uint(r, {self.bits}U, {ref})')

    def ref_put(self, writer, value, minor):
        writer.put(value, self.bits)

    def ref_take(self, reader, writer_minor):
        return reader.take(self.bits)

    def ref_gen(self, rng, minor):
        return ref_edge_int(rng, 0, self.top)


class RangeType(Type):
    def __init__(self, spec):
        self.low = spec.get('min')
        self.high = spec.get('max')
        require(isinstance(self.low, int) and isinstance(self.high, int) and self.low < self.high,
                f'range needs min < max: {spec}')
        self.span = self.high - self.low
        require(self.span < (1 << 64), f'range wider than 64 bits: {spec}')
        self.bits = bit_width(self.span)
        self.min_bits = self.max_bits = self.bits
        self.cpp_name = integer_cpp(self.low, self.high)

    def layout(self, minor):
        return ['range', self.low, self.high]

    def cpp(self):
        return self.cpp_name

    def zero_init(self):
        return f' = {int_literal(max(self.low, min(self.high, 0)), self.cpp_name)}'

    def default_literal(self, default):
        require(isinstance(default, int) and self.low <= default <= self.high, f'default {default} outside the range')
        return int_literal(default, self.cpp_name)

    def ref_default(self, default):
        return default if default is not None else max(self.low, min(self.high, 0))

    def differs(self, ref, default):
        return f'{ref} != {int_literal(self.ref_default(default), self.cpp_name)}'

    def put(self, ref):
        low = int_literal(self.low, self.cpp_name)
        return refuse_if(f'!wire_range_holds<{self.cpp_name}>({ref}, {low}, {self.span}ULL)', 'OutOfRange') + [
            f'wire_put_range<{self.cpp_name}>(w, {ref}, {low}, {self.bits}U);']

    def end(self, ref):
        return [f'position += {self.bits}U;']

    def take(self, ref):
        low = int_literal(self.low, self.cpp_name)
        return guard(f'wire_take_range<{self.cpp_name}>(r, {low}, {self.span}ULL, {self.bits}U, {ref})')

    def ref_put(self, writer, value, minor):
        writer.put(value - self.low, self.bits)

    def ref_take(self, reader, writer_minor):
        offset = reader.take(self.bits)
        if offset > self.span:
            raise RefFail('above the range')
        return self.low + offset

    def ref_gen(self, rng, minor):
        return ref_edge_int(rng, self.low, self.high)


class VaruintType(Type):
    def __init__(self, spec):
        self.top = spec.get('max')
        self.group = spec.get('group', 7)
        require(isinstance(self.top, int) and 1 <= self.top < (1 << 64), f'varuint needs a max: {spec}')
        require(isinstance(self.group, int) and 1 <= self.group <= 32, f'varuint group must be 1 to 32: {spec}')
        self.min_bits = self.group + 1
        self.max_bits = ((bit_width(self.top) + self.group - 1) // self.group) * (self.group + 1)
        self.cpp_name = unsigned_cpp(self.top)[0]

    def layout(self, minor):
        return ['varuint', self.top, self.group]

    def cpp(self):
        return self.cpp_name

    def zero_init(self):
        return ' = 0U'

    def default_literal(self, default):
        require(isinstance(default, int) and 0 <= default <= self.top, f'default {default} outside the varuint')
        return int_literal(default, self.cpp_name)

    def differs(self, ref, default):
        return f'{ref} != {int_literal(default or 0, self.cpp_name)}'

    def put(self, ref):
        lines = []
        if self.top != (1 << unsigned_cpp(self.top)[1]) - 1:
            lines += refuse_if(f'static_cast<u64>({ref}) > {self.top}ULL', 'OutOfRange')
        return lines + [f'wire_put_varuint(w, {ref}, {self.group}U);']

    def end(self, ref):
        return [f'position += wire_varuint_bits({ref}, {self.group}U);']

    def take(self, ref):
        return guard(f'wire_take_varuint_as(r, {self.group}U, {self.top}ULL, {ref})')

    def ref_put(self, writer, value, minor):
        ref_put_varuint(writer, value, self.group)

    def ref_take(self, reader, writer_minor):
        return ref_take_varuint(reader, self.group, self.top)

    def ref_gen(self, rng, minor):
        return ref_edge_int(rng, 0, self.top if rng.random() < 0.3 else min(self.top, 1 << rng.randrange(1, 40)))


class VarsintType(Type):
    def __init__(self, spec):
        self.low = spec.get('min')
        self.high = spec.get('max')
        self.group = spec.get('group', 7)
        require(isinstance(self.low, int) and isinstance(self.high, int) and self.low < 0 <= self.high,
                f'varsint needs min < 0 <= max: {spec}')
        require(self.low >= -(1 << 63) and self.high < (1 << 63), f'varsint wider than 64 bits: {spec}')
        require(isinstance(self.group, int) and 1 <= self.group <= 32, f'varsint group must be 1 to 32: {spec}')
        self.widest = max(self.zigzag(self.low), self.zigzag(self.high))
        self.min_bits = self.group + 1
        self.max_bits = ((bit_width(self.widest) + self.group - 1) // self.group) * (self.group + 1)
        self.cpp_name = integer_cpp(self.low, self.high)

    @staticmethod
    def zigzag(value):
        return (value << 1) ^ (value >> 63) if value >= 0 else ((-value - 1) << 1) | 1

    def layout(self, minor):
        return ['varsint', self.low, self.high, self.group]

    def cpp(self):
        return self.cpp_name

    def zero_init(self):
        return ' = 0'

    def default_literal(self, default):
        require(isinstance(default, int) and self.low <= default <= self.high, f'default {default} outside the varsint')
        return int_literal(default, self.cpp_name)

    def differs(self, ref, default):
        return f'{ref} != {int_literal(default or 0, self.cpp_name)}'

    def put(self, ref):
        low = int_literal(self.low, 'i64')
        span = self.high - self.low
        return refuse_if(f'!wire_range_holds<i64>({ref}, {low}, {span}ULL)', 'OutOfRange') + [
            f'wire_put_varuint(w, wire_zigzag({ref}), {self.group}U);']

    def end(self, ref):
        return [f'position += wire_varuint_bits(wire_zigzag({ref}), {self.group}U);']

    def take(self, ref):
        low = int_literal(self.low, 'i64')
        high = int_literal(self.high, 'i64')
        return guard(f'wire_take_varsint(r, {self.group}U, {low}, {high}, {ref})')

    def ref_put(self, writer, value, minor):
        ref_put_varuint(writer, self.zigzag(value), self.group)

    def ref_take(self, reader, writer_minor):
        raw = ref_take_varuint(reader, self.group, self.widest)
        value = (raw >> 1) if not raw & 1 else -(raw >> 1) - 1
        if value < self.low or value > self.high:
            raise RefFail('outside the varsint')
        return value

    def ref_gen(self, rng, minor):
        return ref_edge_int(rng, self.low, self.high) if rng.random() < 0.5 else rng.randint(
            max(self.low, -300), min(self.high, 300))


class EnumType(Type):
    def __init__(self, name, spec, schema_minor):
        require(TYPE_NAME.match(name), f'enum name {name}')
        self.name = name
        self.schema_minor = schema_minor
        self.bits = spec.get('bits')
        require(isinstance(self.bits, int) and 1 <= self.bits <= 32, f'enum {name}: bits must be 1 to 32')
        self.min_bits = self.max_bits = self.bits
        self.values = []
        seen = set()
        for entry in spec.get('values', []):
            value_name = entry['name']
            require(TYPE_NAME.match(value_name), f'enum {name}: value name {value_name}')
            number = entry['value']
            require(isinstance(number, int) and 0 <= number < (1 << self.bits),
                    f'enum {name}: {value_name} = {number} does not fit {self.bits} bits')
            require(number not in seen and value_name not in {v[0] for v in self.values},
                    f'enum {name}: {value_name} = {number} is a duplicate')
            seen.add(number)
            since = entry.get('since', 0)
            require(isinstance(since, int) and 0 <= since <= schema_minor, f'enum {name}: {value_name} since {since}')
            self.values.append((value_name, number, since))
        require(self.values, f'enum {name} has no value')
        self.cpp_name = unsigned_cpp((1 << self.bits) - 1)[0]

    def layout(self, minor):
        return ['enum', self.bits, sorted(v[1] for v in self.values if v[2] <= minor)]

    def cpp(self):
        return self.name

    def zero_init(self):
        return f' = {self.name}::{self.first()[0]}'

    def first(self):
        return min(self.values, key=lambda v: v[1])

    def by_name(self, default):
        for value in self.values:
            if value[0] == default:
                return value
        raise SchemaError(f'enum {self.name} has no value {default}')

    def default_literal(self, default):
        return f'{self.name}::{self.by_name(default)[0]}'

    def ref_default(self, default):
        return self.by_name(default)[1] if default is not None else self.first()[1]

    def differs(self, ref, default):
        return f'{ref} != {self.name}::{(self.by_name(default) if default else self.first())[0]}'

    def put(self, ref):
        return ([f'if (!enum_known({ref}, minor))', '{',
                 f'    return enum_known({ref}, kMinor) ? WireError::NotInRevision : WireError::UnknownValue;',
                 '}'] + [f'w.put(static_cast<u64>({ref}), {self.bits}U);'])

    def end(self, ref):
        return [f'position += {self.bits}U;']

    def take(self, ref):
        return (guard(f'wire_take_uint(r, {self.bits}U, {ref})') +
                refuse_if(f'writer_minor <= kMinor && !enum_known({ref}, writer_minor)', 'UnknownValue'))

    def ref_put(self, writer, value, minor):
        writer.put(value, self.bits)

    def ref_take(self, reader, writer_minor):
        number = reader.take(self.bits)
        if writer_minor <= self.schema_minor:
            known = any(v[1] == number and v[2] <= writer_minor for v in self.values)
            if not known:
                raise RefFail('unknown value')
        return number

    def ref_gen(self, rng, minor):
        return rng.choice([v[1] for v in self.values if v[2] <= minor])

    def cpp_assign(self, emit, ref, value):
        emit.line(f'{ref} = static_cast<{self.name}>({value}U);')


class FloatType(Type):
    def __init__(self, width, spec):
        self.width = width
        self.nonfinite = bool(spec.get('nonfinite', False))
        self.min_bits = self.max_bits = width
        self.cpp_name = 'f32' if width == 32 else 'f64'
        self.exponent = 0x7F800000 if width == 32 else 0x7FF0000000000000

    def layout(self, minor):
        return [self.cpp_name, self.nonfinite]

    def cpp(self):
        return self.cpp_name

    def zero_init(self):
        return ' = 0.0F' if self.width == 32 else ' = 0.0'

    def differs(self, ref, default):
        require(default is None, 'a float default is not supported')
        return f'!wire_same({ref}, {self.cpp_name}{{}})'

    def put(self, ref):
        lines = [] if self.nonfinite else refuse_if(f'!wire_finite({ref})', 'NonFinite')
        return lines + [f'wire_put_float(w, {ref});']

    def end(self, ref):
        return [f'position += {self.width}U;']

    def take(self, ref):
        return guard(f'wire_take_float(r, {"false" if self.nonfinite else "true"}, {ref})')

    def ref_put(self, writer, value, minor):
        writer.put(value, self.width)

    def ref_take(self, reader, writer_minor):
        bits = reader.take(self.width)
        if not self.nonfinite and (bits & self.exponent) == self.exponent:
            raise RefFail('not finite')
        return bits

    def ref_gen(self, rng, minor):
        if self.width == 32:
            bits = struct.unpack('<I', struct.pack('<f', rng.choice([0.0, -0.0, 1.5, -2.25, 1e30, 1e-30])))[0]
        else:
            bits = struct.unpack('<Q', struct.pack('<d', rng.choice([0.0, -0.0, 1.5, -2.25, 1e300, 1e-300])))[0]
        if self.nonfinite and rng.random() < 0.4:
            return self.exponent | (rng.randrange(1, 4) if rng.random() < 0.5 else 0)
        return bits

    def cpp_assign(self, emit, ref, value):
        raw = f'{value}U' if self.width == 32 else f'{value}ULL'
        wide = 'u32' if self.width == 32 else 'u64'
        emit.line(f'{ref} = std::bit_cast<{self.cpp_name}>(static_cast<{wide}>({raw}));')


class BoundedType(Type):
    lossy = True

    def __init__(self, spec):
        self.low = float(spec.get('min'))
        self.high = float(spec.get('max'))
        require(self.low < self.high, f'bounded needs min < max: {spec}')
        if 'resolution' in spec:
            require('bits' not in spec, f'bounded takes bits or resolution, not both: {spec}')
            self.max_code = max(1, int(-(-(self.high - self.low) // float(spec['resolution']))))
        else:
            bits = spec.get('bits')
            require(isinstance(bits, int) and bits >= 1, f'bounded needs bits or a resolution: {spec}')
            self.max_code = (1 << bits) - 1 if bits <= 24 else 0
        require(1 <= self.max_code <= (1 << 24) - 1, f'bounded takes at most 24 bits: {spec}')
        self.bits = bit_width(self.max_code)
        self.min_bits = self.max_bits = self.bits
        self.format_name = None

    def layout(self, minor):
        return ['bounded', self.low, self.high, self.max_code]

    def cpp(self):
        return 'f32'

    def zero_init(self):
        return ' = 0.0F'

    def differs(self, ref, default):
        require(default is None, 'a bounded default is not supported')
        return f'!wire_same({ref}, f32{{}})'

    def constants(self, name):
        self.format_name = name
        return [f'inline constexpr hesap::net::BoundedFloat {name} =',
                f'    wire_bounded_format({float_literal(self.low)}, {float_literal(self.high)}, {self.max_code}U);',
                f'static_assert({name}.bits == {self.bits}U);']

    def put(self, ref):
        return (refuse_if(f'!wire_finite({ref})', 'NonFinite') +
                [f'w.put(hesap::net::bounded_encode({self.format_name}, {ref}), {self.bits}U);'])

    def end(self, ref):
        return [f'position += {self.bits}U;']

    def take(self, ref):
        return guard(f'wire_take_bounded(r, {self.format_name}, {ref})')


class QuaternionType(Type):
    lossy = True

    def __init__(self, spec):
        self.bits = spec.get('bits')
        require(isinstance(self.bits, int) and 2 <= self.bits <= 16, f'quaternion takes 2 to 16 bits: {spec}')
        self.min_bits = self.max_bits = 2 + (3 * self.bits)
        self.format_name = None

    def layout(self, minor):
        return ['quaternion', self.bits]

    def cpp(self):
        return 'WireQuaternion'

    def differs(self, ref, default):
        require(default is None, 'a quaternion default is not supported')
        return f'!wire_same({ref}, WireQuaternion{{}})'

    def constants(self, name):
        self.format_name = name
        return [f'inline constexpr hesap::net::SmallestThree {name} = wire_smallest_three_format({self.bits}U);']

    def put(self, ref):
        return (refuse_if(f'!wire_quaternion_finite({ref})', 'NonFinite') +
                [f'wire_put_quaternion(w, {self.format_name}, {ref});'])

    def end(self, ref):
        return [f'position += {self.max_bits}U;']

    def take(self, ref):
        return guard(f'wire_take_quaternion(r, {self.format_name}, {ref})')


class BytesType(Type):
    scalar = False

    def __init__(self, spec):
        if 'size' in spec:
            self.low = self.high = spec['size']
        else:
            self.low = spec.get('min', 0)
            self.high = spec.get('max')
        require(isinstance(self.low, int) and isinstance(self.high, int) and 0 <= self.low <= self.high and
                self.high >= 1, f'bytes needs a size or a max: {spec}')
        self.length_bits = bit_width(self.high - self.low)
        self.min_bits = self.length_bits + (self.low * 8)
        self.max_bits = self.length_bits + 7 + (self.high * 8)

    def layout(self, minor):
        return ['bytes', self.low, self.high]

    def cpp(self):
        return 'cont::ConstSpan<u8>'

    def differs(self, ref, default):
        require(default is None and self.low == 0, 'a field added later must have an empty default')
        return f'!{ref}.empty()'

    def same(self, a, b):
        return f'wire_same_bytes({a}, {b})'

    def ref_default(self, default):
        return b''

    def put(self, ref):
        below = f'{ref}.size() < {self.low}U || ' if self.low else ''
        return (refuse_if(f'{below}{ref}.size() > {self.high}U', 'OutOfRange') +
                [f'wire_put_bytes(w, {ref}, {self.low}U, {self.length_bits}U);'])

    def end(self, ref):
        return [f'position = wire_bytes_end(position, {self.length_bits}U, {ref}.size());']

    def take(self, ref):
        return guard(f'wire_take_bytes(r, {self.low}U, {self.high}U, {self.length_bits}U, {ref})')

    def ref_put(self, writer, value, minor):
        if self.length_bits:
            writer.put(len(value) - self.low, self.length_bits)
        writer.align()
        for byte in value:
            writer.put(byte, 8)

    def ref_take(self, reader, writer_minor):
        length = reader.take(self.length_bits) if self.length_bits else 0
        if length > self.high - self.low:
            raise RefFail('length above the bound')
        reader.align()
        return reader.take_bytes(self.low + length)

    def ref_gen(self, rng, minor):
        size = self.low if self.low == self.high else ref_edge_int(rng, self.low, min(self.high, self.low + 40))
        if self.high <= 400 and rng.random() < 0.2:
            size = rng.randint(self.low, self.high)
        return bytes(rng.randrange(256) for _ in range(size))

    def cpp_assign(self, emit, ref, value):
        if value:
            name = emit.fresh('bytes')
            emit.line(f'static constexpr u8 {name}[] = {{')
            for at in range(0, len(value), 16):
                emit.line('    ' + ', '.join(f'0x{byte:02X}U' for byte in value[at:at + 16]) + ',')
            emit.line('};')
            emit.line(f'{ref} = cont::ConstSpan<u8>({name});')


class TextType(Type):
    scalar = False

    def __init__(self, spec):
        self.high = spec.get('max')
        require(isinstance(self.high, int) and self.high >= 1, f'text needs a max: {spec}')
        self.length_bits = bit_width(self.high)
        self.min_bits = self.length_bits
        self.max_bits = self.length_bits + 7 + (self.high * 8)

    def layout(self, minor):
        return ['text', self.high]

    def cpp(self):
        return 'std::string_view'

    def differs(self, ref, default):
        require(default is None, 'a field added later must have an empty default')
        return f'!{ref}.empty()'

    def same(self, a, b):
        return f'{a} == {b}'

    def ref_default(self, default):
        return b''

    def put(self, ref):
        return (refuse_if(f'{ref}.size() > {self.high}U', 'OutOfRange') +
                refuse_if(f'!wire_text_valid(wire_text_bytes({ref}))', 'BadText') +
                [f'wire_put_bytes(w, wire_text_bytes({ref}), 0U, {self.length_bits}U);'])

    def end(self, ref):
        return [f'position = wire_bytes_end(position, {self.length_bits}U, {ref}.size());']

    def take(self, ref):
        return guard(f'wire_take_text(r, {self.high}U, {self.length_bits}U, {ref})')

    def ref_put(self, writer, value, minor):
        writer.put(len(value), self.length_bits)
        writer.align()
        for byte in value:
            writer.put(byte, 8)

    def ref_take(self, reader, writer_minor):
        length = reader.take(self.length_bits)
        if length > self.high:
            raise RefFail('length above the bound')
        reader.align()
        data = reader.take_bytes(length)
        try:
            data.decode('utf-8')
        except UnicodeDecodeError as error:
            raise RefFail('not UTF-8') from error
        return data

    def ref_gen(self, rng, minor):
        return utf8_text(rng, self.high)

    def cpp_assign(self, emit, ref, value):
        if value:
            text = ''.join(f'\\x{byte:02X}' for byte in value)
            emit.line(f'{ref} = std::string_view("{text}", {len(value)}U);')


class ListType(Type):
    scalar = False

    def __init__(self, spec, parse, owner):
        self.of = parse(spec.get('of'), owner + 'Item')
        self.low = spec.get('min', 0)
        self.high = spec.get('max')
        require(isinstance(self.low, int) and isinstance(self.high, int) and 0 <= self.low <= self.high and
                self.high >= 1, f'list needs a max: {spec}')
        require(self.of.min_bits >= 1, f'list of elements of no bits: {spec}')
        self.count_bits = bit_width(self.high - self.low)
        self.codec_name = None
        self.owner = owner
        self.min_bits = self.count_bits + (self.low * self.of.min_bits)
        self.max_bits = self.count_bits + (self.high * self.of.max_bits)
        self.lossy = self.of.lossy

    def children(self):
        return (self.of,)

    def layout(self, minor):
        return ['list', self.low, self.high, self.of.layout(minor)]

    def cpp(self):
        return f'WireList<{self.codec_name}>'

    def differs(self, ref, default):
        require(default is None and self.low == 0, 'a field added later must have an empty default')
        return f'!{ref}.empty()'

    def same(self, a, b):
        return f'wire_same_list({a}, {b})'

    def ref_default(self, default):
        return []

    def put(self, ref):
        return guard(f'wire_put_list(w, {ref}, {self.low}U, {self.high}U, {self.count_bits}U, minor)')

    def end(self, ref):
        return [f'position = wire_list_end({ref}, {self.count_bits}U, minor, position);']

    def take(self, ref):
        return guard(f'wire_take_list(r, {self.low}U, {self.high}U, {self.count_bits}U, writer_minor, {ref})')

    def nested_codecs(self, emit_codec):
        self.codec_name = emit_codec(self.of, self.owner + 'ItemCodec')

    def ref_put(self, writer, value, minor):
        if self.count_bits:
            writer.put(len(value) - self.low, self.count_bits)
        for item in value:
            self.of.ref_put(writer, item, minor)

    def ref_take(self, reader, writer_minor):
        extra = reader.take(self.count_bits) if self.count_bits else 0
        if extra > self.high - self.low:
            raise RefFail('count above the bound')
        count = self.low + extra
        return [self.of.ref_take(reader, writer_minor) for _ in range(count)]

    def ref_gen(self, rng, minor):
        count = ref_edge_int(rng, self.low, min(self.high, self.low + 4))
        return [self.of.ref_gen(rng, minor) for _ in range(count)]

    def cpp_assign(self, emit, ref, value):
        if value:
            name = emit.fresh('items')
            emit.line(f'{self.codec_name}::Value {name}[{len(value)}]{{}};')
            for index, item in enumerate(value):
                self.of.cpp_assign(emit, f'{name}[{index}]', item)
            emit.line(f'{ref} = {self.cpp()}(cont::ConstSpan<{self.codec_name}::Value>({name}));')


class Field:
    def __init__(self, spec, parse, owner, schema_minor):
        self.name = spec.get('name')
        require(isinstance(self.name, str) and NAME.match(self.name) and self.name not in CPP_KEYWORDS,
                f'{owner}: field name {self.name!r}')
        self.since = spec.get('since', 0)
        require(isinstance(self.since, int) and 0 <= self.since <= schema_minor,
                f'{owner}.{self.name}: since {self.since} past the schema minor {schema_minor}')
        self.optional = bool(spec.get('optional', False))
        self.default = spec.get('default')
        require(not (self.optional and self.default is not None), f'{owner}.{self.name}: optional with a default')
        camel = ''.join(part.capitalize() for part in self.name.split('_'))
        self.type = parse(spec, owner + camel)
        self.format_name = owner + camel
        if self.default is not None:
            self.type.default_literal(self.default)
        if self.since > 0 and not self.optional:
            self.type.differs('x', self.default)

    def min_bits(self):
        if self.since > 0:
            return 0
        return 1 if self.optional else self.type.min_bits

    def max_bits(self):
        return (1 if self.optional else 0) + self.type.max_bits

    def layout(self, minor):
        return [self.since, self.optional, self.default, self.type.layout(minor)]


class StructType(Type):
    scalar = False

    def __init__(self, name, spec, schema):
        require(TYPE_NAME.match(name), f'type name {name!r}')
        self.name = name
        self.schema = schema
        self.spec = spec
        self.extensible = bool(spec.get('extensible', False))
        self.fields = None
        self.resolving = False
        self.is_message = False

    def resolve(self, is_message=False):
        require(not self.resolving, f'{self.name} is recursive')
        if self.fields is not None:
            return
        self.resolving = True
        self.fields = []
        previous = 0
        names = set()
        for spec in self.spec.get('fields', []):
            field = Field(spec, self.schema.parse_type, self.name, self.schema.minor)
            require(field.name not in names, f'{self.name}.{field.name} is a duplicate')
            names.add(field.name)
            require(not ('has_' + field.name) in names, f'{self.name}.{field.name} collides with a presence flag')
            require(field.since >= previous, f'{self.name}.{field.name}: a field added later goes after the others')
            previous = field.since
            if field.since > 0:
                require(is_message or self.extensible,
                        f'{self.name}.{field.name}: only a message or an extensible struct adds fields')
            self.fields.append(field)
        require(self.fields, f'{self.name} has no field')
        self.is_message = is_message
        body_min = sum(f.min_bits() for f in self.fields)
        body_max = sum(f.max_bits() for f in self.fields)
        if self.extensible and not is_message:
            self.min_bits = 8 + ((body_min + 7) // 8 * 8)
            aligned = (body_max + 7) // 8
            self.max_bits = 7 + (((bit_width(aligned) + 6) // 7) * 8) + (aligned * 8)
        else:
            self.min_bits = body_min
            self.max_bits = body_max
        require(self.min_bits >= 1, f'{self.name} can take no bits')
        self.lossy = any(f.type.lossy for f in self.fields)
        self.depth = 1 + max(depth_of(f.type) for f in self.fields)
        require(self.depth <= MAX_DEPTH, f'{self.name} nests deeper than {MAX_DEPTH}')
        self.resolving = False

    def layout(self, minor):
        return ['struct', self.extensible, [f.layout(minor) for f in self.fields if f.since <= minor]]

    def cpp(self):
        return self.name

    def differs(self, ref, default):
        raise SchemaError(f'{self.name}: a struct added later must be optional')

    def same(self, a, b):
        return f'{self.name}Codec::same({a}, {b})'

    def put(self, ref):
        return guard(f'{self.name}Codec::encode({ref}, w, minor)')

    def end(self, ref):
        return [f'position = {self.name}Codec::end({ref}, minor, position);']

    def take(self, ref):
        return guard(f'{self.name}Codec::decode(r, writer_minor, {ref})')

    def ref_default(self, default):
        return {f.name: f.type.ref_default(f.default) if not f.optional else None for f in self.fields}

    def ref_body_put(self, writer, value, minor):
        for field in self.fields:
            if field.since > minor:
                continue
            item = value[field.name]
            if field.optional:
                writer.put(0 if item is None else 1, 1)
                if item is None:
                    continue
            field.type.ref_put(writer, item, minor)

    def ref_body_take(self, reader, writer_minor):
        out = {}
        for field in self.fields:
            if field.since > writer_minor:
                out[field.name] = None if field.optional else field.type.ref_default(field.default)
                continue
            if field.optional and not reader.take(1):
                out[field.name] = None
                continue
            out[field.name] = field.type.ref_take(reader, writer_minor)
        return out

    def ref_put(self, writer, value, minor):
        if not self.extensible:
            self.ref_body_put(writer, value, minor)
            return
        writer.align()
        inner = RefWriter()
        self.ref_body_put(inner, value, minor)
        data = inner.data()
        ref_put_varuint(writer, len(data), 7)
        for byte in data:
            writer.put(byte, 8)

    def ref_take(self, reader, writer_minor):
        if not self.extensible:
            return self.ref_body_take(reader, writer_minor)
        reader.align()
        length = ref_take_varuint(reader, 7, 0xFFFFFFFF)
        inner = RefReader(reader.take_bytes(length))
        out = self.ref_body_take(inner, writer_minor)
        ref_finish(inner, writer_minor, self.schema.minor)
        return out

    def ref_gen(self, rng, minor):
        out = {}
        for field in self.fields:
            if field.since > minor:
                out[field.name] = None if field.optional else field.type.ref_default(field.default)
            elif field.optional and rng.random() < 0.4:
                out[field.name] = None
            else:
                out[field.name] = field.type.ref_gen(rng, minor)
        return out

    def cpp_assign(self, emit, ref, value):
        for field in self.fields:
            item = value[field.name]
            if field.optional:
                if item is None:
                    continue
                emit.line(f'{ref}.has_{field.name} = true;')
            field.type.cpp_assign(emit, f'{ref}.{field.name}', item)


def depth_of(item):
    if isinstance(item, StructType):
        item.resolve()
        return item.depth
    return max([0] + [depth_of(child) for child in item.children()])


def ref_finish(reader, writer_minor, schema_minor):
    if writer_minor > schema_minor:
        return
    reader.align()
    if reader.left() != 0:
        raise RefFail('trailing bytes')


class Schema:
    def __init__(self, document, label):
        require(document.get('format') == FORMAT, f'{label}: format must be {FORMAT}')
        self.label = label
        self.name = document.get('name')
        require(isinstance(self.name, str) and NAME.match(self.name), f'{label}: name')
        self.about = document.get('about', '')
        require(self.about, f'{label}: about says what the schema is for')
        raw = document.get('id', '')
        require(isinstance(raw, str) and re.fullmatch('[0-9a-f]{32}', raw) and int(raw, 16) != 0,
                f'{label}: id must be 32 lowercase hex digits, not zero (authored once, stable across revisions)')
        self.identifier = bytes.fromhex(raw)
        self.kind = document.get('kind')
        require(self.kind in SCOPE_KINDS, f'{label}: kind must be one of {sorted(SCOPE_KINDS)}')
        revision = document.get('revision', {})
        self.major = revision.get('major')
        self.minor = revision.get('minor')
        require(isinstance(self.major, int) and 1 <= self.major < 65536, f'{label}: revision major')
        require(isinstance(self.minor, int) and 0 <= self.minor < 65536, f'{label}: revision minor')
        self.history = document.get('history', {})
        self.parse_operations(document)
        self.parse_channels(document)
        self.enums = {name: EnumType(name, spec, self.minor) for name, spec in document.get('enums', {}).items()}
        self.structs = {}
        for name, spec in document.get('structs', {}).items():
            require(name not in self.enums and name not in self.structs, f'{label}: {name} is a duplicate type')
            self.structs[name] = StructType(name, spec, self)
        self.messages = []
        self.parse_messages(document)
        for item in self.structs.values():
            item.resolve()
        self.check_channel_bounds()
        self.check_history()

    def parse_operations(self, document):
        operations = document.get('operations', {})
        self.operations = {}
        bits = set()
        for name, spec in operations.items():
            require(NAME.match(name), f'{self.label}: operation {name}')
            bit = spec.get('bit')
            require(isinstance(bit, int) and 0 <= bit < 64 and bit not in bits,
                    f'{self.label}: operation {name}: bit {bit}')
            bits.add(bit)
            self.operations[name] = (1 << bit, bool(spec.get('idempotent', False)))
        if self.kind == 'transport':
            require(not self.operations, f'{self.label}: a transport schema runs below S18 and declares no operation')
        else:
            require(self.operations, f'{self.label}: a {self.kind} schema declares its operations')

    def parse_channels(self, document):
        self.channels = []
        names = set()
        for spec in document.get('channels', []):
            name = spec.get('name')
            require(isinstance(name, str) and NAME.match(name) and name not in names, f'{self.label}: channel {name}')
            names.add(name)
            require(spec.get('class') in CHANNEL_CLASSES, f'{self.label}: channel {name}: class')
            for key in ('max_message_bytes', 'messages_per_s', 'bytes_per_s'):
                require(isinstance(spec.get(key), int) and spec[key] > 0, f'{self.label}: channel {name}: {key}')
            require(isinstance(spec.get('priority'), int) and 0 <= spec['priority'] < 256,
                    f'{self.label}: channel {name}: priority')
            require(isinstance(spec.get('padding_block', 0), int) and 0 <= spec.get('padding_block', 0) < 65536,
                    f'{self.label}: channel {name}: padding_block')
            self.channels.append(spec)
        require(self.channels, f'{self.label}: no channel')

    def parse_messages(self, document):
        ids = set()
        for spec in document.get('messages', []):
            name = spec.get('name')
            require(isinstance(name, str) and TYPE_NAME.match(name), f'{self.label}: message {name!r}')
            require(name not in self.structs and name not in self.enums, f'{self.label}: {name} is a duplicate type')
            identifier = spec.get('id')
            require(isinstance(identifier, int) and 1 <= identifier < (1 << 30) and identifier not in ids,
                    f'{self.label}: {name}: id {identifier}')
            ids.add(identifier)
            since = spec.get('since', 0)
            require(isinstance(since, int) and 0 <= since <= self.minor, f'{self.label}: {name}: since {since}')
            channel = [c for c in self.channels if c['name'] == spec.get('channel')]
            require(channel, f'{self.label}: {name}: channel {spec.get("channel")!r}')
            operation = spec.get('operation')
            if self.kind == 'transport':
                require(operation is None, f'{self.label}: {name}: a transport schema declares no operation')
                mask, idempotent = 0, False
            else:
                require(operation in self.operations, f'{self.label}: {name}: operation {operation!r}')
                mask, idempotent = self.operations[operation]
            require(not channel[0].get('idempotent', False) or idempotent or self.kind == 'transport',
                    f'{self.label}: {name}: an idempotent channel carries only idempotent operations')
            item = StructType(name, spec, self)
            item.resolve(is_message=True)
            self.structs[name] = item
            self.messages.append({'name': name, 'id': identifier, 'since': since, 'type': item,
                                  'channel': self.channels.index(channel[0]), 'operation': mask})
        require(self.messages, f'{self.label}: no message')
        self.messages.sort(key=lambda m: m['id'])

    def parse_type(self, spec, owner):
        require(isinstance(spec, dict), f'{self.label}: {owner}: a type is an object')
        kind = spec.get('type')
        if kind == 'bool':
            return BoolType()
        if kind == 'uint':
            return UintType(spec)
        if kind == 'range':
            return RangeType(spec)
        if kind == 'varuint':
            return VaruintType(spec)
        if kind == 'varsint':
            return VarsintType(spec)
        if kind in ('f32', 'f64'):
            return FloatType(32 if kind == 'f32' else 64, spec)
        if kind == 'bounded':
            return BoundedType(spec)
        if kind == 'quaternion':
            return QuaternionType(spec)
        if kind == 'bytes':
            return BytesType(spec)
        if kind == 'text':
            return TextType(spec)
        if kind == 'list':
            return ListType(spec, self.parse_type, owner)
        if kind in self.enums:
            return self.enums[kind]
        if kind in self.structs:
            target = self.structs[kind]
            target.resolve()
            require(not target.is_message, f'{self.label}: {owner}: a message is not a field type')
            return target
        raise SchemaError(f'{self.label}: {owner}: unknown type {kind!r}')

    def check_channel_bounds(self):
        for message in self.messages:
            bytes_at_most = (message['type'].max_bits + 7) // 8
            require(bytes_at_most <= MAX_MESSAGE_BYTES, f'{self.label}: {message["name"]} too large')
            channel = self.channels[message['channel']]
            require(bytes_at_most <= channel['max_message_bytes'],
                    f'{self.label}: {message["name"]} takes up to {bytes_at_most} bytes, above channel '
                    f'{channel["name"]}\'s {channel["max_message_bytes"]}')

    def layout_digest(self, minor):
        layout = [[m['id'], m['type'].layout(minor)] for m in self.messages if m['since'] <= minor]
        text = json.dumps([self.major, layout], sort_keys=True, separators=(',', ':'))
        return hashlib.sha256(text.encode('utf-8')).hexdigest()

    def check_history(self):
        for minor in range(self.minor + 1):
            digest = self.layout_digest(minor)
            recorded = self.history.get(str(minor))
            require(recorded == digest,
                    f'{self.label}: minor {minor}\'s layout is {digest}, its history says {recorded}. A new minor adds '
                    f'its digest; a changed digest of an older minor is an incompatible change: a new major.')
        require(len(self.history) == self.minor + 1, f'{self.label}: history holds a minor past the revision')

    def has_lossy(self):
        return any(item.lossy for item in self.structs.values())

    def ordered_structs(self):
        ordered = []
        seen = set()

        def visit(item):
            if isinstance(item, StructType):
                if item.name in seen:
                    return
                seen.add(item.name)
                for field in item.fields:
                    visit(field.type)
                ordered.append(item)
            else:
                for child in item.children():
                    visit(child)

        for item in self.structs.values():
            visit(item)
        return ordered


# ---- The header ----


def field_format_constants(struct_item):
    lines = []

    def visit(item, name):
        if isinstance(item, (BoundedType, QuaternionType)):
            lines.extend(item.constants('k' + name + 'Format'))
        elif isinstance(item, ListType):
            visit(item.of, name + 'Item')

    for field in struct_item.fields:
        visit(field.type, field.format_name)
    return lines


def scalar_codec(item, name):
    lines = [f'struct {name}', '{', f'    using Value = {item.cpp()};', '']
    lines += ['    [[nodiscard]] static WireError encode(const Value& value, cont::BitWriter& w,',
              '                                          [[maybe_unused]] u16 minor) noexcept', '    {']
    lines += wrap_lines(item.put('value'), '        ') + ['        return WireError::Ok;', '    }', '']
    lines += ['    [[nodiscard]] static u64 end([[maybe_unused]] const Value& value, [[maybe_unused]] u16 minor,',
              '                                 u64 position) noexcept', '    {']
    lines += wrap_lines(item.end('value'), '        ') + ['        return position;', '    }', '']
    lines += ['    [[nodiscard]] static WireError decode(cont::BitReader& r, [[maybe_unused]] u16 writer_minor,',
              '                                          Value& value) noexcept', '    {']
    lines += wrap_lines(item.take('value'), '        ') + ['        return WireError::Ok;', '    }', '']
    lines += ['    [[nodiscard]] static bool same(const Value& a, const Value& b) noexcept', '    {',
              f'        return {item.same("a", "b")};', '    }', '};', '']
    return lines


def struct_lines(item, schema, message):
    lines = field_format_constants(item)
    if lines:
        lines.append('')

    def emit_codec(element, name):
        if isinstance(element, StructType):
            return element.name + 'Codec'
        element.nested_codecs(emit_codec)
        lines.extend(scalar_codec(element, name))
        return name

    for field in item.fields:
        field.type.nested_codecs(emit_codec)

    codec = item.name + 'Codec'
    lines += [f'struct {codec};', '', f'struct {item.name}', '{', f'    using WireCodec = {codec};', '']
    width = max(len(f.type.cpp()) for f in item.fields)
    for field in item.fields:
        if field.optional:
            lines.append(f'    {"bool".ljust(width)} has_{field.name} = false;')
        lines.append(f'    {field.type.cpp().ljust(width)} {field.name}{field.type.member_init(field.default)};')
    lines += ['};', '']

    outer = item.extensible and message is None
    encode_name = 'encode_body' if outer else 'encode'
    end_name = 'body_end' if outer else 'end'
    decode_name = 'decode_body' if outer else 'decode'

    lines += [f'struct {codec}', '{', f'    using Value = {item.name};',
              '    static constexpr u16 kSchemaMinor = kMinor;']
    if message is not None:
        lines += [f'    static constexpr u32 kMessageId = {message["id"]}U;',
                  f'    static constexpr u32 kMinBytes = {(item.min_bits + 7) // 8}U; // at the first minor revision',
                  f'    static constexpr u32 kMaxBytes = {(item.max_bits + 7) // 8}U; // at the current minor revision',
                  f'    static constexpr bool kLossy = {"true" if item.lossy else "false"}; '
                  '// a decoded value may re-encode to other bits']
    lines += ['']

    lines += [f'    [[nodiscard]] static WireError {encode_name}(const Value& value, cont::BitWriter& w,',
              '                                                 [[maybe_unused]] u16 minor) noexcept', '    {']
    body = []
    for field in item.fields:
        ref = f'value.{field.name}'
        inner = []
        if field.optional:
            inner.append(f'w.put(value.has_{field.name} ? 1U : 0U, 1U);')
            inner += [f'if (value.has_{field.name})', '{'] + wrap_lines(field.type.put(ref)) + ['}']
        else:
            inner += field.type.put(ref)
        if field.since > 0:
            differs = f'value.has_{field.name}' if field.optional else field.type.differs(ref, field.default)
            body += [f'if (minor >= {field.since}U)', '{'] + wrap_lines(inner) + ['}',
                                                                                  f'else if ({differs})', '{',
                                                                                  '    return WireError::NotInRevision;',
                                                                                  '}']
        else:
            body += inner
    lines += wrap_lines(body, '        ') + ['        return WireError::Ok;', '    }', '']

    lines += [f'    [[nodiscard]] static u64 {end_name}([[maybe_unused]] const Value& value, [[maybe_unused]] u16 minor,',
              '                                 u64 position) noexcept', '    {']
    body = []
    for field in item.fields:
        ref = f'value.{field.name}'
        inner = []
        if field.optional:
            inner.append('position += 1U;')
            inner += [f'if (value.has_{field.name})', '{'] + wrap_lines(field.type.end(ref)) + ['}']
        else:
            inner += field.type.end(ref)
        if field.since > 0:
            body += [f'if (minor >= {field.since}U)', '{'] + wrap_lines(inner) + ['}']
        else:
            body += inner
    lines += wrap_lines(body, '        ') + ['        return position;', '    }', '']

    lines += [f'    [[nodiscard]] static WireError {decode_name}(cont::BitReader& r, [[maybe_unused]] u16 writer_minor,',
              '                                                 Value& value) noexcept', '    {', '        value = Value{};']
    body = []
    for field in item.fields:
        ref = f'value.{field.name}'
        inner = []
        if field.optional:
            inner += guard(f'wire_take_bool(r, value.has_{field.name})')
            inner += [f'if (value.has_{field.name})', '{'] + wrap_lines(field.type.take(ref)) + ['}']
        else:
            inner += field.type.take(ref)
        if field.since > 0:
            body += [f'if (writer_minor >= {field.since}U)', '{'] + wrap_lines(inner) + ['}']
        else:
            body += inner
    lines += wrap_lines(body, '        ') + ['        return WireError::Ok;', '    }', '']

    if outer:
        lines += ['    [[nodiscard]] static WireError encode(const Value& value, cont::BitWriter& w, u16 minor) noexcept',
                  '    {', f'        return wire_put_extensible<{codec}>(w, value, minor);', '    }', '',
                  '    [[nodiscard]] static u64 end(const Value& value, u16 minor, u64 position) noexcept', '    {',
                  f'        return wire_extensible_end<{codec}>(value, minor, position);', '    }', '',
                  '    [[nodiscard]] static WireError decode(cont::BitReader& r, u16 writer_minor, Value& value) noexcept',
                  '    {', f'        return wire_take_extensible<{codec}>(r, writer_minor, kSchemaMinor, value);', '    }',
                  '']

    terms = []
    for field in item.fields:
        one, other = f'a.{field.name}', f'b.{field.name}'
        if field.optional:
            terms.append(f'a.has_{field.name} == b.has_{field.name}')
            terms.append(f'(!a.has_{field.name} || {field.type.same(one, other)})')
        else:
            terms.append(field.type.same(one, other))
    lines += ['    [[nodiscard]] static bool same(const Value& a, const Value& b) noexcept', '    {']
    lines += [f'        return {terms[0]}' + (' &&' if len(terms) > 1 else ';')]
    for index, term in enumerate(terms[1:], start=2):
        lines.append(f'               {term}' + (' &&' if index < len(terms) else ';'))
    lines += ['    }', '};', '']
    return lines


def header_text(schema, entry):
    lines = ['#pragma once', '', f'// GENERATED by {GENERATOR} from {entry["schema"]}. Do not edit.', '//']
    lines += ['// ' + line for line in textwrap.wrap(schema.about, 110)]
    lines += ['',
              '#include <crd/net/wire_quantized.hpp>' if schema.has_lossy() else '#include <crd/net/wire.hpp>', '',
              f'namespace crd::net::wire::{schema.name}', '{', '']
    lines += [f'inline constexpr u16 kMajor = {schema.major}U;', f'inline constexpr u16 kMinor = {schema.minor}U;', '']
    for enum in schema.enums.values():
        lines += [f'enum class {enum.name} : {enum.cpp_name}', '{']
        lines += [f'    {name} = {number}U,' for name, number, _ in enum.values]
        lines += ['};', '']
        lines += [f'[[nodiscard]] constexpr bool enum_known({enum.name} value, [[maybe_unused]] u16 minor) noexcept',
                  '{', '    switch (value)', '    {']
        for since in sorted({value[2] for value in enum.values}):
            lines += [f'    case {enum.name}::{name}:' for name, _, added in enum.values if added == since]
            lines.append('        return true;' if since == 0 else f'        return minor >= {since}U;')
        lines += ['    default:', '        return false;', '    }', '}', '']
    message_by_type = {m['type'].name: m for m in schema.messages}
    for item in schema.ordered_structs():
        lines += struct_lines(item, schema, message_by_type.get(item.name))

    lines += ['inline constexpr ChannelDesc kChannels[] = {']
    for spec in schema.channels:
        fields = [f'"{spec["name"]}"', f'ChannelClass::{spec["class"]}',
                  'true' if spec.get('reliable') else 'false', 'true' if spec.get('ordered') else 'false',
                  'true' if spec.get('idempotent') else 'false', f'{spec["priority"]}U',
                  f'{spec["max_message_bytes"]}U', f'{spec["messages_per_s"]}U', f'{spec["bytes_per_s"]}ULL',
                  f'{spec.get("padding_block", 0)}U']
        lines += ['    {' + ', '.join(fields[:6]) + ',', '     ' + ', '.join(fields[6:]) + '},']
    lines += ['};', '', 'inline constexpr MessageDesc kMessages[] = {']
    for message in schema.messages:
        item = message['type']
        lines.append(f'    {{"{message["name"]}", {message["id"]}U, {message["channel"]}U, {message["since"]}U, '
                     f'{message["operation"]}ULL, {item.name}Codec::kMinBytes,')
        lines.append(f'     {item.name}Codec::kMaxBytes}},')
    identifier = ', '.join(f'0x{byte:02X}U' for byte in schema.identifier)
    operations = sum(value[0] for value in schema.operations.values())
    idempotent = sum(value[0] for value in schema.operations.values() if value[1])
    lines += ['};', '', 'inline constexpr WireSchema kSchema{', f'    "{schema.name}",', f'    {{{identifier}}},',
              f'    {SCOPE_KINDS[schema.kind]}U, // {schema.kind}', '    kMajor,', '    kMinor,',
              f'    {operations}ULL,', f'    {idempotent}ULL,', '    kChannels,', '    kMessages,', '};', '',
              f'}} // namespace crd::net::wire::{schema.name}', '']
    return '\n'.join(lines)


# ---- The fuzz target ----


def seeds_for(schema):
    rng = random.Random(f'seeds:{schema.name}')
    seeds = []
    for index, message in enumerate(schema.messages):
        item = message['type']
        if item.lossy:
            data = bytes(rng.randrange(256) for _ in range((item.min_bits + 7) // 8 + 8))
            seeds.append((index, schema.minor, data))
            continue
        for minor in sorted({0, schema.minor}):
            if message['since'] > minor:
                continue
            for _ in range(2):
                writer = RefWriter()
                item.ref_body_put(writer, item.ref_gen(rng, minor), minor)
                seeds.append((index, minor, writer.data()))
    return seeds


def fuzz_text(schema, entry):
    namespace = f'crd::net::wire::{schema.name}'
    target = 'crd-fuzz-wire-' + schema.name.replace('_', '-')
    lines = [f'// GENERATED by {GENERATOR} from {entry["schema"]}. Do not edit.', '//',
             f'// {target}: the generated decoder of every message of the schema under the bounded fuzz harness.',
             '//',
             '// Input. The first byte picks the message, the second the writer\'s minor revision, the rest is the message.',
             '//',
             '// Oracles, on every input a decoder accepts:',
             '//   * a walk of every list succeeds (a list view re-reads what the decode validated);',
             '//   * a second decode gives the same message;',
             '//   * up to the schema\'s revision the size lies within the schema\'s bounds, and the message re-encodes to',
             '//     exactly the input (one message has one encoding), unless its fields are quantised, when the re-encoded',
             '//     message decodes at the same size;',
             '//   * encoded_bytes() equals what the encoder wrote.',
             '#include <crd/fuzz/harness.hpp>', '', f'#include <{entry["include"]}>', '', '#include <cstring>', '',
             'namespace', '{', 'using crd::u16;', 'using crd::u8;', 'using crd::usize;',
             'using crd::containers::ConstSpan;', 'using crd::net::wire::WireError;', '',
             f'namespace schema = {namespace};', '',
             'u8 g_out[crd::fuzz::kMaxInputBytes];', '',
             'template <typename M>', 'void exercise(ConstSpan<u8> bytes, u16 writer_minor)', '{',
             '    using C = typename M::WireCodec;', '    M first{};',
             '    if (crd::net::wire::decode(bytes, writer_minor, first) != WireError::Ok)', '    {', '        return;', '    }',
             '    CRD_FUZZ_REQUIRE(crd::net::wire::same(first, first));', '    M second{};',
             '    CRD_FUZZ_REQUIRE(crd::net::wire::decode(bytes, writer_minor, second) == WireError::Ok);',
             '    CRD_FUZZ_REQUIRE(crd::net::wire::same(first, second));',
             '    const bool known = writer_minor <= C::kSchemaMinor;', '    if (known)', '    {',
             '        CRD_FUZZ_REQUIRE(bytes.size() >= C::kMinBytes);', '        CRD_FUZZ_REQUIRE(bytes.size() <= C::kMaxBytes);',
             '    }',
             '    const u16 minor = known ? writer_minor : C::kSchemaMinor;', '    usize written = 0U;',
             '    const WireError error = crd::net::wire::encode(first, minor, crd::containers::Span<u8>(g_out), written);',
             '    if (!known && error == WireError::UnknownValue)', '    {', '        return; // a newer writer\'s enum value', '    }',
             '    CRD_FUZZ_REQUIRE(error == WireError::Ok);',
             '    CRD_FUZZ_REQUIRE(crd::net::wire::encoded_bytes(first, minor) == written);',
             '    if (known)', '    {', '        CRD_FUZZ_REQUIRE(written == bytes.size());',
             '        CRD_FUZZ_REQUIRE(C::kLossy || written == 0U || std::memcmp(g_out, bytes.data(), written) == 0);',
             '    }', '    M again{};',
             '    CRD_FUZZ_REQUIRE(crd::net::wire::decode(ConstSpan<u8>(g_out, written), minor, again) == WireError::Ok);',
             '    CRD_FUZZ_REQUIRE(C::kLossy || crd::net::wire::same(first, again));', '}', '', '} // namespace', '',
             'extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)', '{',
             '    if (size < 2U || size - 2U > crd::fuzz::kMaxInputBytes)', '    {', '        return 0;', '    }',
             '    const ConstSpan<u8> bytes(data + 2U, size - 2U);',
             f'    const auto writer_minor = static_cast<u16>(data[1] % (schema::kMinor + {MINORS_PAST_SCHEMA + 1}U));',
             f'    switch (data[0] % {len(schema.messages)}U)', '    {']
    for index, message in enumerate(schema.messages):
        lines += [f'    case {index}U:', f'        exercise<schema::{message["name"]}>(bytes, writer_minor);', '        break;']
    lines += ['    default:', '        break;', '    }', '    return 0;', '}', '',
              'void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)', '{']
    for number, (index, minor, data) in enumerate(seeds_for(schema)):
        whole = bytes([index, minor]) + data
        lines.append(f'    static constexpr u8 seed_{number}[] = {{')
        for at in range(0, len(whole), 16):
            lines.append('        ' + ', '.join(f'0x{byte:02X}U' for byte in whole[at:at + 16]) + ',')
        lines += ['    };', f'    sink.add("wire_{number}", seed_{number}, sizeof(seed_{number}));']
    lines += ['}', '']
    return '\n'.join(lines)


# ---- The known answers ----


class VectorEmit:
    def __init__(self):
        self.lines = []
        self.counter = 0

    def line(self, text):
        self.lines.append('    ' + text)

    def fresh(self, prefix):
        self.counter += 1
        return f'{prefix}_{self.counter}'


def hex_literal(data):
    text = data.hex().upper()
    return ['    "' + text[at:at + 96] + '"' for at in range(0, len(text), 96)] or ['    ""']


def mutated(rng, data):
    out = bytearray(data)
    choice = rng.randrange(5)
    if choice == 0 and out:
        at = rng.randrange(len(out))
        out[at] ^= 1 << rng.randrange(8)
    elif choice == 1 and out:
        del out[rng.randrange(len(out)):]
    elif choice == 2:
        out += bytes(rng.randrange(256) for _ in range(rng.randrange(1, 4)))
    elif choice == 3 and out:
        at = rng.randrange(len(out))
        out[at] = rng.choice((0x00, 0xFF, 0x80, 0x7F))
    elif out:
        out[-1] |= 1
    return bytes(out)


def ref_verdict(item, data, writer_minor):
    reader = RefReader(data)
    try:
        item.ref_body_take(reader, writer_minor)
        ref_finish(reader, writer_minor, item.schema.minor)
    except RefFail:
        return False
    return True


def vectors_text(schema, entry, values_per_minor=5, mutations_per_value=4):
    rng = random.Random(f'vectors:{schema.name}')
    emit = VectorEmit()
    lines = [f'// GENERATED by {GENERATOR} from {entry["schema"]}. Do not edit.', '//',
             '// Known answers from the generator\'s reference interpreter, which shares nothing with the generated C++:',
             '// check_value builds a message field by field and holds the encoder and decoder to the reference\'s bytes,',
             '// and check_verdict holds the decoder to the reference\'s verdict on damaged bytes.', '']
    for message in schema.messages:
        item = message['type']
        if item.lossy:
            continue
        for minor in range(message['since'], schema.minor + 1):
            for _ in range(values_per_minor):
                value = item.ref_gen(rng, minor)
                writer = RefWriter()
                item.ref_body_put(writer, value, minor)
                data = writer.data()
                emit.lines = []
                emit.line(f'{item.name} message{{}};')
                item.cpp_assign(emit, 'message', value)
                lines += ['{'] + emit.lines + [f'    check_value(message, {minor}U,'] + hex_literal(data)
                lines[-1] += ');'
                lines.append('}')
                for writer_minor in (minor, schema.minor, schema.minor + 1):
                    if writer_minor < minor:
                        continue
                    accepted = ref_verdict(item, data, writer_minor)
                    lines += [f'check_verdict<{item.name}>({writer_minor}U, {"true" if accepted else "false"},']
                    lines += hex_literal(data)
                    lines[-1] += ');'
                for _ in range(mutations_per_value):
                    damaged = mutated(rng, data)
                    writer_minor = rng.choice((minor, schema.minor, schema.minor + 1))
                    accepted = ref_verdict(item, damaged, writer_minor)
                    lines += [f'check_verdict<{item.name}>({writer_minor}U, {"true" if accepted else "false"},']
                    lines += hex_literal(damaged)
                    lines[-1] += ');'
    return '\n'.join(lines) + '\n'


# ---- Running ----


def outputs_for(root, entry):
    document = json.loads((root / entry['schema']).read_text(encoding='utf-8'))
    schema = Schema(document, entry['schema'])
    out = {entry['header']: header_text(schema, entry), entry['fuzz']: fuzz_text(schema, entry)}
    if 'vectors' in entry:
        out[entry['vectors']] = vectors_text(schema, entry)
    for path, text in out.items():
        for number, line in enumerate(text.split('\n'), start=1):
            require(len(line) <= 120 or path.endswith('.inc'), f'{path}:{number} is longer than 120 columns: {line}')
    return schema, out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args(argv)
    drift = []
    identifiers = {}
    try:
        for entry in OUTPUTS:
            schema, outputs = outputs_for(args.root, entry)
            other = identifiers.setdefault(schema.identifier, schema.name)
            require(other == schema.name, f'{schema.name} and {other} share one identifier')
            for relative, text in outputs.items():
                path = args.root / relative
                if args.check:
                    if not path.exists() or path.read_bytes() != text.encode('utf-8'):
                        drift.append(relative)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(text.encode('utf-8'))
    except SchemaError as error:
        print(f'gen-wire-schemas: {error}', file=sys.stderr)
        return 2
    for relative in drift:
        print(f'gen-wire-schemas: {relative} differs from its schema; run python {GENERATOR}', file=sys.stderr)
    return 1 if drift else 0


if __name__ == '__main__':
    sys.exit(main())
