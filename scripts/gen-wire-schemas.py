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
DELTA_HEADER_MAX_BYTES = 9  # crd/net/delta.hpp's kDeltaHeaderMaxBytes
DELTA_EXPANDED_MAX_BYTES = 5  # crd/net/delta.hpp's kDeltaExpandedMaxBytes
MIN_PADDING_BLOCK = 16  # a smaller block hides almost nothing and costs a padding header on most datagrams

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
    {
        'schema': 'tests/foundation/net/schemas/delta_test.json',
        'header': 'tests/foundation/net/generated/delta_test.hpp',
        'include': 'generated/delta_test.hpp',
        'fuzz': 'tests/foundation/net/fuzz/fuzz_wire_delta_test.cpp',
        'vectors': 'tests/foundation/net/generated/delta_test_vectors.inc',
    },
    {
        'schema': 'tests/foundation/net/schemas/coding_test.json',
        'header': 'tests/foundation/net/generated/coding_test.hpp',
        'include': 'generated/coding_test.hpp',
    },
    {
        'schema': 'tests/foundation/net/schemas/serialize_board.json',
        'header': 'tests/foundation/net/generated/serialize_board.hpp',
        'include': 'generated/serialize_board.hpp',
    },
]

SCOPE_KINDS = {'transport': 0, 'World': 1, 'Project': 2, 'Room': 3, 'Upload': 4, 'Tool': 5, 'Service': 6}
CHANNEL_CLASSES = ('Control', 'Input', 'State', 'Event', 'Transaction', 'Bulk', 'Media')
# Control and Transaction traffic mixes credentials and peer-chosen bytes, and a coded size can show how they relate; a
# Media body is already a codec's output (network foundation S32).
CODED_CLASSES = ('Input', 'State', 'Event', 'Bulk')
CODEC_KINDS = {'dictionary': 'Dictionary', 'entropy': 'Entropy'}
MODEL_HASH = re.compile(r'^[0-9a-f]{16}$')
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


def ref_fold(difference, width):
    mask = (1 << width) - 1
    wrapped = difference & mask
    signed = wrapped if wrapped < (1 << (width - 1)) else wrapped - (1 << width)
    return 2 * signed if signed >= 0 else -2 * signed - 1


def ref_unfold(folded):
    return folded // 2 if folded % 2 == 0 else -(folded // 2) - 1


class Type:
    lossy = False
    scalar = True
    diffable = False

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

    def check(self, ref):
        """A list's validating pass: the take's verdict; a quantised field skips the value it would rebuild."""
        return self.take(ref)

    def ref_default(self, default):
        return default if default is not None else 0

    def cpp_assign(self, emit, ref, value):
        emit.line(f'{ref} = {int_literal(value, self.cpp())};')

    def nested_codecs(self, emit_codec):
        """Emit whatever element codecs this type needs before the struct that holds it."""

    def children(self):
        return ()

    def delta_entry(self):
        """The table's view of the type: kind, width, low, its children's table and how many."""
        return 'Bits', self.max_bits, 0, None, 0

    def ref_code(self, value):
        return value

    def ref_from_code(self, code):
        return code

    def ref_same(self, a, b, minor):
        one, other = RefWriter(), RefWriter()
        self.ref_put(one, a, minor)
        self.ref_put(other, b, minor)
        return one.bits == other.bits

    def ref_delta_put(self, writer, base, value, minor, diff):
        width = self.max_bits
        if diff:
            ref_put_varuint(writer, ref_fold(self.ref_code(value) - self.ref_code(base), width), diff)
        elif width != 1:
            writer.put(self.ref_code(value), width)

    def ref_delta_take(self, reader, base, writer_minor, diff):
        width = self.max_bits
        was = self.ref_code(base)
        if diff:
            step = ref_unfold(ref_take_varuint(reader, diff, (1 << width) - 1))
            code = (was + step) & ((1 << width) - 1)
        elif width == 1:
            code = 1 - was
        else:
            code = reader.take(width)
        if code == was:
            raise RefFail('unchanged')
        return self.ref_from_code(code)

    def ref_perturb(self, value, rng, minor):
        return self.ref_gen(rng, minor)


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

    def ref_code(self, value):
        return 1 if value else 0

    def ref_from_code(self, code):
        return code == 1

    def cpp_assign(self, emit, ref, value):
        emit.line(f'{ref} = {"true" if value else "false"};')


class UintType(Type):
    diffable = True

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
    diffable = True

    def ref_code(self, value):
        return value - self.low

    def ref_from_code(self, code):
        return self.low + code

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
    diffable = True

    def delta_entry(self):
        return 'Varuint', self.group, 0, None, 0

    def ref_delta_put(self, writer, base, value, minor, diff):
        if diff:
            ref_put_varuint(writer, ref_fold(value - base, 64), diff)
        else:
            ref_put_varuint(writer, value, self.group)

    def ref_delta_take(self, reader, base, writer_minor, diff):
        if diff:
            value = (base + ref_unfold(ref_take_varuint(reader, diff, (1 << 64) - 1))) & ((1 << 64) - 1)
        else:
            value = ref_take_varuint(reader, self.group, (1 << 64) - 1)
        if value == base:
            raise RefFail('unchanged')
        return value

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
    diffable = True

    def delta_entry(self):
        return 'Varsint', self.group, 0, None, 0

    def ref_delta_put(self, writer, base, value, minor, diff):
        if diff:
            ref_put_varuint(writer, ref_fold(value - base, 64), diff)
        else:
            ref_put_varuint(writer, self.zigzag(value), self.group)

    def ref_delta_take(self, reader, base, writer_minor, diff):
        if diff:
            step = ref_unfold(ref_take_varuint(reader, diff, (1 << 64) - 1))
            value = ((base + step + (1 << 63)) & ((1 << 64) - 1)) - (1 << 63)
        else:
            raw = ref_take_varuint(reader, self.group, (1 << 64) - 1)
            value = (raw >> 1) if not raw & 1 else -(raw >> 1) - 1
        if value == base:
            raise RefFail('unchanged')
        return value

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
    diffable = True

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

    def check(self, ref):
        return guard(f'wire_check_bounded(r, {self.format_name})')


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

    def check(self, ref):
        return guard(f'wire_check_quaternion(r, {self.format_name})')


