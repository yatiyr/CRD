#!/usr/bin/env python3
"""Validate Cerid's single documentation tracker and its current entry-point links.

Run from any directory: python scripts/check-master-plan.py
This checks documentation structure and references, not runtime completion or performance.
"""
from pathlib import Path
import json
import re
import sys
import argparse
import hashlib
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
BUDGETS = {'START_HERE.md': 4500, 'AGENTS.md': 8000, 'docs/PRINCIPLES.md': 6500, 'docs/SANITY.md': 4500,
           'docs/BUILDING.md': 7000, 'MEMORY.md': 3000, 'context.md': 2500, 'docs/README.md': 6000}
STATES = {'Done', 'In progress', 'Blocked', 'Recorded', 'Review', 'Open', 'Partial', 'Later'}
COMPLETE = {'Done', 'Recorded'}


def document_paths():
    return ['START_HERE.md', 'AGENTS.md', 'README.md', 'context.md', 'MEMORY.md'] + [
        path.relative_to(ROOT).as_posix() for path in sorted((ROOT / 'docs').rglob('*.md'))
    ] + (['CLAUDE.md'] if (ROOT / 'CLAUDE.md').exists() else [])


def read(path):
    return path.read_text(encoding='utf-8-sig')


def outside_fences(text):
    lines = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r'^\s*(`{3,}|~{3,})', line)
        if marker:
            if fence is None:
                fence = marker[1][0]
            elif marker[1][0] == fence:
                fence = None
            lines.append('')
        else:
            lines.append(line if fence is None else '')
    return '\n'.join(lines)


def anchors(path):
    text = outside_fences(read(path))
    result = set(re.findall(r'\b(?:id|name)=["\x27]([^"\x27]+)', text))
    counts = {}
    for heading in re.findall(r'^#{1,6}\s+(.+?)\s*#*$', text, flags=re.M):
        heading = re.sub(r'\[([^]]+)\]\([^)]*\)', r'\1', heading)
        heading = re.sub(r'<[^>]+>', '', heading)
        slug = re.sub(r'[^\w\- ]', '', heading.lower()).replace(' ', '-')
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        result.add(slug + (f'-{count}' if count else ''))
    return result


def link_errors(files):
    errors = []
    count = 0
    cache = {}
    names = {}

    def check_case(target):
        try:
            parts = target.relative_to(ROOT).parts
        except ValueError:
            return True
        current = ROOT
        for part in parts:
            if current not in names:
                names[current] = {child.name.lower(): child.name for child in current.iterdir()}
            actual = names[current].get(part.lower())
            if actual != part:
                return False
            current = current / part
        return True
    for name in files:
        path = ROOT / name
        # Inline code can contain state[depth=2](0,iv), but must remain part of heading slugs.
        body = re.sub(r'(`+).*?\1', '', outside_fences(read(path)))
        for match in re.finditer(r'\]\((<[^>\n]+>|[^)\s]+)(?:\s+"[^"\n]*")?\)', body):
            url = match[1].strip('<>')
            if re.match(r'[A-Za-z][A-Za-z\d+.-]*:', url) or url.startswith('//'):
                continue
            count += 1
            dest, _, fragment = unquote(url).partition('#')
            target = (path.parent / dest).resolve() if dest else path
            if not target.exists():
                errors.append(f'{name}: missing path {url}')
            elif not check_case(target):
                errors.append(f'{name}: path case will fail on a case-sensitive filesystem: {url}')
            elif fragment and target.is_file() and target.suffix.lower() == '.md':
                if target not in cache:
                    cache[target] = anchors(target)
                if fragment not in cache[target]:
                    errors.append(f'{name}: missing anchor {url}')
    return count, errors


def code_spans(text):
    """Preserve reference code independently of rebased prose links and navigation banners."""
    result = []
    fence = None
    block = []
    for line in text.splitlines():
        marker = re.match(r'^\s*(`{3,}|~{3,})', line)
        if marker and fence is None:
            fence = marker[1][0]
            block = [line]
        elif fence:
            block.append(line)
            if marker and marker[1][0] == fence:
                result.append('\n'.join(block))
                fence = None
        else:
            result.extend(match[0] for match in re.finditer(r'(`+).*?\1', line))
    if fence:
        result.append('\n'.join(block))
    return result


def code_digest(text):
    return hashlib.sha256(json.dumps(code_spans(text), ensure_ascii=False).encode('utf-8')).hexdigest()


def parse_rows(text):
    rows = []
    for line in text.splitlines():
        if re.match(r'^\| \d+ \|', line):
            cells = [part.strip() for part in line.split('|')[1:-1]]
            if len(cells) != 6:
                raise ValueError('Expected six cells: ' + line[:100])
            match = re.search(r'\*\*([^*]+)\*\*', cells[1])
            if not match:
                raise ValueError('Missing slice ID: ' + line[:100])
            rows.append((match[1], cells))
    return rows


def show_rows(rows):
    for name, cells in rows:
        print(f'{cells[0]} {name} [{cells[2]}]\n  {cells[3]}\n  Requires: {cells[4]}\n  Sources: {cells[5]}\n')


