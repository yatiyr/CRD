#!/usr/bin/env python3
"""The cryptography inventory of every Cerid export and its reviewed export-control classification (COMPLY.1).

An export is what leaves the publisher: the relocatable package (every module whose CMakeLists.txt calls
crd_package_library) and every program module the root CMakeLists.txt registers outside engine/ (the host tools,
the runtime and the sandbox). Its inventory is read from the build and the NET-STD standards register, never typed:
the modules its closure reaches (crd_module DEPENDS), the third-party packages they declare (PACKAGES), every
implemented register entry those modules own that performs a cryptographic function (`cryptography`) and every
implemented algorithm among them (a "Cerid algorithm identifiers" codepoint: its class, functions and key lengths).
Third-party cryptography can only enter through the crypto owner's providers (the crypto.providers signatures refuse
a crypto library header in any other module), so the register entries are the whole of it.

The committed inventory, assets/comply/crypto-inventory.json, is what reviewers and the store compliance kit read.
cmake/legal-register.json's `crypto_classifications` holds, per export, a classification record per release and one
head record (`release` null) for the next one: the SHA-256 of the export's inventory it was reviewed against, the
day, a finding per export-control obligation whose active check cites this script, and the counsel review.
scripts/check-registers.py (crd-check-registers) recomputes the inventory and fails an export whose inventory
changed since its head record was reviewed. docs/design/registers.md#crypto-classification is the contract.

  python scripts/gen-crypto-inventory.py                      # write the inventory
  python scripts/gen-crypto-inventory.py --check              # byte-compare with the committed file; nonzero on drift
  python scripts/gen-crypto-inventory.py --review EXPORT [--day D]    # record the restated findings' review
  python scripts/gen-crypto-inventory.py --release EXPORT VERSION     # freeze the current head record for a release
  python scripts/gen-crypto-inventory.py --facts EXPORT       # the facts the store compliance kit answers from

The crd-comply-crypto-inventory-drift CTest runs --check; scripts/generated-sources.json records the file's bytes.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = 'scripts/gen-crypto-inventory.py'
INVENTORY = 'assets/comply/crypto-inventory.json'
STANDARDS = 'cmake/standards-register.json'
LEGAL = 'cmake/legal-register.json'
FORMAT = 'cerid-crypto-inventory/1'
PACKAGE = 'cerid-package'
MODULE = re.compile(r'\bcrd_module\(([^()]*(?:\([^()]*\)[^()]*)*)\)')
KEYWORDS = {'HOST', 'NAME', 'EXECUTABLES', 'DEPENDS', 'PACKAGES', 'TESTS', 'TEST_DEPENDS', 'TEST_PACKAGES'}
RELEASE = re.compile(r'^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')

_spec = importlib.util.spec_from_file_location('gen_register_ids', Path(__file__).resolve().parent /
                                               'gen-register-ids.py')
_ids = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_ids)


class InventoryError(Exception):
    pass


def module_graph(cmake_text):
    """{module: {'source', 'depends', 'packages'}} of every crd_module registration (cmake/CrdModules.cmake): the
    name is NAME, else the source directory's last part."""
    graph = {}
    for body in MODULE.findall(re.sub(r'#[^\n]*', '', cmake_text)):
        tokens = body.split()
        if not tokens:
            continue
        fields = {'NAME': [], 'DEPENDS': [], 'PACKAGES': []}
        current = None
        for token in tokens[1:]:
            if token in KEYWORDS:
                current = token
            elif current in fields:
                fields[current].append(token)
        name = fields['NAME'][0] if fields['NAME'] else tokens[0].rsplit('/', 1)[-1]
        if name in graph:
            raise InventoryError(f'CMakeLists.txt registers the module {name} twice')
        graph[name] = {'source': tokens[0], 'depends': fields['DEPENDS'], 'packages': fields['PACKAGES']}
    return graph


def closure(graph, roots):
    reached, pending = set(), list(roots)
    while pending:
        name = pending.pop()
        if name in reached:
            continue
        if name not in graph:
            raise InventoryError(f'the module {name} is depended on but not registered in CMakeLists.txt')
        reached.add(name)
        pending += graph[name]['depends']
    return reached