class WholeInDelta:
    """A type whose change travels as the whole value."""

    def ref_delta_put(self, writer, base, value, minor, diff):
        self.ref_put(writer, value, minor)

    def ref_delta_take(self, reader, base, writer_minor, diff):
        value = self.ref_take(reader, writer_minor)
        if value == base:
            raise RefFail('unchanged')
        return value


class BytesType(WholeInDelta, Type):
    scalar = False

    def delta_entry(self):
        return 'Bytes', self.length_bits, self.low, None, 0

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


class TextType(WholeInDelta, Type):
    scalar = False

    def delta_entry(self):
        return 'Bytes', self.length_bits, 0, None, 0

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
        self.element_diff = spec['of'].get('delta_group', 0)
        require(self.element_diff == 0 or (isinstance(self.element_diff, int) and 1 <= self.element_diff <= 32 and
                                           self.of.diffable),
                f'list element takes no delta_group {self.element_diff}: {spec}')
        self.key = spec.get('delta_key')
        if self.key is not None:
            require(isinstance(self.of, StructType) and not self.of.extensible,
                    f'delta_key needs a list of a plain struct: {spec}')
            self.of.keyed_element = True
            head = self.of.fields[0]
            require(head.name == self.key, f'delta_key {self.key} must be the first field of {self.of.name}')
            require(not head.optional and head.since == 0 and isinstance(head.type, (UintType, RangeType, VaruintType)),
                    f'delta_key {self.key} must be a uint, range or varuint every element has')

    def children(self):
        return (self.of,)

    def layout(self, minor):
        out = ['list', self.low, self.high, self.of.layout(minor)]
        if self.element_diff or self.key is not None:
            out.append([self.element_diff, self.key])
        return out

    def delta_entry(self):
        kind = 'List' if self.key is None else 'KeyedList'
        return kind, self.count_bits, self.low, f'k{self.owner}ItemDelta', 1

    def cpp(self):
        return f'WireList<{self.codec_name}>'

    def differs(self, ref, default):
        require(default is None and self.low == 0, 'a field added later must have an empty default')
        return f'!{ref}.empty()'

    def same(self, a, b):
        return f'wire_same_list({a}, {b})'

    def ref_default(self, default):
        return []

    def key_order(self, ref):
        if self.key is None:
            return []
        return guard(f'wire_keys_ascend({ref}, WireKeyOf<&{self.of.name}::{self.key}>{{}})')

    def put(self, ref):
        return (self.key_order(ref) +
                guard(f'wire_put_list(w, {ref}, {self.low}U, {self.high}U, {self.count_bits}U, minor)'))

    def end(self, ref):
        return [f'position = wire_list_end({ref}, {self.count_bits}U, minor, position);']

    def take(self, ref):
        # The keys are checked on the validating pass itself, so a keyed list is walked once.
        take = f'wire_take_list(r, {self.low}U, {self.high}U, {self.count_bits}U, writer_minor, {ref}'
        if self.key is None:
            return guard(take + ')')
        return ['if (const WireError error =', f'        {take}, WireKeyOf<&{self.of.name}::{self.key}>{{}});',
                '    error != WireError::Ok)', '{', '    return error;', '}']

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
        out = [self.of.ref_take(reader, writer_minor) for _ in range(count)]
        self.ref_check_keys(out)
        return out

    def ref_check_keys(self, items):
        if self.key is not None:
            for one, other in zip(items, items[1:]):
                if not one[self.key] < other[self.key]:
                    raise RefFail('keys out of order')

    def ref_keyed(self, items, rng, minor):
        """`items` with distinct ascending keys and at least the least count."""
        if self.key is None:
            return items
        by_key = {item[self.key]: item for item in items}
        while len(by_key) < self.low:
            item = self.of.ref_gen(rng, minor)
            by_key[item[self.key]] = item
        return [by_key[key] for key in sorted(by_key)][:self.high]

    def ref_gen(self, rng, minor):
        count = ref_edge_int(rng, self.low, min(self.high, self.low + 4))
        return self.ref_keyed([self.of.ref_gen(rng, minor) for _ in range(count)], rng, minor)

    def ref_perturb(self, value, rng, minor):
        out = []
        for item in value:
            choice = rng.random()
            if choice < 0.15:
                continue
            out.append(self.of.ref_perturb(item, rng, minor) if choice < 0.5 else item)
            if self.key is not None and choice >= 0.5 and rng.random() < 0.1:
                out.append(self.of.ref_gen(rng, minor))
        if rng.random() < 0.3:
            out.append(self.of.ref_gen(rng, minor))
        while len(out) < self.low:
            out.append(self.of.ref_gen(rng, minor))
        return self.ref_keyed(out[:self.high], rng, minor)

    def ref_delta_put(self, writer, base, value, minor, diff):
        if self.count_bits:
            writer.put(len(value) - self.low, self.count_bits)
        if self.key is not None:
            self.ref_delta_put_keyed(writer, base, value, minor)
            return
        shared = min(len(base), len(value))
        for one, other in zip(base[:shared], value[:shared]):
            if self.of.ref_same(one, other, minor):
                writer.put(0, 1)
            else:
                writer.put(1, 1)
                self.of.ref_delta_put(writer, one, other, minor, self.element_diff)
        for item in value[shared:]:
            self.of.ref_put(writer, item, minor)

    def ref_delta_put_keyed(self, writer, base, value, minor):
        at_old = at_new = 0
        while at_old < len(base) or at_new < len(value):
            if at_old == len(base):
                self.of.ref_put(writer, value[at_new], minor)
                at_new += 1
            elif at_new == len(value):
                at_old += 1
            elif base[at_old][self.key] == value[at_new][self.key]:
                if self.of.ref_same(base[at_old], value[at_new], minor):
                    writer.put(0, 1)
                else:
                    writer.put(0b10, 2)
                    self.of.ref_delta_fields_put(writer, base[at_old], value[at_new], minor, start=1)
                at_old += 1
                at_new += 1
            elif value[at_new][self.key] < base[at_old][self.key]:
                writer.put(0b110, 3)
                self.of.ref_put(writer, value[at_new], minor)
                at_new += 1
            else:
                writer.put(0b111, 3)
                at_old += 1

    def ref_delta_take(self, reader, base, writer_minor, diff):
        extra = reader.take(self.count_bits) if self.count_bits else 0
        count = self.low + extra
        if extra > self.high - self.low:
            raise RefFail('count above the bound')
        if self.key is not None:
            return self.ref_delta_take_keyed(reader, base, count, writer_minor)
        out = []
        changed = count != len(base)
        for index in range(min(count, len(base))):
            if reader.take(1):
                changed = True
                out.append(self.of.ref_delta_take(reader, base[index], writer_minor, self.element_diff))
            else:
                out.append(base[index])
        for _ in range(count - len(out)):
            out.append(self.of.ref_take(reader, writer_minor))
        if not changed:
            raise RefFail('unchanged')
        return out

    def ref_delta_take_keyed(self, reader, base, count, writer_minor):
        out = []
        at_old = 0
        passed = None
        changed = count != len(base)

        def insert(limit):
            item = self.of.ref_take(reader, writer_minor)
            key = item[self.key]
            if (passed is not None and key <= passed) or (limit is not None and key >= limit):
                raise RefFail('key out of place')
            return item, key

        while at_old < len(base) or len(out) < count:
            if at_old == len(base):
                item, passed = insert(None)
                out.append(item)
                changed = True
                continue
            old_key = base[at_old][self.key]
            if len(out) == count:
                passed = old_key
                at_old += 1
                changed = True
                continue
            if not reader.take(1):
                out.append(base[at_old])
            elif not reader.take(1):
                item, any_field = self.of.ref_delta_fields_take(reader, base[at_old], writer_minor, start=1)
                if not any_field:
                    raise RefFail('unchanged')
                out.append(item)
                changed = True
            elif not reader.take(1):
                item, passed = insert(old_key)
                out.append(item)
                changed = True
                continue
            else:
                changed = True
                passed = old_key
                at_old += 1
                continue
            passed = old_key
            at_old += 1
        if not changed:
            raise RefFail('unchanged')
        return out

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
        self.diff = spec.get('delta_group', 0)
        require(self.diff == 0 or (isinstance(self.diff, int) and 1 <= self.diff <= 32 and self.type.diffable),
                f'{owner}.{self.name}: delta_group {self.diff} on a type that takes none, or outside 1 to 32')
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
        out = [self.since, self.optional, self.default, self.type.layout(minor)]
        return out + [self.diff] if self.diff else out

    def ref_same(self, a, b, minor):
        if a is None or b is None:
            return a is None and b is None
        return self.type.ref_same(a, b, minor)


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
        self.keyed_element = False

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

    def check(self, ref):
        return guard(f'{self.name}Codec::check(r, writer_minor, {ref})')

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

    def delta_entry(self):
        return ('Extensible' if self.extensible else 'Struct'), 0, 0, f'k{self.name}Delta', len(self.fields)

    def ref_perturb(self, value, rng, minor):
        out = dict(value)
        for index, field in enumerate(self.fields):
            if field.since > minor or rng.random() > 0.35 or (index == 0 and self.keyed_element):
                continue
            if field.optional and (value[field.name] is None or rng.random() < 0.3):
                out[field.name] = None if value[field.name] is not None else field.type.ref_gen(rng, minor)
            else:
                out[field.name] = field.type.ref_perturb(value[field.name], rng, minor)
        return out

    def ref_delta_fields_put(self, writer, base, value, minor, start=0):
        for field in self.fields[start:]:
            if field.since > minor:
                continue
            was, now = base[field.name], value[field.name]
            if field.ref_same(was, now, minor):
                writer.put(0, 1)
                continue
            writer.put(1, 1)
            if field.optional:
                writer.put(0 if now is None else 1, 1)
                if now is None:
                    continue
                if was is None:
                    field.type.ref_put(writer, now, minor)
                    continue
            field.type.ref_delta_put(writer, was, now, minor, field.diff)

    def ref_delta_fields_take(self, reader, base, writer_minor, start=0):
        out = dict(base)
        any_field = False
        for field in self.fields[start:]:
            if field.since > writer_minor:
                continue
            was = base[field.name]
            if not reader.take(1):
                continue
            any_field = True
            if field.optional:
                if not reader.take(1):
                    if was is None:
                        raise RefFail('unchanged')
                    out[field.name] = None
                    continue
                if was is None:
                    out[field.name] = field.type.ref_take(reader, writer_minor)
                    continue
            out[field.name] = field.type.ref_delta_take(reader, was, writer_minor, field.diff)
        return out, any_field

    def ref_delta_put(self, writer, base, value, minor, diff):
        if not self.extensible:
            self.ref_delta_fields_put(writer, base, value, minor)
            return
        writer.align()
        inner = RefWriter()
        self.ref_delta_fields_put(inner, base, value, minor)
        data = inner.data()
        ref_put_varuint(writer, len(data), 7)
        for byte in data:
            writer.put(byte, 8)

    def ref_delta_take(self, reader, base, writer_minor, diff):
        if not self.extensible:
            out, any_field = self.ref_delta_fields_take(reader, base, writer_minor)
        else:
            reader.align()
            length = ref_take_varuint(reader, 7, 0xFFFFFFFF)
            inner = RefReader(reader.take_bytes(length))
            out, any_field = self.ref_delta_fields_take(inner, base, writer_minor)
            ref_finish(inner, writer_minor, self.schema.minor)
            if writer_minor > self.schema.minor:
                any_field = True
        if not any_field:
            raise RefFail('unchanged')
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
            block = spec.get('padding_block')
            require(isinstance(block, int) and (block == 0 or MIN_PADDING_BLOCK <= block < 65536),
                    f'{self.label}: channel {name}: padding_block is 0 or from {MIN_PADDING_BLOCK}')
            require(spec.get('integrity', True) is True or (spec['integrity'] is False and spec['class'] == 'Media'),
                    f'{self.label}: channel {name}: integrity is only a Media channel\'s to turn off')
            self.parse_models(name, spec)
            self.channels.append(spec)
        require(self.channels, f'{self.label}: no channel')

    def parse_models(self, name, spec):
        models = spec.get('models', [])
        require(isinstance(models, list), f'{self.label}: channel {name}: models is a list')
        if not models:
            return
        require(self.kind != 'transport', f'{self.label}: channel {name}: a transport schema codes nothing')
        require(spec['class'] in CODED_CLASSES, f'{self.label}: channel {name}: a {spec["class"]} channel codes nothing')
        previous = -1
        for model in models:
            since = model.get('since')
            require(isinstance(since, int) and previous < since <= self.minor,
                    f'{self.label}: channel {name}: a model\'s since ascends and stays within the revision')
            previous = since
            require(model.get('kind') in CODEC_KINDS, f'{self.label}: channel {name}: model kind {model.get("kind")!r}')
            require(isinstance(model.get('hash'), str) and MODEL_HASH.match(model['hash']),
                    f'{self.label}: channel {name}: a model\'s hash is its source\'s XXH3-64 in 16 hex digits')
            ratio = model.get('max_ratio')
            require(isinstance(ratio, int) and 2 <= ratio < 65536,
                    f'{self.label}: channel {name}: a model\'s max_ratio is from 2 to 65535')

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
            delta = spec.get('delta', False)
            require(isinstance(delta, bool), f'{self.label}: {name}: delta is true or false')
            self.messages.append({'name': name, 'id': identifier, 'since': since, 'type': item, 'delta': delta,
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
            channel = self.channels[message['channel']]
            header = DELTA_HEADER_MAX_BYTES + (DELTA_EXPANDED_MAX_BYTES if channel.get('models') else 0)
            bytes_at_most = (message['type'].max_bits + 7) // 8 + (header if message['delta'] else 0)
            require(bytes_at_most <= MAX_MESSAGE_BYTES, f'{self.label}: {message["name"]} too large')
            require(bytes_at_most <= channel['max_message_bytes'],
                    f'{self.label}: {message["name"]} takes up to {bytes_at_most} bytes, above channel '
                    f'{channel["name"]}\'s {channel["max_message_bytes"]}')

    def layout_digest(self, minor):
        layout = [[m['id'], m['type'].layout(minor)] + ([True] if m['delta'] else [])
                  for m in self.messages if m['since'] <= minor]
        # A model is part of its revision: both ends of that revision must hold the same one.
        codecs = [[channel['name'], model['since'], model['kind'], model['hash'], model['max_ratio']]
                  for channel in self.channels for model in channel.get('models', []) if model['since'] <= minor]
        text = json.dumps([self.major, layout] + ([codecs] if codecs else []), sort_keys=True, separators=(',', ':'))
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

    def delta_messages(self):
        return [message for message in self.messages if message['delta']]

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
    body = wrap_lines(item.check('value'), '        ')
    lines += check_lines('check', lambda: body, body != wrap_lines(item.take('value'), '        '),
                         '[[maybe_unused]] ')
    lines += ['    [[nodiscard]] static bool same(const Value& a, const Value& b) noexcept', '    {',
              f'        return {item.same("a", "b")};', '    }', '};', '']
    return lines


def kept_lists(item):
    """The fields of a struct through which a list's decode storage is reached: its lists and the structs holding one."""
    return [f for f in item.fields
            if isinstance(f.type, ListType) or (isinstance(f.type, StructType) and kept_lists(f.type))]


def keep_lines(item):
    """A struct's `keep`: a decode starts from a fresh value that keeps the storage its lists decode into."""
    fields = kept_lists(item)
    if not fields:
        return []
    lines = ['    // The storage its lists decode into, from the value a decode starts over.',
             '    static void keep(Value& fresh, const Value& old) noexcept', '    {']
    for field in fields:
        if isinstance(field.type, ListType):
            lines.append(f'        fresh.{field.name}.into(old.{field.name}.storage());')
        else:
            lines.append(f'        {field.type.name}Codec::keep(fresh.{field.name}, old.{field.name});')
    return lines + ['    }', '']


def check_lines(name, body, checks, unused=''):
    """A codec's `check`, which a list's validating pass runs: its decode's verdict and end, every field but the quantised
    ones written. A codec whose own fields are not quantised checks by decoding."""
    lines = [f'    [[nodiscard]] static WireError {name}(cont::BitReader& r, [[maybe_unused]] u16 writer_minor,',
             f'                                                 {unused if checks else ""}Value& value) noexcept', '    {']
    if not checks:
        return lines + ['        return decode(r, writer_minor, value);', '    }', '']
    return lines + body() + ['        return WireError::Ok;', '    }', '']


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
    lines += [''] + keep_lines(item)

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

    def reads(read):
        body = []
        for field in item.fields:
            ref = f'value.{field.name}'
            inner = []
            if field.optional:
                inner += guard(f'wire_take_bool(r, value.has_{field.name})')
                inner += [f'if (value.has_{field.name})', '{'] + wrap_lines(read(field.type, ref)) + ['}']
            else:
                inner += read(field.type, ref)
            if field.since > 0:
                body += [f'if (writer_minor >= {field.since}U)', '{'] + wrap_lines(inner) + ['}']
            else:
                body += inner
        reset = f'value = wire_fresh<{codec}>(value);' if kept_lists(item) else 'value = Value{};'
        return ['        ' + reset] + wrap_lines(body, '        ')

    lines += [f'    [[nodiscard]] static WireError {decode_name}(cont::BitReader& r, [[maybe_unused]] u16 writer_minor,',
              '                                                 Value& value) noexcept', '    {']
    decode_body = reads(lambda kind, ref: kind.take(ref))
    lines += decode_body + ['        return WireError::Ok;', '    }', '']
    # A message is never a list's element. A check that reads as the decode does is the decode.
    check_body = reads(lambda kind, ref: kind.check(ref))
    checks = message is None and check_body != decode_body
    if message is None and not outer:
        lines += check_lines('check', lambda: check_body, checks)
    elif checks:
        lines += check_lines('check_body', lambda: check_body, checks)

    if outer:
        lines += ['    [[nodiscard]] static WireError encode(const Value& value, cont::BitWriter& w, u16 minor) noexcept',
                  '    {', f'        return wire_put_extensible<{codec}>(w, value, minor);', '    }', '',
                  '    [[nodiscard]] static u64 end(const Value& value, u16 minor, u64 position) noexcept', '    {',
                  f'        return wire_extensible_end<{codec}>(value, minor, position);', '    }', '',
                  '    [[nodiscard]] static WireError decode(cont::BitReader& r, u16 writer_minor, Value& value) noexcept',
                  '    {', f'        return wire_take_extensible<{codec}>(r, writer_minor, kSchemaMinor, value);', '    }',
                  '']
        check = (f'wire_take_extensible<{codec}, &{codec}::check_body>(r, writer_minor, kSchemaMinor, value)'
                 if checks else 'decode(r, writer_minor, value)')
        lines += ['    [[nodiscard]] static WireError check(cont::BitReader& r, u16 writer_minor, Value& value) noexcept',
                  '    {', f'        return {check};', '    }', '']

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


def delta_runs(fields):
    """Each field's run, as (fields, bits): the siblings from it on, added at the same revision, whose encodings always
    take the same bits (no presence bit, no length or count that varies, no alignment). A run unchanged in a delta is
    its fields' zero bits, so the interpreter compares, copies and skips it as words. (0, 0) for a field outside one.
    Only a width the walk reads without looking at the bits counts: a varuint whose maximum fits one group takes one
    group when valid, but a hostile baseline's continuation bit would make the walk read on."""
    def fixed(item, optional):
        kind = item.delta_entry()[0]
        if optional:
            return False
        if kind == 'Struct':
            return all(field.since == 0 and fixed(field.type, field.optional) for field in item.fields)
        if kind == 'List':
            return item.count_bits == 0 and fixed(item.of, False)
        return kind == 'Bits'

    spans = [(0, 0)] * len(fields)
    for index in range(len(fields) - 1, -1, -1):
        item, since, optional = fields[index]
        if not fixed(item, optional):
            continue
        after = spans[index + 1] if index + 1 < len(fields) and fields[index + 1][1] == since else (0, 0)
        spans[index] = (after[0] + 1, after[1] + item.max_bits)
    return spans


def delta_tables(schema):
    """The tables crd/net/delta.hpp's interpreter walks, children first, and each delta message's layout."""
    lines = []
    emitted = set()

    def entry(item, since, optional, diff, run):
        kind, width, low, table, count = item.delta_entry()
        if isinstance(item, StructType):
            table_of(item)
        elif isinstance(item, ListType) and table not in emitted:
            emitted.add(table)
            element = entry(item.of, 0, False, item.element_diff, delta_runs([(item.of, 0, False)])[0])
            lines.extend([f'inline constexpr DeltaField {table}[] = {{', f'    {element},', '};'])
        return (f'{{{table or "nullptr"}, {count}U, {since}U, DeltaKind::{kind}, {"true" if optional else "false"}, '
                f'{width}U, {diff}U, {low}U, {run[1]}U, {run[0]}U}}')

    def table_of(item):
        name = f'k{item.name}Delta'
        if name not in emitted:
            emitted.add(name)
            fields = item.fields
            spans = delta_runs([(field.type, field.since, field.optional) for field in fields])
            rows = [entry(field.type, field.since, field.optional, field.diff, span)
                    for field, span in zip(fields, spans)]
            lines.extend([f'inline constexpr DeltaField {name}[] = {{'] + [f'    {row},' for row in rows] + ['};'])
        return name

    for message in schema.delta_messages():
        item = message['type']
        table = table_of(item)
        lines.extend([f'inline constexpr DeltaLayout k{item.name}Layout{{{table}, {len(item.fields)}U, kMinor,',
                      f'                                  {item.name}Codec::kMaxBytes, delta_validate<{item.name}>}};',
                      ''])
    return lines


def message_bounds(schema, message):
    """A message's frame body bounds: a delta message's body is a header and the smaller of its delta or itself. A
    frame's bounds hold what it decodes to, but a delta codes its own payload, so on a coded channel its body can be as
    small as the coder's output."""
    item = message['type']
    if not message['delta']:
        return f'{item.name}Codec::kMinBytes', f'{item.name}Codec::kMaxBytes'
    if schema.channels[message['channel']].get('models'):
        return 'kDeltaHeaderMinBytes + 1U', f'kDeltaHeaderMaxBytes + kDeltaExpandedMaxBytes + {item.name}Codec::kMaxBytes'
    least_delta = (sum(1 for field in item.fields if field.since == 0) + 7) // 8
    return (f'kDeltaHeaderMinBytes + {min(least_delta, (item.min_bits + 7) // 8)}U',
            f'kDeltaHeaderMaxBytes + {item.name}Codec::kMaxBytes')


def header_text(schema, entry):
    lines = ['#pragma once', '', f'// GENERATED by {GENERATOR} from {entry["schema"]}. Do not edit.', '//']
    lines += ['// ' + line for line in textwrap.wrap(schema.about, 110)]
    has_delta = bool(schema.delta_messages())
    includes = ['#include <crd/net/delta.hpp>'] if has_delta else []
    includes.append('#include <crd/net/wire_quantized.hpp>' if schema.has_lossy() else '#include <crd/net/wire.hpp>')
    lines += ['', *includes, '']
    lines += [f'namespace crd::net::wire::{schema.name}', '{', '']
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

    for index, spec in enumerate(schema.channels):
        if spec.get('models'):
            lines += [f'inline constexpr CodecDesc kChannel{index}Codecs[] = {{']
            lines += [f'    {{{model["since"]}U, CodecKind::{CODEC_KINDS[model["kind"]]}, {model["max_ratio"]}U, '
                      f'0x{model["hash"]}ULL}},' for model in spec['models']]
            lines += ['};', '']
    lines += ['inline constexpr ChannelDesc kChannels[] = {']
    for index, spec in enumerate(schema.channels):
        fields = [f'"{spec["name"]}"', f'ChannelClass::{spec["class"]}',
                  'true' if spec.get('reliable') else 'false', 'true' if spec.get('ordered') else 'false',
                  'true' if spec.get('idempotent') else 'false', f'{spec["priority"]}U',
                  f'{spec["max_message_bytes"]}U', f'{spec["messages_per_s"]}U', f'{spec["bytes_per_s"]}ULL',
                  f'{spec["padding_block"]}U', 'true' if spec.get('integrity', True) else 'false']
        codecs = [f'kChannel{index}Codecs, {len(spec["models"])}U'] if spec.get('models') else []
        lines += ['    {' + ', '.join(fields[:6]) + ',', '     ' + ', '.join(fields[6:] + codecs) + '},']
    lines += ['};', '']
    lines += delta_tables(schema)
    lines += ['inline constexpr MessageDesc kMessages[] = {']
    for message in schema.messages:
        item = message['type']
        least, most = message_bounds(schema, message)
        layout = f'&k{item.name}Layout' if message['delta'] else 'nullptr'
        lines.append(f'    {{"{message["name"]}", {message["id"]}U, {message["channel"]}U, {message["since"]}U, '
                     f'{message["operation"]}ULL, {least},')
        lines.append(f'     {most}, {layout}}},')
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
    for index, message in enumerate(schema.delta_messages(), start=len(schema.messages)):
        item = message['type']
        if item.lossy:
            data = bytes(rng.randrange(256) for _ in range(40))
            seeds.append((index, schema.minor, bytes([0, 20]) + data))
            continue
        for minor in sorted({message['since'], schema.minor}):
            for _ in range(3):
                base = item.ref_gen(rng, minor)
                value = item.ref_perturb(base, rng, minor)
                old, delta, whole = RefWriter(), RefWriter(), RefWriter()
                item.ref_body_put(old, base, minor)
                item.ref_delta_fields_put(delta, base, value, minor)
                item.ref_body_put(whole, value, minor)
                size = len(old.data())
                seeds.append((index, minor, bytes([size >> 8, size & 255]) + old.data() + delta.data()))
                seeds.append((index, minor, bytes([size >> 8, size & 255]) + old.data() + whole.data()))
    first = len(schema.messages) + len(schema.delta_messages())
    for index, (_, element) in enumerate(list_codecs(schema), start=first):
        if element.lossy:
            seeds.append((index, schema.minor, bytes(rng.randrange(256) for _ in range((element.min_bits + 7) // 8 + 8))))
            continue
        for _ in range(2):
            writer = RefWriter()
            element.ref_put(writer, element.ref_gen(rng, schema.minor), schema.minor)
            seeds.append((index, schema.minor, writer.data()))
    return seeds


def delta_fuzz_lines(schema):
    if not schema.delta_messages():
        return []
    return ['namespace', '{', 'u8 g_rebuilt[crd::fuzz::kMaxInputBytes + crd::net::wire::kDeltaSlackBytes];',
            'u8 g_delta[crd::fuzz::kMaxInputBytes + crd::net::wire::kDeltaSlackBytes];', '',
            'template <typename M>',
            'void exercise_delta(const crd::net::wire::DeltaLayout& layout, ConstSpan<u8> bytes, u16 writer_minor)', '{',
            '    namespace wire = crd::net::wire;', '    if (bytes.size() < 2U)', '    {', '        return;', '    }',
            '    const usize size = (static_cast<usize>(bytes[0]) << 8U) | bytes[1];',
            '    if (size > bytes.size() - 2U)', '    {', '        return;', '    }',
            '    const ConstSpan<u8> baseline = bytes.subspan(2U, size);',
            '    const ConstSpan<u8> rest = bytes.subspan(2U + size);', '    M message{};',
            '    if (wire::decode(baseline, writer_minor, message) != WireError::Ok)', '    {', '        return;', '    }',
            '    usize rebuilt = 0U;',
            '    if (wire::delta_apply(layout, baseline, rest, writer_minor, g_rebuilt, rebuilt) == WireError::Ok &&',
            '        wire::decode(ConstSpan<u8>(g_rebuilt, rebuilt), writer_minor, message) == WireError::Ok &&',
            '        writer_minor <= layout.schema_minor)', '    {', '        usize written = 0U;',
            '        const ConstSpan<u8> whole(g_rebuilt, rebuilt);',
            '        CRD_FUZZ_REQUIRE(wire::delta_encode(layout, baseline, whole, writer_minor, g_delta, written) ==',
            '                         WireError::Ok);', '        CRD_FUZZ_REQUIRE(written == rest.size());',
            '        CRD_FUZZ_REQUIRE(written == 0U || std::memcmp(g_delta, rest.data(), written) == 0);', '    }',
            '    if (writer_minor > layout.schema_minor ||',
            '        wire::decode(rest, writer_minor, message) != WireError::Ok)', '    {', '        return;', '    }',
            '    usize written = 0U;',
            '    CRD_FUZZ_REQUIRE(wire::delta_encode(layout, baseline, rest, writer_minor, g_delta, written) == WireError::Ok);',
            '    const ConstSpan<u8> delta(g_delta, written);',
            '    CRD_FUZZ_REQUIRE(wire::delta_apply(layout, baseline, delta, writer_minor, g_rebuilt, rebuilt) == WireError::Ok);',
            '    CRD_FUZZ_REQUIRE(rebuilt == rest.size());',
            '    CRD_FUZZ_REQUIRE(rebuilt == 0U || std::memcmp(g_rebuilt, rest.data(), rebuilt) == 0);', '}', '',
            '} // namespace', '']


def list_codecs(schema):
    """The codec of every list's element, which the list's validating pass checks, in a stable order."""
    codecs = []

    def visit(item):
        if isinstance(item, ListType) and all(name != item.codec_name for name, _ in codecs):
            codecs.append((item.codec_name, item.of))
        for child in item.children():
            visit(child)

    for item in schema.ordered_structs():
        for field in item.fields:
            visit(field.type)
    return codecs


FUZZ_STORAGE_CAP = 64


def storage_lines(schema):
    """Storage for every message's own lists, up to FUZZ_STORAGE_CAP elements, and the overloads that attach it."""
    lines = []
    for message in schema.messages:
        lists = [f for f in message['type'].fields if isinstance(f.type, ListType)]
        if not lists:
            continue
        lines += [f'void attach(schema::{message["name"]}& message)', '{']
        for field in lists:
            size = min(field.type.high, FUZZ_STORAGE_CAP)
            lines += [f'    static schema::{field.type.codec_name}::Value s_{field.name}[{size}];',
                      f'    message.{field.name}.into(s_{field.name});']
        lines += ['}', '']
    if lines:
        lines = ['template <typename M>', 'void attach(M&)', '{', '}', ''] + lines
    return lines


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
             *(["//   * a decode into storage for the message's lists gives the same verdict and, accepted, the same",
                '//     message.'] if storage_lines(schema) else []),
             *(['//',
                '// A delta message also takes [baseline size, 16 bits][baseline][delta]. On a baseline its decoder accepts:',
                '//   * what a delta rebuilds passes the decoder or is refused, and its delta at that revision is the input',
                '//     (one delta has one encoding);',
                '//   * the rest read as a second message: its delta rebuilds it exactly.'] if schema.delta_messages() else []),
             *(['//',
                "// The last cases pick a list's element codec: its check, which the list's validating pass runs, gives",
                "// its decode's verdict on the same bytes and stops where it does."]
               if list_codecs(schema) else []),
             '#include <crd/fuzz/harness.hpp>', '', f'#include <{entry["include"]}>', '', '#include <cstring>', '',
             'namespace', '{', 'using crd::u16;', 'using crd::u8;', 'using crd::usize;',
             'using crd::containers::ConstSpan;', 'using crd::net::wire::WireError;', '',
             f'namespace schema = {namespace};', '',
             'u8 g_out[crd::fuzz::kMaxInputBytes];', '',
             *storage_lines(schema),
             'template <typename M>', 'void exercise(ConstSpan<u8> bytes, u16 writer_minor)', '{',
             '    using C = typename M::WireCodec;', '    M first{};',
             '    const WireError verdict = crd::net::wire::decode(bytes, writer_minor, first);',
             *(['    M held{};', '    attach(held);',
                '    CRD_FUZZ_REQUIRE(crd::net::wire::decode(bytes, writer_minor, held) == verdict);',
                '    CRD_FUZZ_REQUIRE(verdict != WireError::Ok || crd::net::wire::same(first, held));']
               if storage_lines(schema) else []),
             '    if (verdict != WireError::Ok)', '    {', '        return;', '    }',
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
             '    CRD_FUZZ_REQUIRE(C::kLossy || crd::net::wire::same(first, again));', '}', '',
             *(['template <typename C>', 'void agree(ConstSpan<u8> bytes, u16 writer_minor)', '{',
                '    crd::containers::BitReader decoded(bytes);', '    crd::containers::BitReader checked(bytes);',
                '    typename C::Value one{};', '    typename C::Value other{};',
                '    const WireError verdict = C::decode(decoded, writer_minor, one);',
                '    CRD_FUZZ_REQUIRE(C::check(checked, writer_minor, other) == verdict);',
                '    CRD_FUZZ_REQUIRE(verdict != WireError::Ok || decoded.bit_position() == checked.bit_position());',
                '}', ''] if list_codecs(schema) else []),
             '} // namespace', '',
             *delta_fuzz_lines(schema),
             'extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)', '{',
             '    if (size < 2U || size - 2U > crd::fuzz::kMaxInputBytes)', '    {', '        return 0;', '    }',
             '    const ConstSpan<u8> bytes(data + 2U, size - 2U);',
             f'    const auto writer_minor = static_cast<u16>(data[1] % (schema::kMinor + {MINORS_PAST_SCHEMA + 1}U));',
             f'    switch (data[0] % {len(schema.messages) + len(schema.delta_messages()) + len(list_codecs(schema))}U)',
             '    {']
    for index, message in enumerate(schema.messages):
        lines += [f'    case {index}U:', f'        exercise<schema::{message["name"]}>(bytes, writer_minor);', '        break;']
    for index, message in enumerate(schema.delta_messages(), start=len(schema.messages)):
        name = message['name']
        lines += [f'    case {index}U:', f'        exercise_delta<schema::{name}>(schema::k{name}Layout, bytes, writer_minor);',
                  '        break;']
    first = len(schema.messages) + len(schema.delta_messages())
    for index, (codec, _) in enumerate(list_codecs(schema), start=first):
        lines += [f'    case {index}U:', f'        agree<schema::{codec}>(bytes, writer_minor);', '        break;']
    lines += ['    default:', '        break;', '    }', '    return 0;', '}', '',
              'void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)', '{']
    for number, (index, minor, data) in enumerate(seeds_for(schema)):
        whole = bytes([index, minor]) + data
        lines.append(f'    static constexpr u8 kSeed{number}[] = {{')
        for at in range(0, len(whole), 16):
            lines.append('        ' + ', '.join(f'0x{byte:02X}U' for byte in whole[at:at + 16]) + ',')
        lines += ['    };', f'    sink.add("wire_{number}", kSeed{number}, sizeof(kSeed{number}));']
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


def ref_delta_verdict(item, base, delta, writer_minor):
    """The whole message a reference reader rebuilds, or None when it refuses."""
    reader = RefReader(delta)
    try:
        value, _ = item.ref_delta_fields_take(reader, base, writer_minor)
        ref_finish(reader, writer_minor, item.schema.minor)
        writer = RefWriter()
        item.ref_body_put(writer, value, writer_minor)
        data = writer.data()
    except RefFail:
        return None
    return data if ref_verdict(item, data, writer_minor) else None


def delta_vectors(schema, rng, values_per_minor=8, mutations_per_value=5):
    lines = []
    for message in schema.delta_messages():
        item = message['type']
        if item.lossy:
            continue
        for minor in range(message['since'], schema.minor + 1):
            for round_number in range(values_per_minor):
                base = item.ref_gen(rng, minor)
                value = base if round_number == 0 else item.ref_perturb(base, rng, minor)
                old, new, delta = RefWriter(), RefWriter(), RefWriter()
                item.ref_body_put(old, base, minor)
                item.ref_body_put(new, value, minor)
                item.ref_delta_fields_put(delta, base, value, minor)
                lines += [f'check_delta<{item.name}>({minor}U,'] + hex_literal(old.data())
                lines[-1] += ','
                lines += hex_literal(new.data())
                lines[-1] += ','
                lines += hex_literal(delta.data())
                lines[-1] += ');'
                for _ in range(mutations_per_value):
                    damaged = mutated(rng, delta.data())
                    writer_minor = schema.minor + 1 if minor == schema.minor and rng.random() < 0.3 else minor
                    rebuilt = ref_delta_verdict(item, base, damaged, writer_minor)
                    lines += [f'check_delta_verdict<{item.name}>({writer_minor}U,'] + hex_literal(old.data())
                    lines[-1] += ','
                    lines += hex_literal(damaged)
                    lines[-1] += ','
                    lines += [f'    {"true" if rebuilt is not None else "false"},'] + hex_literal(rebuilt or b'')
                    lines[-1] += ');'
    return lines


def vectors_text(schema, entry, values_per_minor=5, mutations_per_value=4):
    rng = random.Random(f'vectors:{schema.name}')
    emit = VectorEmit()
    lines = [f'// GENERATED by {GENERATOR} from {entry["schema"]}. Do not edit.', '//',
             '// Known answers from the generator\'s reference interpreter, which shares nothing with the generated C++:',
             '// check_value builds a message field by field and holds the encoder and decoder to the reference\'s bytes,',
             '// and check_verdict holds the decoder to the reference\'s verdict on damaged bytes; check_delta and',
             '// check_delta_verdict do the same for baseline deltas.', '']
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
    lines += delta_vectors(schema, rng)
    return '\n'.join(lines) + '\n'


# ---- Running ----


def outputs_for(root, entry):
    document = json.loads((root / entry['schema']).read_text(encoding='utf-8'))
    schema = Schema(document, entry['schema'])
    out = {entry['header']: header_text(schema, entry)}
    if 'fuzz' in entry:
        out[entry['fuzz']] = fuzz_text(schema, entry)
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
