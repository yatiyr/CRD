#!/usr/bin/env python3
"""What scripts/gen-wire-schemas.py refuses and what it computes (NET.3): every schema error is caught before any code
exists, and the sizes a schema publishes are the reference writer's. No build. Run: python scripts/test-wire-schemas.py.
"""
from pathlib import Path
import copy
import importlib.util
import random
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location('gen_wire_schemas', ROOT / 'scripts' / 'gen-wire-schemas.py')
gen = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gen)


def base():
    return {
        'format': gen.FORMAT,
        'name': 'sample',
        'about': 'A schema for these tests.',
        'id': '0123456789abcdef0123456789abcdef',
        'kind': 'transport',
        'revision': {'major': 1, 'minor': 0},
        'channels': [{'name': 'one', 'class': 'Control', 'priority': 0, 'max_message_bytes': 100000,
                      'messages_per_s': 10, 'bytes_per_s': 100}],
        'messages': [{'name': 'Only', 'id': 1, 'channel': 'one', 'fields': [{'name': 'a', 'type': 'uint', 'bits': 3}]}],
    }


def build(document):
    """A schema from `document`, its history taken from its own layout unless the document states one."""
    original = gen.Schema.check_history
    if 'history' not in document:
        gen.Schema.check_history = lambda self: None
        try:
            schema = gen.Schema(copy.deepcopy(document), 'test')
        finally:
            gen.Schema.check_history = original
        document = dict(document, history={str(m): schema.layout_digest(m) for m in range(schema.minor + 1)})
    return gen.Schema(copy.deepcopy(document), 'test')


class SchemaCase(unittest.TestCase):
    def refused(self, document, words):
        with self.assertRaises(gen.SchemaError) as caught:
            build(document)
        self.assertIn(words, str(caught.exception))

    def with_fields(self, *fields):
        document = base()
        document['messages'][0]['fields'] = list(fields)
        return document