def exports(root, graph):
    """[(id, kind, title, root modules)]: the package, then every program module in registration order."""
    found = []
    members = [name for name, module in graph.items() if module['source'].startswith('engine/') and
               (root / module['source'] / 'CMakeLists.txt').is_file() and
               'crd_package_library(' in (root / module['source'] / 'CMakeLists.txt').read_text(encoding='utf-8')]
    if members:
        found.append((PACKAGE, 'package', 'The relocatable Cerid package: the engine libraries cmake --install '
                      'exports as Cerid:: targets', members))
    for name, module in graph.items():
        if not module['source'].startswith('engine/'):
            if name == PACKAGE:
                raise InventoryError(f'a module may not be named {PACKAGE}, the package export\'s id')
            found.append((name, 'program', f'The {name} program ({module["source"]})', [name]))
    return found


def ordered(functions):
    return [name for name in _ids.CRYPTO_FUNCTIONS if name in functions]


def cryptographic_content(standards, owners):
    """(entries, algorithms) the implemented standards entries owned by these modules contribute."""
    entries, algorithms = [], []
    for entry in standards['entries']:
        if entry['state'] != 'implemented' or entry['owner'] not in owners or not entry['cryptography']:
            continue
        standard = entry['spec']['type'] != 'cerid'
        entries.append({'id': entry['id'], 'kind': entry['kind'], 'owner': entry['owner'],
                        'functions': entry['cryptography'], 'spec': entry['spec']['ref'], 'standard': standard})
        for point in entry.get('codepoints', []):
            if point['registry'] != _ids.ALGORITHMS:
                continue
            value = int(point['value'], 16)
            name, _, functions = _ids.ALGORITHM_CLASSES[value >> 8]
            algorithms.append({'name': point['name'], 'value': point['value'], 'class': name,
                               'functions': list(functions), 'key_bits': point['key_bits'], 'entry': entry['id'],
                               'spec': entry['spec']['ref'], 'standard': standard, 'owner': entry['owner']})
    return entries, algorithms


def build(root):
    """The inventory of the tree at `root`."""
    try:
        graph = module_graph((root / 'CMakeLists.txt').read_text(encoding='utf-8'))
        standards = json.loads((root / STANDARDS).read_text(encoding='utf-8'))
        items = []
        for export_id, kind, title, roots in exports(root, graph):
            modules = closure(graph, roots)
            entries, algorithms = cryptographic_content(standards, modules)
            functions = ordered({name for item in entries + algorithms for name in item['functions']})
            packages = sorted({package for name in modules for package in graph[name]['packages']})
            items.append({'id': export_id, 'kind': kind, 'title': title, 'packages': packages, 'functions': functions,
                          'confidentiality': 'confidentiality' in functions, 'entries': entries,
                          'algorithms': algorithms})
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise InventoryError(f'cannot read the inventory\'s sources: {error!r}') from error
    return {'format': FORMAT,
            'about': f'Generated by {GENERATOR} from CMakeLists.txt, the modules\' CMakeLists.txt and {STANDARDS}; '
                     'never edit by hand.',
            'exports': items}


def render(inventory):
    return json.dumps(inventory, indent=2, ensure_ascii=False) + '\n'


