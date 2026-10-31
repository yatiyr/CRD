#!/usr/bin/env python3
"""Every engine capability has one owning module (cmake/capability-owners.json, OWN.1 and OWN.2).

The registry names each capability's owner: a module the root CMakeLists.txt registers with crd_module (a landed
owner, whose public API files must exist inside it) or a module a ROADMAP row will create (a planned owner). A
capability with a recognisable implementation lists signatures: regular expressions over C-family code (comments and
string literals are not code) that match its constants, intrinsics or definitions. A signature found outside the
owner's directory in first-party product code (engine/, the host tools under tools/ and the sandbox application) is a
second implementation, and this guard fails on it unless the registry lists that file as `pending` (an owning row
moves it onto the owner) or `allowed` (the match is not an implementation, with the reason). A pending or allowed file
that no longer matches is stale and fails too, so the lists only shrink.

  python scripts/check-capability-owners.py [--root <repository root>] [--audit]

--audit prints every match outside its owner, pending and allowed ones included, grouped by capability. Tests and the
measurement programs under runtime/examples carry reference implementations as oracles and peers; they, mods, plugins,
user projects and CHIR/CEIR scripts are outside this guard.
"""
import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

REGISTRY = 'cmake/capability-owners.json'
SCHEMA = 'cerid-capability-owners/1'
ROOTS = ('engine', 'tools', 'sandbox')
SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.cu', '.h', '.hpp', '.inl', '.mm'}
MODULE = re.compile(r'^crd_module\(([^)]*)\)', re.MULTILINE)
ROW = re.compile(r'<a id="slice-([^"]+)"></a>')
ID = re.compile(r'^[a-z0-9]+(?:[.-][a-z0-9]+)*$')
CAPABILITY_KEYS = {'id', 'summary', 'owner', 'status', 'row', 'api', 'signatures', 'pending', 'allowed', 'consumers',
                   'decision'}

_spec = importlib.util.spec_from_file_location('check_environment_reads',
                                               Path(__file__).resolve().parent / 'check-environment-reads.py')
_reads = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_reads)
code_only = _reads.code_only


def registered_modules(cmake_text):
    """{module name: source directory} for every engine crd_module registration of the root CMakeLists.txt; the name
    is the one DEPENDS uses: NAME, else the source directory's last part (cmake/CrdModules.cmake)."""
    modules = {}
    for arguments in MODULE.findall(cmake_text):
        words = arguments.split()
        source = words[0].rstrip('/')
        if not source.startswith('engine/'):
            continue
        name = words[words.index('NAME') + 1] if 'NAME' in words else source.rsplit('/', 1)[-1]
        modules[name] = source + '/'
    return modules


def roadmap_rows(roadmap_text):
    """The row IDs of docs/ROADMAP.md, lower case, as their anchors spell them."""
    return set(ROW.findall(roadmap_text))


def row_known(rows, row):
    return isinstance(row, str) and row.lower() in rows


def validate(registry, modules, rows, root):
    """Problems with the registry itself: its shape, owners, rows, API files and signatures."""
    problems = []
    if registry.get('schema') != SCHEMA:
        problems.append('schema is %r, expected %r' % (registry.get('schema'), SCHEMA))
    capabilities = registry.get('capabilities')
    if not isinstance(capabilities, list) or not capabilities:
        return problems + ['capabilities must be a non-empty list']
    seen = set()
    for index, capability in enumerate(capabilities):
        where = 'capability %d' % index
        if not isinstance(capability, dict):
            problems.append('%s is not an object' % where)
            continue
        cid = capability.get('id')
        where = capability.get('id') or where
        if not isinstance(cid, str) or not ID.match(cid):
            problems.append('%s: id must be lower-case words joined by "." or "-"' % where)
        elif cid in seen:
            problems.append('%s: id registered twice' % cid)
        seen.add(cid)
        unknown = sorted(set(capability) - CAPABILITY_KEYS)
        if unknown:
            problems.append('%s: unknown keys %s' % (where, ', '.join(unknown)))
        if not isinstance(capability.get('summary'), str) or not capability['summary'].strip():
            problems.append('%s: needs a summary' % where)
        problems += _validate_owner(capability, where, modules, rows, root)
        problems += _validate_signatures(capability, where)
        for key in ('pending', 'allowed'):
            problems += _validate_entries(capability, key, where, rows, root)
        for row in capability.get('consumers', []):
            if not row_known(rows, row):
                problems.append('%s: consumer row %r is not a ROADMAP row' % (where, row))
    return problems