class Refusals(SchemaCase):

    def test_the_base_schema_builds(self):
        schema = build(base())
        self.assertEqual(schema.messages[0]['type'].max_bits, 3)
        self.assertIn('struct OnlyCodec', gen.header_text(schema, {'schema': 'x'}))

    def test_format_name_and_identifier(self):
        for key, value, words in (('format', 'other/1', 'format'), ('name', 'Bad', 'name'),
                                  ('id', '00' * 16, 'id must be'), ('id', 'abc', 'id must be'),
                                  ('id', 'AB' * 16, 'id must be'), ('about', '', 'about')):
            self.refused(dict(base(), **{key: value}), words)

    def test_kind_and_operations(self):
        self.refused(dict(base(), kind='Nowhere'), 'kind')
        self.refused(dict(base(), kind='World'), 'declares its operations')
        self.refused(dict(base(), operations={'x': {'bit': 0}}), 'declares no operation')
        document = dict(base(), kind='World', operations={'x': {'bit': 64}})
        self.refused(document, 'bit 64')
        document = dict(base(), kind='World', operations={'x': {'bit': 1}, 'y': {'bit': 1}})
        self.refused(document, 'bit 1')
        document = dict(base(), kind='World', operations={'x': {'bit': 1}})
        self.refused(document, "operation None")

    def test_an_idempotent_channel_carries_idempotent_operations(self):
        document = dict(base(), kind='World', operations={'x': {'bit': 1}})
        document['channels'][0]['idempotent'] = True
        document['messages'][0]['operation'] = 'x'
        self.refused(document, 'idempotent')

    def test_channels(self):
        self.refused(dict(base(), channels=[]), 'no channel')
        for key, value, words in (('class', 'Nope', 'class'), ('max_message_bytes', 0, 'max_message_bytes'),
                                  ('messages_per_s', 0, 'messages_per_s'), ('bytes_per_s', -1, 'bytes_per_s'),
                                  ('priority', 256, 'priority'), ('padding_block', 70000, 'padding_block')):
            document = base()
            document['channels'][0][key] = value
            self.refused(document, words)

    def test_message_ids_names_and_channel(self):
        document = base()
        document['messages'].append(copy.deepcopy(document['messages'][0]))
        document['messages'][1]['name'] = 'Other'
        self.refused(document, 'id 1')
        document = base()
        document['messages'][0]['channel'] = 'none'
        self.refused(document, 'channel')
        document = base()
        document['messages'][0]['since'] = 1
        self.refused(document, 'since')
        self.refused(dict(base(), messages=[]), 'no message')

    def test_a_message_above_its_channel(self):
        document = self.with_fields({'name': 'a', 'type': 'bytes', 'max': 200000})
        self.refused(document, 'above channel')

    def test_field_names(self):
        for name in ('Bad', 'major', 'minor', 'value', 'class'):
            self.refused(self.with_fields({'name': name, 'type': 'bool'}), 'field name')
        self.refused(self.with_fields({'name': 'a', 'type': 'bool'}, {'name': 'a', 'type': 'bool'}), 'duplicate')

    def test_every_type_bound(self):
        for spec, words in (({'type': 'uint', 'bits': 0}, 'uint bits'), ({'type': 'uint', 'bits': 65}, 'uint bits'),
                            ({'type': 'range', 'min': 3, 'max': 3}, 'min < max'),
                            ({'type': 'range', 'min': -1, 'max': 1 << 64}, 'wider than 64'),
                            ({'type': 'varuint'}, 'needs a max'), ({'type': 'varuint', 'max': 0}, 'needs a max'),
                            ({'type': 'varuint', 'max': 5, 'group': 33}, 'group'),
                            ({'type': 'varsint', 'min': 0, 'max': 5}, 'min < 0'),
                            ({'type': 'varsint', 'min': -1, 'max': 1 << 63}, 'wider than 64'),
                            ({'type': 'bytes'}, 'size or a max'), ({'type': 'bytes', 'max': 0}, 'size or a max'),
                            ({'type': 'bytes', 'min': 5, 'max': 2}, 'size or a max'),
                            ({'type': 'text'}, 'text needs a max'),
                            ({'type': 'list', 'of': {'type': 'bool'}}, 'list needs a max'),
                            ({'type': 'bounded', 'min': 1.0, 'max': 0.0, 'bits': 3}, 'min < max'),
                            ({'type': 'bounded', 'min': 0.0, 'max': 1.0, 'bits': 25}, 'at most 24'),
                            ({'type': 'bounded', 'min': 0.0, 'max': 1.0}, 'bits or a resolution'),
                            ({'type': 'bounded', 'min': 0.0, 'max': 1.0, 'bits': 3, 'resolution': 0.1}, 'not both'),
                            ({'type': 'quaternion', 'bits': 1}, '2 to 16'),
                            ({'type': 'quaternion', 'bits': 17}, '2 to 16'), ({'type': 'nope'}, 'unknown type')):
            self.refused(self.with_fields(dict(spec, name='a')), words)

    def test_defaults(self):
        self.refused(self.with_fields({'name': 'a', 'type': 'uint', 'bits': 2, 'default': 4}), 'default')
        self.refused(self.with_fields({'name': 'a', 'type': 'range', 'min': 1, 'max': 3, 'default': 0}), 'default')
        self.refused(self.with_fields({'name': 'a', 'type': 'bool', 'default': 1}), 'default')
        self.refused(self.with_fields({'name': 'a', 'type': 'bool', 'optional': True, 'default': True}), 'optional')
        self.refused(self.with_fields({'name': 'a', 'type': 'bytes', 'max': 3, 'default': 1}), 'takes no default')

    def test_enums(self):
        def with_enum(spec):
            document = self.with_fields({'name': 'a', 'type': 'Choice'})
            document['enums'] = {'Choice': spec}
            return document

        self.refused(with_enum({'bits': 0, 'values': [{'name': 'A', 'value': 0}]}), 'bits')
        self.refused(with_enum({'bits': 1, 'values': []}), 'no value')
        self.refused(with_enum({'bits': 1, 'values': [{'name': 'A', 'value': 2}]}), 'does not fit')
        self.refused(with_enum({'bits': 2, 'values': [{'name': 'A', 'value': 1}, {'name': 'B', 'value': 1}]}),
                     'duplicate')
        self.refused(with_enum({'bits': 2, 'values': [{'name': 'A', 'value': 1, 'since': 1}]}), 'since')
        document = with_enum({'bits': 2, 'values': [{'name': 'A', 'value': 1}]})
        document['messages'][0]['fields'][0]['default'] = 'B'
        self.refused(document, 'has no value B')

    def test_recursion_and_depth(self):
        document = self.with_fields({'name': 'a', 'type': 'Loop'})
        document['structs'] = {'Loop': {'fields': [{'name': 'next', 'type': 'Loop', 'optional': True}]}}
        self.refused(document, 'recursive')
        document = self.with_fields({'name': 'a', 'type': 'Loop'})
        document['structs'] = {
            'Loop': {'fields': [{'name': 'next', 'type': 'list', 'max': 2, 'of': {'type': 'Other'}}]},
            'Other': {'fields': [{'name': 'back', 'type': 'Loop', 'optional': True}]},
        }
        with self.assertRaises(gen.SchemaError):
            build(document)
        document = self.with_fields({'name': 'a', 'type': 'S0'})
        document['structs'] = {f'S{i}': {'fields': [{'name': 'b', 'type': f'S{i + 1}'}]} for i in range(9)}
        document['structs']['S9'] = {'fields': [{'name': 'b', 'type': 'bool'}]}
        document['structs'] = dict(sorted(document['structs'].items(), reverse=True))
        self.refused(document, 'nests deeper')

    def test_no_empty_struct_and_no_message_as_a_field(self):
        document = self.with_fields({'name': 'a', 'type': 'Empty'})
        document['structs'] = {'Empty': {'fields': []}}
        self.refused(document, 'has no field')
        document = base()
        document['messages'].append({'name': 'Two', 'id': 2, 'channel': 'one',
                                     'fields': [{'name': 'a', 'type': 'Only'}]})
        self.refused(document, 'a message is not a field type')

    def test_a_list_of_nothing_is_impossible(self):
        document = self.with_fields({'name': 'a', 'type': 'list', 'max': 9, 'of': {'type': 'list', 'max': 2,
                                                                                    'of': {'type': 'bool'}}})
        self.assertEqual(build(document).messages[0]['type'].min_bits, 4)
        # An element that may take no bits at all cannot be declared: a list's count takes bits unless its size is
        # fixed, and a fixed size of zero is not a list.
        self.refused(self.with_fields({'name': 'a', 'type': 'list', 'min': 0, 'max': 0, 'of': {'type': 'bool'}}),
                     'needs a max')

    def test_revision_rules(self):
        document = base()
        document['revision']['minor'] = 1
        document['messages'][0]['fields'].append({'name': 'b', 'type': 'uint', 'bits': 2, 'since': 2})
        self.refused(document, 'since 2')

        document = base()
        document['revision']['minor'] = 1
        document['messages'][0]['fields'].insert(0, {'name': 'b', 'type': 'uint', 'bits': 2, 'since': 1})
        self.refused(document, 'goes after the others')

        document = self.with_fields({'name': 'a', 'type': 'Fixed'})
        document['revision']['minor'] = 1
        document['structs'] = {'Fixed': {'fields': [{'name': 'a', 'type': 'bool'},
                                                    {'name': 'b', 'type': 'bool', 'since': 1}]}}
        self.refused(document, 'only a message or an extensible struct')

        document = self.with_fields({'name': 'a', 'type': 'bool'}, {'name': 'b', 'type': 'Later', 'since': 1})
        document['revision']['minor'] = 1
        document['structs'] = {'Later': {'fields': [{'name': 'x', 'type': 'bool'}]}}
        self.refused(document, 'must be optional')

        document = self.with_fields({'name': 'a', 'type': 'bool'},
                                    {'name': 'b', 'type': 'bytes', 'size': 4, 'since': 1})
        document['revision']['minor'] = 1
        self.refused(document, 'empty default')

    def test_history_holds_every_recorded_layout(self):
        schema = build(base())
        document = dict(base(), history={'0': schema.layout_digest(0)})
        self.assertEqual(build(document).minor, 0)

        # Any change to the layout of a recorded revision is refused: a width, an order, a bound, a default.
        for field in ({'name': 'a', 'type': 'uint', 'bits': 4},
                      {'name': 'a', 'type': 'range', 'min': 0, 'max': 7},
                      {'name': 'a', 'type': 'uint', 'bits': 3, 'default': 1}):
            changed = dict(self.with_fields(field), history={'0': schema.layout_digest(0)})
            self.refused(changed, 'history says')

        # A rename is not a layout change.
        renamed = dict(self.with_fields({'name': 'renamed', 'type': 'uint', 'bits': 3}),
                       history={'0': schema.layout_digest(0)})
        self.assertEqual(build(renamed).minor, 0)

        # A minor appends, keeps minor 0's digest and adds its own; a new major starts over.
        appended = self.with_fields({'name': 'a', 'type': 'uint', 'bits': 3},
                                    {'name': 'b', 'type': 'uint', 'bits': 2, 'since': 1})
        appended['revision']['minor'] = 1
        self.refused(dict(appended, history={'0': schema.layout_digest(0)}), 'history says')
        self.assertEqual(build(appended).layout_digest(0), schema.layout_digest(0))
        next_major = dict(base(), revision={'major': 2, 'minor': 0})
        self.assertNotEqual(build(next_major).layout_digest(0), schema.layout_digest(0))
        self.refused(dict(base(), history={'0': schema.layout_digest(0), '1': 'x'}), 'history holds')

    def test_a_line_over_the_limit_fails_the_generator(self):
        document = self.with_fields({'name': 'a' * 130, 'type': 'bool'})
        schema = build(document)
        text = gen.header_text(schema, {'schema': 'x'})
        self.assertTrue(any(len(line) > 120 for line in text.split('\n')))


