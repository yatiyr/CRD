#!/usr/bin/env python3
"""A JSON Schema 2020-12 validator for the keywords the published MCP schema and ceridc's tool schemas use.

It is the conformance oracle of MCP-ADAPT.0 (tests/tools/ceridc/mcp_conformance.py, docs/design/agent-mcp.md). Every
keyword a schema holds must be one this module implements or an annotation it may ignore; any other keyword refuses
the schema, so a construct it does not understand is never read as permissive. `$ref` resolves only inside the
document (`#/$defs/<name>`, `#`): the MCP revisions forbid dereferencing a network reference by default, and the
published schema needs none. `format` is an annotation, as 2020-12 makes it by default. A `$ref` cycle that consumes
no instance depth is refused instead of recursing forever.

  python scripts/json-schema-subset.py <schema.json> <instance.json> [--ref <#/$defs/Name>]
"""
from pathlib import Path
import argparse
import json
import math
import sys

# The assertions this validator applies, and the annotations and containers it may pass over.
ASSERTIONS = {'type', 'enum', 'const', 'properties', 'required', 'additionalProperties', 'items', 'anyOf', 'allOf',
              'oneOf', 'not', 'minimum', 'maximum', 'minLength', 'maxLength', 'minItems', 'maxItems', '$ref'}
ANNOTATIONS = {'$schema', '$id', '$defs', '$comment', 'description', 'title', 'format', 'default', 'examples',
               'deprecated', 'readOnly', 'writeOnly'}
TYPES = {'null', 'boolean', 'object', 'array', 'number', 'string', 'integer'}


class SchemaError(ValueError):
    """The schema uses a keyword or a form this validator does not implement, or is not a valid schema."""


def json_type_of(value):
    if value is None:
        return 'null'
    if isinstance(value, bool):
        return 'boolean'
    if isinstance(value, (int, float)):
        return 'number'
    if isinstance(value, str):
        return 'string'
    if isinstance(value, list):
        return 'array'
    if isinstance(value, dict):
        return 'object'
    raise TypeError('not a JSON value: %r' % (value,))


def is_type(value, name):
    actual = json_type_of(value)
    if name == 'integer':
        # 2020-12: an integer is any number with a zero fractional part (1.0 is one).
        return actual == 'number' and (isinstance(value, int) or (math.isfinite(value) and value.is_integer()))
    return actual == name


def json_equal(a, b):
    """JSON equality: a boolean is never a number (Python's True == 1 is not), 1 equals 1.0, containers deeply."""
    ta, tb = json_type_of(a), json_type_of(b)
    if ta != tb:
        return False
    if ta == 'array':
        return len(a) == len(b) and all(json_equal(x, y) for x, y in zip(a, b))
    if ta == 'object':
        return a.keys() == b.keys() and all(json_equal(a[k], b[k]) for k in a)
    return a == b


def check_schema(schema, where='#'):
    """Refuse a schema holding a keyword outside ASSERTIONS and ANNOTATIONS, or a malformed one of them."""
    if isinstance(schema, bool):
        return
    if not isinstance(schema, dict):
        raise SchemaError('%s: a schema is an object or a boolean' % where)
    for key, value in schema.items():
        at = '%s/%s' % (where, key)
        if key not in ASSERTIONS and key not in ANNOTATIONS:
            raise SchemaError('%s: the keyword %r is not implemented by this validator' % (at, key))
        if key == 'type':
            names = value if isinstance(value, list) else [value]
            if not names or any(n not in TYPES for n in names):
                raise SchemaError('%s: unknown type %r' % (at, value))
        elif key in ('properties', '$defs'):
            if not isinstance(value, dict):
                raise SchemaError('%s: must be an object' % at)
            for name, sub in value.items():
                check_schema(sub, '%s/%s' % (at, name))
        elif key in ('additionalProperties', 'items', 'not'):
            check_schema(value, at)
        elif key in ('anyOf', 'allOf', 'oneOf'):
            if not isinstance(value, list) or not value:
                raise SchemaError('%s: must be a non-empty array' % at)
            for i, sub in enumerate(value):
                check_schema(sub, '%s/%d' % (at, i))
        elif key == 'required':
            if not isinstance(value, list) or any(not isinstance(n, str) for n in value):
                raise SchemaError('%s: must be an array of strings' % at)
        elif key == 'enum':
            if not isinstance(value, list):
                raise SchemaError('%s: must be an array' % at)
        elif key in ('minimum', 'maximum'):
            if json_type_of(value) != 'number':
                raise SchemaError('%s: must be a number' % at)
        elif key in ('minLength', 'maxLength', 'minItems', 'maxItems'):
            if not is_type(value, 'integer') or value < 0:
                raise SchemaError('%s: must be a non-negative integer' % at)
        elif key == '$ref':
            if not isinstance(value, str) or not (value == '#' or value.startswith('#/')):
                raise SchemaError('%s: only references inside the document are resolved, not %r' % (at, value))