def _validate_owner(capability, where, modules, rows, root):
    problems = []
    owner, status = capability.get('owner'), capability.get('status', 'landed')
    if not isinstance(owner, str) or not owner:
        return ['%s: needs an owner module' % where]
    if status == 'landed':
        if owner not in modules:
            return ['%s: owner %r is not a crd_module registration (planned owners say "status": "planned")' % (
                where, owner)]
        api = capability.get('api')
        if not isinstance(api, list) or not api:
            problems.append('%s: a landed owner lists its public API files' % where)
        else:
            for path in api:
                if not isinstance(path, str) or not path.startswith(modules[owner]):
                    problems.append('%s: API file %r is not inside the owner %s' % (where, path, modules[owner]))
                elif not (root / path).is_file():
                    problems.append('%s: API file %s does not exist' % (where, path))
    elif status == 'assigned':
        if owner not in modules:
            problems.append('%s: an assigned owner is a crd_module registration; %r is not' % (where, owner))
        if not row_known(rows, capability.get('row')):
            problems.append('%s: an assigned capability names the ROADMAP row that creates its API' % where)
        if capability.get('api'):
            problems.append('%s: an assigned capability has no API yet; mark it landed with its API files' % where)
    elif status == 'planned':
        if owner in modules:
            problems.append('%s: owner %r is registered now; mark it landed with its API files' % (where, owner))
        if not row_known(rows, capability.get('row')):
            problems.append('%s: a planned owner names the ROADMAP row that creates it' % where)
        if capability.get('signatures') or capability.get('pending') or capability.get('allowed'):
            problems.append('%s: a planned owner has no directory for signatures to protect yet' % where)
    else:
        problems.append('%s: status must be "landed", "assigned" or "planned"' % where)
    if 'row' in capability and not row_known(rows, capability['row']):
        problems.append('%s: row %r is not a ROADMAP row' % (where, capability['row']))
    return problems


def _validate_signatures(capability, where):
    problems = []
    for signature in capability.get('signatures', []):
        if not isinstance(signature, dict) or set(signature) != {'regex', 'what'}:
            problems.append('%s: a signature is {"regex", "what"}' % where)
            continue
        try:
            re.compile(signature['regex'])
        except re.error as error:
            problems.append('%s: signature %r does not compile: %s' % (where, signature['what'], error))
    return problems


def _validate_entries(capability, key, where, rows, root):
    problems = []
    for entry in capability.get(key, []):
        need = {'path', 'row'} if key == 'pending' else {'path', 'why'}
        if not isinstance(entry, dict) or set(entry) != need:
            problems.append('%s: a %s entry is {%s}' % (where, key, ', '.join('"%s"' % k for k in sorted(need))))
            continue
        if not (root / entry['path']).is_file():
            problems.append('%s: %s file %s does not exist' % (where, key, entry['path']))
        if key == 'pending' and not row_known(rows, entry['row']):
            problems.append('%s: pending file %s names %r, not a ROADMAP row' % (where, entry['path'], entry['row']))
        if key == 'allowed' and not entry['why'].strip():
            problems.append('%s: allowed file %s needs the reason' % (where, entry['path']))
    return problems


def product_sources(root):
    for top in ROOTS:
        for path in sorted((root / top).rglob('*')):
            if path.suffix in SUFFIXES and path.is_file():
                yield path.relative_to(root).as_posix(), path