def query(args, rows):
    if args.memory:
        print('Reference lookup only: old state/schedules/grants defer to current AGENTS/context/ROADMAP.\n')
        memory_index = json.loads(read(ROOT / 'docs/lessons/memory/index.json'))
        records = memory_index['records']
        term = args.memory.removesuffix('.md')
        if term in memory_index.get('retired_reference_routes', {}):
            print('Original named source was absent from the captured corpus. Current rule: ' +
                  memory_index['retired_reference_routes'][term])
            return 0
        term = memory_index.get('aliases', {}).get(term, term)
        if term in records:
            record = records[term]
            source = read(ROOT / record['path'])
            begin = source.index(f'<a id="{record["anchor"]}"></a>')
            end = source.index(f'<!-- end-memory:{term} -->', begin)
            print('Historical reference. Current rules/status remain AGENTS, MEMORY and ROADMAP.\n')
            print(source[begin:end].strip())
            return 0
        matches = [(key, value) for key, value in records.items()
                   if term.lower() in (key + ' ' + value['description']).lower()]
        for key, value in matches[:30]:
            print(f'{key}\n  {value["description"]}\n  {value["path"]}#{value["anchor"]}')
        print(f'{len(matches)} matches; showing at most 30. Use an exact record name to read one.')
        return 0 if matches else 1
    if args.slice:
        term = args.slice.lower()
        selected = [(key, value) for key, value in rows
                    if key.lower() == term or re.match(re.escape(term) + r'(?:[.-]|[a-z](?=[.-]|$))', key.lower())]
        show_rows(selected)
        if not selected:
            print('No such slice. Use --find for a text search.')
        return 0 if selected else 1
    if args.find:
        selected = [(key, value) for key, value in rows if args.find.lower() in ' '.join(value).lower()]
        show_rows(selected[:30])
        print(f'{len(selected)} matches; showing at most 30.')
        return 0 if selected else 1
    if args.next:
        current = re.search(r'<!-- current-slice: ([^ ]+) -->', read(ROOT / 'context.md'))
        print('Current context pointer: ' + (current[1] if current else 'MISSING'))
        selected = [(key, value) for key, value in rows if value[2] not in COMPLETE][:1]
        show_rows(selected)
        print('First unfinished row in the default order; inspect its gate and user authorization before acting.')
        return 0
    return None


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    text = read(ROOT / 'docs/ROADMAP.md')
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--slice', help='Show a slice and named children from the single master table')
    group.add_argument('--find', help='Find a symptom, term or audit finding ID in master rows')
    group.add_argument('--memory', help='Search reference memories, or read one by exact record name')
    group.add_argument('--next', action='store_true', help='Show the current pointer and first unfinished row')
    args = parser.parse_args()
    errors = []
    try:
        rows = parse_rows(text)
    except ValueError as error:
        print('FAIL: ' + str(error))
        return 1
    result = query(args, rows)
    if result is not None:
        return result
    ids = [name for name, _ in rows]
    index = {name: i for i, name in enumerate(ids)}
    if len(index) != len(ids):
        errors.append('Duplicate slice IDs')
    if len(re.findall(r'^\|\s*:?-{3,}', text, flags=re.M)) != 1:
        errors.append('ROADMAP must contain exactly one table')
    for i, (name, cells) in enumerate(rows):
        if cells[2] not in STATES:
            errors.append('Unknown state: ' + name)
        if cells[2] == 'Done' and not re.search(r'\]\((?:sessions|bench)/', cells[5]):
            errors.append('Done requires session/benchmark evidence: ' + name)
        if int(cells[0]) != i + 1:
            errors.append('Non-contiguous order at ' + name)
        if f'id="slice-{name.lower()}"' not in cells[1]:
            errors.append('Missing stable anchor: ' + name)
        if '[Detail](' not in cells[5] or '[ADR-' not in cells[5]:
            errors.append('Missing detail or ADR: ' + name)
        for dep in re.findall(r'\[([^]]+)\]\(#slice-', cells[4]):
            if dep not in index or index[dep] >= i:
                errors.append(f'{name}: dependency is missing or later: {dep}')
            elif cells[2] == 'Done' and rows[index[dep]][1][2] not in COMPLETE:
                errors.append(f'{name}: Done but prerequisite {dep} is incomplete')
    for name in ids:
        children = [child for child in ids if child.startswith(name + '.')]
        if children and index[name] < max(index[child] for child in children):
            errors.append('Parent precedes its children: ' + name)
        if rows[index[name]][1][2] == 'Done' and any(rows[index[child]][1][2] not in COMPLETE for child in children):
            errors.append('Done parent has incomplete children: ' + name)
        if name.endswith('.runtime') and index[name] >= index['RENDERER-RUNTIME']:
            errors.append('Renderer runtime appears after its close: ' + name)
        if name.startswith(('I2D-', 'SPR-')) and index[name] <= index['RENDERER-RUNTIME']:
            errors.append('UI/SPR work precedes the renderer gate: ' + name)
        if name.endswith('.authoring') and index[name] <= index['I2D-9']:
            errors.append('Visual authoring precedes the editor widgets: ' + name)
    if not index['CR-D007'] < index['NOTEBOOK']:
        errors.append('Notebook precedes CR-D007 release')
    snapshot = json.loads(read(ROOT / 'docs/research/2026-09-12-system-audit.json'))
    for old, route in snapshot['source_id_routes'].items():
        if route['owner'] not in index and route['owner'] != 'Recorded CEIR/RAF history':
            errors.append('Unrouted D-007 source ID: ' + old)
    for old, owners in snapshot.get('v17_source_routes', {}).items():
        if not owners or any(owner not in index for owner in owners):
            errors.append('Unrouted v17 source ID: ' + old)
    for finding, owners in snapshot.get('finding_owners', {}).items():
        for owner in owners:
            if owner not in index or f'[{finding}](' not in rows[index[owner]][1][5]:
                errors.append(f'Finding {finding} missing from owner {owner}')
    expansion = json.loads(read(ROOT / 'docs/research/2026-09-12-whole-system-census.json'))
    for name in expansion['retained_master_ids'] + expansion['added_master_ids']:
        if name not in index:
            errors.append('Whole-system requirement lost its master ID: ' + name)
    for finding, owners in expansion['finding_owners'].items():
        if not owners:
            errors.append('Whole-system finding has no owner: ' + finding)
        for owner in owners:
            if owner not in index or f'[{finding}](' not in rows[index[owner]][1][5]:
                errors.append(f'Finding {finding} missing from owner {owner}')
    # Dated inventory is evidence, not a mutable qualification database. Validate its shape/routes,
    # not equality with source hashes that legitimately change during later implementation.
    module_names = [entry['module'] for entry in expansion['modules']]
    if len(module_names) != len(set(module_names)) or any(not entry['family'] for entry in expansion['modules']):
        errors.append('Whole-system census has duplicate or unclassified module entries')
    for name in snapshot['removed_files']:
        if (ROOT / name).exists():
            errors.append('Superseded tracker returned: ' + name)
    for archive in [snapshot['archive'], 'docs/archive/D-007-gpu-program-system.md']:
        if not (ROOT / archive).is_file():
            errors.append('Missing preserved source archive: ' + archive)
    current = re.search(r'<!-- current-slice: ([^ ]+) -->', read(ROOT / 'context.md'))
    if not current or current[1] not in index:
        errors.append('Context must name one real current-slice ID')
    elif rows[index[current[1]]][1][2] in COMPLETE:
        errors.append('Context points to completed work; advance its pointer')
    for name, limit in BUDGETS.items():
        if (ROOT / name).stat().st_size > limit:
            errors.append(f'Orientation budget exceeded: {name} > {limit} bytes')
    files = document_paths()
    trackers = []
    for name in files:
        source = read(ROOT / name)
        role = re.search(r'<!-- doc-role: ([a-z-]+) -->', source[:2000])
        if role and role[1] == 'tracker':
            trackers.append(name)
        if any(ord(char) < 32 and char not in '\n\r\t' for char in source):
            errors.append('Unexpected control character: ' + name)
        if not name.startswith('docs/generated/') and '<!-- doc-role:' not in source[:2000]:
            errors.append('Missing document-role/current-route marker: ' + name)
        if name.startswith(('docs/phases/', 'docs/systems/')) and 'Reference pointer; no live status here.' in source:
            if (ROOT / name).stat().st_size > 1200:
                errors.append('Compatibility pointer exceeds 1200 bytes: ' + name)
    if trackers != ['docs/ROADMAP.md']:
        errors.append('Exactly one tracker role is allowed: docs/ROADMAP.md')
    memory_index = json.loads(read(ROOT / 'docs/lessons/memory/index.json'))
    memory_bodies = {}
    for name, record in memory_index['records'].items():
        if record['path'] not in memory_bodies:
            memory_bodies[record['path']] = read(ROOT / record['path'])
        body = memory_bodies[record['path']]
        start = f'<a id="{record["anchor"]}"></a>'
        end = f'<!-- end-memory:{name} -->'
        if body.count(start) != 1 or body.count(end) != 1:
            errors.append('Memory record missing: ' + name)
        elif 'source_code_sha256' in record:
            record_text = body[body.index(start):body.index(end, body.index(start))]
            if code_digest(record_text) != record['source_code_sha256']:
                errors.append('Preserved memory code changed; add new lessons separately: ' + name)
    for alias, name in memory_index.get('aliases', {}).items():
        if name not in memory_index['records']:
            errors.append('Memory alias has no target: ' + alias)
    count, broken = link_errors(files)
    errors.extend(broken)
    print(f'Checked {len(rows)} rows, {len(files)} documents, {count} local links, '
          f'{len(snapshot["source_id_routes"])} D-007 and '
          f'{len(snapshot.get("v17_source_routes", {}))} v17 source-ID routes.')
    for error in errors:
        print('ERROR: ' + error)
    print('PASS' if not errors else f'FAIL: {len(errors)} issues')
    return bool(errors)


if __name__ == '__main__':
    sys.exit(main())