def digest(export):
    """The SHA-256 a classification record holds: of the export's inventory in canonical JSON."""
    canonical = json.dumps(export, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
    return hashlib.sha256(canonical.encode('utf-8')).hexdigest()


def find(inventory, export_id):
    for export in inventory['exports']:
        if export['id'] == export_id:
            return export
    raise InventoryError(f'{export_id} is not an export ({", ".join(item["id"] for item in inventory["exports"])})')


def head_of(legal, export_id):
    """The export's head record (the last, with `release` null)."""
    records = (legal.get('crypto_classifications') or {}).get(export_id) or []
    if not records or records[-1].get('release') is not None:
        raise InventoryError(f'{export_id} has no head record in {LEGAL} crypto_classifications; author one with its '
                             f'findings (docs/design/registers.md#crypto-classification), then review it')
    return records[-1]


def write_legal(root, legal):
    records = legal['crypto_classifications']
    legal['crypto_classifications'] = {key: records[key] for key in sorted(records)}
    (root / LEGAL).write_bytes((json.dumps(legal, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))


def review(root, legal, export_id, day):
    """Record the review of the export's restated findings against its current inventory."""
    export = find(build(root), export_id)
    head = head_of(legal, export_id)
    current = digest(export)
    if head.get('inventory') == current:
        raise InventoryError(f'{export_id}\'s inventory is unchanged since its review on {head.get("reviewed")}; '
                             'nothing to review')
    if not head.get('findings'):
        raise InventoryError(f'{export_id}\'s head record states no findings; restate them before the review')
    counsel = head.get('counsel') if isinstance(head.get('counsel'), dict) else {'needed': True}
    head.update({'inventory': current, 'reviewed': day.isoformat(),
                 'counsel': {'needed': counsel.get('needed', True), 'reviewed': None}})
    write_legal(root, legal)
    return current


def release(root, legal, export_id, version):
    """Freeze the export's current, counsel-reviewed head record as the classification of `version`."""
    if not RELEASE.match(version):
        raise InventoryError(f'{version!r} is not a release version (MAJOR.MINOR.PATCH[-PRE])')
    head = head_of(legal, export_id)
    if head.get('inventory') != digest(find(build(root), export_id)):
        raise InventoryError(f'{export_id}\'s inventory changed since its head record was reviewed; review it first')
    if head['counsel']['needed'] and not head['counsel']['reviewed']:
        raise InventoryError(f'{export_id}\'s classification awaits its counsel review (a user action)')
    records = legal['crypto_classifications'][export_id]
    if any(record['release'] == version for record in records):
        raise InventoryError(f'{export_id} already has a classification record for {version}')
    records.insert(len(records) - 1, dict(head, release=version))
    write_legal(root, legal)


def facts(root, legal, export_id):
    """What the store compliance kit (COMPLY.8) turns into the Apple export compliance answer and the EAR
    self-classification report: the inventory, the reviewed findings and the answers they imply."""
    export = find(build(root), export_id)
    head = head_of(legal, export_id)
    if head.get('inventory') != digest(export):
        raise InventoryError(f'{export_id}\'s inventory changed since its head record was reviewed; review it first')
    return {'export': export_id, 'title': export['title'], 'inventory': head['inventory'],
            'reviewed': head['reviewed'], 'counsel': head['counsel'],
            'functions': export['functions'], 'confidentiality': export['confidentiality'],
            'algorithms': [{key: algorithm[key] for key in ('name', 'key_bits', 'spec', 'standard')}
                           for algorithm in export['algorithms']],
            'non_standard': [item['id'] for item in export['entries'] + export['algorithms'] if not item['standard']
                             and 'confidentiality' in item['functions']],
            'findings': head['findings'],
            'apple': {'uses_non_exempt_encryption': export['confidentiality']},
            'reports': [{key: finding[key] for key in ('entry', 'classification', 'authorization')}
                        for finding in head['findings'] if finding['report']]}


def main(argv=None):
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description='Generate the cryptography inventory and keep its classifications')
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument('--check', action='store_true', help='compare with the committed file instead of writing')
    actions.add_argument('--review', metavar='EXPORT', help='record the review of an export\'s restated findings')
    actions.add_argument('--release', nargs=2, metavar=('EXPORT', 'VERSION'), help='freeze a release\'s record')
    actions.add_argument('--facts', metavar='EXPORT', help='print the facts the store compliance kit reads')
    parser.add_argument('--day', help='the review day (YYYY-MM-DD; default the UTC date)')
    parser.add_argument('--root', default=str(ROOT), help='repository root (tests use a fixture)')
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    try:
        if args.review or args.release or args.facts:
            legal = json.loads((root / LEGAL).read_text(encoding='utf-8'))
        if args.review:
            day = datetime.date.fromisoformat(args.day) if args.day else \
                datetime.datetime.now(datetime.timezone.utc).date()
            print(f'reviewed {args.review} on {day}: inventory {review(root, legal, args.review, day)}')
            return 0
        if args.release:
            release(root, legal, *args.release)
            print(f'released {args.release[0]} {args.release[1]}')
            return 0
        if args.facts:
            print(json.dumps(facts(root, legal, args.facts), indent=2, ensure_ascii=False))
            return 0
        text = render(build(root))
    except (InventoryError, OSError, ValueError, KeyError, TypeError) as error:
        print(f'FAIL: {error}')
        return 1
    path = root / INVENTORY
    if args.check:
        if not path.is_file() or path.read_bytes() != text.encode('utf-8'):
            print(f'FAIL: {INVENTORY} differs from {GENERATOR}\'s output; run python {GENERATOR}')
            return 1
        print(f'cryptography inventory current: {len(json.loads(text)["exports"])} export(s)')
        return 0
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode('utf-8'))
    print(f'wrote {INVENTORY}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