class Validator:
    """Validates instances against `document` (a whole schema document, its $defs the reference targets)."""

    def __init__(self, document):
        check_schema(document)
        self.document = document

    def resolve(self, ref):
        node = self.document
        if ref == '#':
            return node
        for part in ref[2:].split('/'):
            part = part.replace('~1', '/').replace('~0', '~')
            if not isinstance(node, dict) or part not in node:
                raise SchemaError('unresolved reference %r' % ref)
            node = node[part]
        return node

    def errors(self, instance, schema=None, path='$'):
        """Every way `instance` fails `schema` (the document when None), each naming where."""
        out = []
        self._check(instance, self.document if schema is None else schema, path, out, ())
        return out

    def valid(self, instance, schema=None):
        return not self.errors(instance, schema)

    def ref_errors(self, instance, ref):
        return self.errors(instance, {'$ref': ref})

    def _check(self, value, schema, path, out, refs):
        if schema is True:
            return
        if schema is False:
            out.append('%s: no value is allowed here' % path)
            return
        if '$ref' in schema:
            ref = schema['$ref']
            key = (ref, id(value))
            if key in refs:
                raise SchemaError('a $ref cycle through %r consumes no instance' % ref)
            self._check(value, self.resolve(ref), path, out, refs + (key,))
        if 'type' in schema:
            names = schema['type'] if isinstance(schema['type'], list) else [schema['type']]
            if not any(is_type(value, n) for n in names):
                out.append('%s: %s is not of type %s' % (path, json_type_of(value), '/'.join(names)))
                return
        if 'const' in schema and not json_equal(value, schema['const']):
            out.append('%s: %r is not %r' % (path, value, schema['const']))
        if 'enum' in schema and not any(json_equal(value, e) for e in schema['enum']):
            out.append('%s: %r is not one of %r' % (path, value, schema['enum']))
        if json_type_of(value) == 'number':
            if 'minimum' in schema and value < schema['minimum']:
                out.append('%s: %r is below the minimum %r' % (path, value, schema['minimum']))
            if 'maximum' in schema and value > schema['maximum']:
                out.append('%s: %r is above the maximum %r' % (path, value, schema['maximum']))
        if isinstance(value, str):
            if 'minLength' in schema and len(value) < schema['minLength']:
                out.append('%s: shorter than %d characters' % (path, schema['minLength']))
            if 'maxLength' in schema and len(value) > schema['maxLength']:
                out.append('%s: longer than %d characters' % (path, schema['maxLength']))
        if isinstance(value, list):
            if 'minItems' in schema and len(value) < schema['minItems']:
                out.append('%s: fewer than %d items' % (path, schema['minItems']))
            if 'maxItems' in schema and len(value) > schema['maxItems']:
                out.append('%s: more than %d items' % (path, schema['maxItems']))
            if 'items' in schema:
                for i, item in enumerate(value):
                    self._check(item, schema['items'], '%s[%d]' % (path, i), out, refs)
        if isinstance(value, dict):
            properties = schema.get('properties', {})
            for name in schema.get('required', []):
                if name not in value:
                    out.append('%s: the member %r is required' % (path, name))
            for name, member in value.items():
                at = '%s.%s' % (path, name)
                if name in properties:
                    self._check(member, properties[name], at, out, refs)
                elif 'additionalProperties' in schema:
                    self._check(member, schema['additionalProperties'], at, out, refs)
        for sub in schema.get('allOf', []):
            self._check(value, sub, path, out, refs)
        if 'anyOf' in schema:
            branches = [self._branch(value, sub, path, refs) for sub in schema['anyOf']]
            if all(branches):
                out.append('%s: matches none of anyOf (%s)' % (path, '; '.join(b[0] for b in branches)))
        if 'oneOf' in schema:
            matched = sum(1 for sub in schema['oneOf'] if not self._branch(value, sub, path, refs))
            if matched != 1:
                out.append('%s: matches %d of oneOf, not exactly one' % (path, matched))
        if 'not' in schema and not self._branch(value, schema['not'], path, refs):
            out.append('%s: matches the schema under not' % path)

    def _branch(self, value, schema, path, refs):
        out = []
        self._check(value, schema, path, out, refs)
        return out


def main(argv=None):
    parser = argparse.ArgumentParser(prog='json-schema-subset.py', description=__doc__.splitlines()[0])
    parser.add_argument('schema', type=Path)
    parser.add_argument('instance', type=Path)
    parser.add_argument('--ref', help='validate against this definition (#/$defs/Name) instead of the document')
    args = parser.parse_args(argv)
    try:
        validator = Validator(json.loads(args.schema.read_text(encoding='utf-8')))
        instance = json.loads(args.instance.read_text(encoding='utf-8'))
        problems = validator.ref_errors(instance, args.ref) if args.ref else validator.errors(instance)
    except (OSError, ValueError) as error:
        print('json-schema-subset: %s' % error, file=sys.stderr)
        return 2
    for problem in problems:
        print('json-schema-subset: %s' % problem)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