def scan(registry, modules, root):
    """{capability id: {path: [(line, what)]}} for every signature match in a product source outside its owner."""
    compiled = []
    for capability in registry['capabilities']:
        if capability.get('status', 'landed') == 'planned' or capability['owner'] not in modules:
            continue
        for signature in capability.get('signatures', []):
            compiled.append((capability['id'], modules[capability['owner']], re.compile(signature['regex']),
                             signature['what']))
    hits = {}
    for name, path in product_sources(root):
        relevant = [c for c in compiled if not name.startswith(c[1])]
        if not relevant:
            continue
        lines = code_only(path.read_text(encoding='utf-8', errors='replace')).splitlines()
        for number, line in enumerate(lines, 1):
            for cid, _, regex, what in relevant:
                if regex.search(line):
                    hits.setdefault(cid, {}).setdefault(name, []).append((number, what))
    return hits


def judge(registry, hits):
    """Problems: a match outside the owner that no pending or allowed entry covers, and a stale entry."""
    problems = []
    for capability in registry['capabilities']:
        cid = capability['id']
        found = hits.get(cid, {})
        listed = {}
        for key in ('pending', 'allowed'):
            for entry in capability.get(key, []):
                listed[entry['path']] = key
        for path, matches in sorted(found.items()):
            if path not in listed:
                lines = ', '.join(str(number) for number, _ in matches)
                problems.append('%s: %s implements it outside its owner %s at line(s) %s (%s); call the owner\'s API '
                                'instead' % (path, cid, capability['owner'], lines, matches[0][1]))
        for path, key in sorted(listed.items()):
            if path not in found:
                problems.append('%s: %s lists %s as %s, but no signature matches there any more; remove the entry' % (
                    cid, path, path, key))
    return problems


def load(root):
    root = Path(root)
    registry = json.loads((root / REGISTRY).read_text(encoding='utf-8'))
    modules = registered_modules((root / 'CMakeLists.txt').read_text(encoding='utf-8'))
    rows = roadmap_rows((root / 'docs/ROADMAP.md').read_text(encoding='utf-8'))
    return registry, modules, rows


def check(root):
    """(registry, hits, problems) for the checkout at `root`."""
    root = Path(root)
    registry, modules, rows = load(root)
    problems = validate(registry, modules, rows, root)
    if problems:
        return registry, {}, problems
    hits = scan(registry, modules, root)
    return registry, hits, judge(registry, hits)


def audit_lines(registry, hits):
    lines = []
    for capability in registry['capabilities']:
        found = hits.get(capability['id'])
        if not found:
            continue
        status = {e['path']: 'pending ' + e['row'] for e in capability.get('pending', [])}
        status.update({e['path']: 'allowed' for e in capability.get('allowed', [])})
        lines.append('%s (owner %s): %d file(s)' % (capability['id'], capability['owner'], len(found)))
        for path, matches in sorted(found.items()):
            numbers = ', '.join(str(number) for number, _ in matches)
            lines.append('  %s: %s [%s]' % (path, numbers, status.get(path, 'UNLISTED')))
    return lines


def main(argv):
    parser = argparse.ArgumentParser(prog='check-capability-owners.py', description=__doc__.splitlines()[0])
    parser.add_argument('--root', default=str(Path(__file__).resolve().parents[1]),
                        help='the repository root (default: this checkout)')
    parser.add_argument('--audit', action='store_true', help='print every match outside its owner')
    args = parser.parse_args(argv[1:])
    registry, hits, problems = check(args.root)
    if args.audit:
        for line in audit_lines(registry, hits):
            print(line)
    for problem in problems:
        print('check-capability-owners: %s' % problem)
    if problems:
        return 1
    pending = sum(len(c.get('pending', [])) for c in registry['capabilities'])
    print('check-capability-owners: %d capabilities, each with one owner; %d file(s) pending a move onto their owner'
          % (len(registry['capabilities']), pending))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