class Sizes(unittest.TestCase):
    """The bounds a header publishes hold every encoding the reference writes."""

    def test_committed_schemas_bound_the_reference_writer(self):
        rng = random.Random(9)
        for entry in gen.OUTPUTS:
            schema, _ = gen.outputs_for(ROOT, entry)
            for message in schema.messages:
                item = message['type']
                if item.lossy:
                    continue
                for minor in range(message['since'], schema.minor + 1):
                    for _ in range(40):
                        writer = gen.RefWriter()
                        value = item.ref_gen(rng, minor)
                        item.ref_body_put(writer, value, minor)
                        data = writer.data()
                        self.assertGreaterEqual(len(data), (item.min_bits + 7) // 8)
                        self.assertLessEqual(len(data), (item.max_bits + 7) // 8)
                        self.assertTrue(gen.ref_verdict(item, data, minor))
                        reader = gen.RefReader(data)
                        self.assertEqual(item.ref_body_take(reader, minor), value)

    def test_identifiers_are_unique_and_committed_files_match(self):
        self.assertEqual(gen.main(['--check', '--root', str(ROOT)]), 0)

    def test_a_truncated_message_is_refused_at_every_length(self):
        schema, _ = gen.outputs_for(ROOT, gen.OUTPUTS[0])
        rng = random.Random(3)
        for message in schema.messages:
            item = message['type']
            writer = gen.RefWriter()
            item.ref_body_put(writer, item.ref_gen(rng, schema.minor), schema.minor)
            data = writer.data()
            self.assertTrue(gen.ref_verdict(item, data, schema.minor))
            for cut in range(len(data)):
                accepted = gen.ref_verdict(item, data[:cut], schema.minor)
                self.assertFalse(accepted, f'{message["name"]} cut at {cut}')

    def test_varuint_reference_is_canonical(self):
        for group in (1, 3, 7, 13, 32):
            for value in (0, 1, (1 << group) - 1, 1 << group, (1 << 64) - 1):
                writer = gen.RefWriter()
                gen.ref_put_varuint(writer, value, group)
                reader = gen.RefReader(writer.data())
                self.assertEqual(gen.ref_take_varuint(reader, group, (1 << 64) - 1), value)
        # Zero in two groups.
        writer = gen.RefWriter()
        writer.put(0b1000, 4)
        writer.put(0b0000, 4)
        with self.assertRaises(gen.RefFail):
            gen.ref_take_varuint(gen.RefReader(writer.data()), 3, 100)


class Deltas(SchemaCase):
    def delta_document(self, field, delta=True):
        document = self.with_fields({'name': 'a', 'type': 'uint', 'bits': 3}, field)
        document['messages'][0]['delta'] = delta
        return document

    def test_delta_attributes_are_refused_where_they_mean_nothing(self):
        plain = {'name': 'b', 'type': 'bytes', 'max': 3, 'delta_group': 2}
        self.refused(self.delta_document(plain), 'delta_group')
        for group in (0.5, 33, -1):
            field = {'name': 'b', 'type': 'uint', 'bits': 3, 'delta_group': group}
            self.refused(self.delta_document(field), 'delta_group')
        self.refused(self.delta_document({'name': 'b', 'type': 'f32', 'delta_group': 2}), 'delta_group')
        self.refused(self.delta_document({'name': 'b', 'type': 'list', 'max': 2,
                                          'of': {'type': 'bool', 'delta_group': 2}}), 'takes no delta_group')
        field = {'name': 'b', 'type': 'uint', 'bits': 3}
        self.refused(self.delta_document(field, delta='yes'), 'delta is true or false')

    def keyed(self, fields, key='id', extensible=False):
        document = self.with_fields({'name': 'a', 'type': 'list', 'of': {'type': 'Item'}, 'max': 4, 'delta_key': key})
        document['structs'] = {'Item': {'fields': fields, 'extensible': extensible}}
        return document

    def test_a_key_must_be_a_rising_number_every_element_has(self):
        identifier = {'name': 'id', 'type': 'uint', 'bits': 8}
        other = {'name': 'b', 'type': 'bool'}
        self.assertEqual(build(self.keyed([identifier, other])).messages[0]['type'].fields[0].type.key, 'id')
        self.refused(self.keyed([other, identifier]), 'first field')
        self.refused(self.keyed([identifier, other], key='b'), 'first field')
        self.refused(self.keyed([dict(identifier, optional=True), other]), 'must be a uint, range or varuint')
        self.refused(self.keyed([{'name': 'id', 'type': 'f32'}, other]), 'must be a uint, range or varuint')
        signed_key = {'name': 'id', 'type': 'varsint', 'min': -3, 'max': 3}
        self.refused(self.keyed([signed_key]), 'must be a uint, range or varuint')
        self.refused(self.keyed([identifier], extensible=True), 'plain struct')
        document = self.with_fields({'name': 'a', 'type': 'list', 'of': {'type': 'uint', 'bits': 3}, 'max': 2,
                                     'delta_key': 'x'})
        self.refused(document, 'plain struct')

    def test_a_delta_message_bounds_its_frame_and_changes_its_digest(self):
        field = {'name': 'b', 'type': 'bytes', 'size': 90}
        document = self.delta_document(field)
        document['channels'][0]['max_message_bytes'] = 91
        document_fits = copy.deepcopy(document)
        self.refused(document, 'above channel')
        document_fits['channels'][0]['max_message_bytes'] = 110
        schema = build(document_fits)
        least, most = gen.message_bounds(schema.messages[0])
        self.assertEqual(least, 'kDeltaHeaderMinBytes + 1U')
        self.assertIn('kDeltaHeaderMaxBytes', most)
        self.assertIn('kOnlyLayout', gen.header_text(schema, {'schema': 'x'}))
        plain = build(self.delta_document(field, delta=False))
        self.assertNotEqual(schema.layout_digest(0), plain.layout_digest(0))
        self.assertNotIn('DeltaLayout', gen.header_text(plain, {'schema': 'x'}))
        with_group = build(self.with_fields({'name': 'a', 'type': 'uint', 'bits': 3, 'delta_group': 2}))
        without = build(self.with_fields({'name': 'a', 'type': 'uint', 'bits': 3}))
        self.assertNotEqual(with_group.layout_digest(0), without.layout_digest(0))

    def test_folds_and_differences_are_small_either_way(self):
        for width in (1, 8, 64):
            top = (1 << width) - 1
            self.assertEqual(gen.ref_fold(1, width) if width > 1 else 1, 2 if width > 1 else 1)
            self.assertEqual(gen.ref_fold(-1, width), 1)
            self.assertLessEqual(gen.ref_fold(top, width), top)
        for value in (0, 1, -1, 5, -5, 1 << 40, -(1 << 40)):
            self.assertEqual(gen.ref_unfold(gen.ref_fold(value, 64)), value)

    def test_the_reference_delta_rebuilds_every_committed_delta_message(self):
        document = gen.json.loads((ROOT / 'tests/foundation/net/schemas/delta_test.json').read_text(encoding='utf-8'))
        schema = gen.Schema(document, 'delta_test')
        rng = random.Random(4)
        for message in schema.delta_messages():
            item = message['type']
            if item.lossy:
                continue
            for minor in range(schema.minor + 1):
                for _ in range(40):
                    base = item.ref_gen(rng, minor)
                    value = item.ref_perturb(base, rng, minor)
                    delta, whole = gen.RefWriter(), gen.RefWriter()
                    item.ref_delta_fields_put(delta, base, value, minor)
                    item.ref_body_put(whole, value, minor)
                    self.assertEqual(gen.ref_delta_verdict(item, base, delta.data(), minor), whole.data())


if __name__ == '__main__':
    result = unittest.main(exit=False, verbosity=1).result
    sys.exit(0 if result.wasSuccessful() else 1)
