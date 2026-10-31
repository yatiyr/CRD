#!/usr/bin/env python3
"""The scheduled re-research of the standards and obligations registers (NET-STD; COMPLY.0 adds its domain).

A register's `review` holds a calendar (for the standards domain, the IETF meetings), the runs recorded so far and
`next`, the day check-registers.review_due computes from them; the guard fails once that day passes. A review run:

  due      prints the next review the register owes, why, and the calendar events still unconfirmed.
  plan     writes the list of primary sources a refresh reads: the RFC Editor record of every RFC, the IETF datatracker
           record of every draft and the IANA registry files holding the register's codepoints, plus the items that
           are read by hand. Reading them is a download the user approves first: the plan is what is approved.
  fetch    reads an approved plan's sources into a directory with a manifest of their SHA-256 digests.
  compare  judges the fetched records against the register and writes a report: the register changes (revisions,
           statuses, codepoints confirmed against IANA), the ROADMAP amendments it proposes for the user and the
           sources it could not read. With --apply it writes the register changes, records the run and moves
           `next`; it never edits a ROADMAP row. A pinned entry whose status changed triggers a full re-research.
  record   records a run made by hand (a full re-research document) and moves `next`.

  python scripts/register-review.py due [--register R] [--today YYYY-MM-DD]
  python scripts/register-review.py plan --out sources.json
  python scripts/register-review.py fetch --plan sources.json --into DIR
  python scripts/register-review.py compare --fetched DIR --report docs/sessions/<day>-register-review.md [--apply]
  python scripts/register-review.py record --scan full --ref <document#anchor> [--day YYYY-MM-DD]

docs/design/registers.md#snapshot-and-review is the contract. The record shapes below were written from knowledge
of the three services; a record that does not have them is reported as unreadable and changes nothing.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import re
import sys
import urllib.request
import xml.etree.ElementTree as ElementTree
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAN_SCHEMA = 'cerid-register-sources/1'
FETCH_SCHEMA = 'cerid-register-fetch/1'
MANIFEST = 'manifest.json'
FETCH_LIMIT = 16 * 1024 * 1024
FETCH_TIMEOUT = 60
RFC_EDITOR = 'https://www.rfc-editor.org/rfc/rfc{number}.json'
DATATRACKER = 'https://datatracker.ietf.org/doc/{name}/doc.json'
IANA = 'https://www.iana.org/assignments/{name}/{name}.xml'
# The register's registry names, the IANA file holding each and the registry title inside it.
IANA_REGISTRIES = {
    'QUIC Versions': ('quic', 'QUIC Versions'),
    'QUIC Transport Parameters': ('quic', 'QUIC Transport Parameters'),
    'TLS Supported Groups': ('tls-parameters', 'TLS Supported Groups'),
    'TLS SignatureScheme': ('tls-parameters', 'TLS SignatureScheme'),
    'TLS ExtensionType Values': ('tls-extensiontype-values', 'TLS ExtensionType Values'),
    'TLS ALPN Protocol IDs': ('tls-extensiontype-values',
                              'TLS Application-Layer Protocol Negotiation (ALPN) Protocol IDs'),
    'AEAD Algorithms': ('aead-parameters', 'AEAD Algorithms'),
}
# Registries and documents without a machine-readable record: the plan lists them to be read by hand.
MANUAL_REGISTRIES = {
    'Object identifiers': 'https://csrc.nist.gov/projects/computer-security-objects-register/algorithm-registration',
}
MANUAL_SPECS = {
    'w3c': ('the status of {ref} on its W3C Technical Reports page', '{ref}'),
    'nist': ('the status of {ref} on the NIST CSRC publications list', 'https://csrc.nist.gov/publications'),
}
CALENDAR_SOURCE = {
    'standards': ('https://www.ietf.org/meeting/upcoming/',
                  'confirm the calendar events marked unverified and add the next IETF meetings'),
}
# The IETF datatracker's IESG states while a draft is being processed, and the RFC Editor's queue.
IESG_PROCESSING = {'publication requested', 'ad evaluation', 'ad evaluation::revised i-d needed', 'in last call',
                   'last call requested', 'waiting for writeup', 'waiting for ad go-ahead', 'iesg evaluation',
                   'iesg evaluation - defer', 'approved-announcement to be sent', 'approved-announcement sent'}

_spec = importlib.util.spec_from_file_location('check_registers', Path(__file__).resolve().parent /
                                               'check-registers.py')
_registers = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_registers)


class ReviewError(Exception):
    """A refusal the command prints and exits 2 on."""


def load_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise ReviewError(f'{path}: {error}') from error


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')


def load_register(path):
    """A register whose review schedule is well-formed, so review_due can read it."""
    register = load_json(path)
    review = register.get('review') if isinstance(register, dict) else None
    if not isinstance(review, dict) or set(review) != _registers.REVIEW_KEYS or not review['runs'] or \
            not any(run.get('scan') == 'full' for run in review['runs']):
        raise ReviewError(f'{path}: the review schedule is malformed; scripts/check-registers.py names the error')
    return register


def rfc_number(ref):
    match = re.fullmatch(r'RFC ?(\d+)', ref.strip(), re.IGNORECASE)
    return int(match.group(1)) if match else None


def plan_sources(register):
    """The sources a refresh of this register reads: [{id, kind, url, entries, registries}] and the manual items."""
    sources = {}
    manual = {}

    def add(source_id, kind, url, entry, registry=None):
        source = sources.setdefault(source_id, {'id': source_id, 'kind': kind, 'url': url, 'entries': [],
                                                'registries': []})
        if entry not in source['entries']:
            source['entries'].append(entry)
        if registry and registry not in source['registries']:
            source['registries'].append(registry)

    def by_hand(read, url, entry=None):
        item = manual.setdefault((read, url), {'read': read, 'url': url, 'entries': []})
        if entry and entry not in item['entries']:
            item['entries'].append(entry)

    for entry in register['entries']:
        spec = entry.get('spec', {})
        kind, ref = spec.get('type'), spec.get('ref', '')
        if kind == 'rfc':
            number = rfc_number(ref)
            if number is None:
                raise ReviewError(f'entry {entry["id"]}: spec.ref {ref!r} is not "RFC <number>"')
            add(f'rfc{number}', 'rfc-editor', RFC_EDITOR.format(number=number), entry['id'])
        elif kind == 'draft':
            if not re.fullmatch(r'draft-[a-z0-9-]+', ref):
                raise ReviewError(f'entry {entry["id"]}: spec.ref {ref!r} is not a draft name')
            add(ref, 'datatracker', DATATRACKER.format(name=ref), entry['id'])
        elif kind in MANUAL_SPECS:
            read, url = MANUAL_SPECS[kind]
            by_hand(read.format(ref=ref), url.format(ref=ref), entry['id'])
        for point in entry.get('codepoints', []):
            registry = point['registry']
            if registry in IANA_REGISTRIES:
                name = IANA_REGISTRIES[registry][0]
                add(f'iana-{name}', 'iana', IANA.format(name=name), entry['id'], registry)
            elif registry in MANUAL_REGISTRIES:
                by_hand(f'the {registry} values', MANUAL_REGISTRIES[registry], entry['id'])
            elif not registry.startswith('Cerid '):
                raise ReviewError(f'entry {entry["id"]}: registry {registry!r} has no source; add it to '
                                  'IANA_REGISTRIES or MANUAL_REGISTRIES')
    if register['domain'] in CALENDAR_SOURCE:
        url, read = CALENDAR_SOURCE[register['domain']]
        by_hand(read, url)
    return sorted(sources.values(), key=lambda source: source['id']), list(manual.values())


def fetch_sources(plan, into, opener=urllib.request.urlopen, today=None):
    """Reads every planned source (HTTPS only, at most FETCH_LIMIT bytes) into `into` with a manifest; a source that
    fails is recorded with its error and read again by the next run."""
    if not isinstance(plan, dict) or plan.get('schema') != PLAN_SCHEMA or not isinstance(plan.get('sources'), list):
        raise ReviewError(f'the plan is not a {PLAN_SCHEMA} document')
    into = Path(into)
    into.mkdir(parents=True, exist_ok=True)
    items = []
    for source in plan['sources']:
        item = {'id': source['id'], 'url': source['url']}
        if not source['url'].startswith('https://') or not re.fullmatch(r'[a-z0-9.-]+', source['id']):
            item['error'] = 'refused: a source is an https URL with a plain identifier'
            items.append(item)
            continue
        request = urllib.request.Request(source['url'], headers={'User-Agent': 'cerid-register-review/1'})
        try:
            with opener(request, timeout=FETCH_TIMEOUT) as response:
                body = response.read(FETCH_LIMIT + 1)
        except OSError as error:
            item['error'] = f'{type(error).__name__}: {error}'
            items.append(item)
            continue
        if len(body) > FETCH_LIMIT:
            item['error'] = f'larger than {FETCH_LIMIT} bytes'
            items.append(item)
            continue
        name = source['id'] + Path(source['url']).suffix
        (into / name).write_bytes(body)
        item.update(file=name, bytes=len(body), sha256=hashlib.sha256(body).hexdigest())
        items.append(item)
    manifest = {'schema': FETCH_SCHEMA, 'day': str(today or datetime.datetime.now(datetime.timezone.utc).date()),
                'plan': plan, 'items': items}
    write_json(into / MANIFEST, manifest)
    return manifest


class Unreadable(Exception):
    """A fetched record without the shape this tool reads."""


def read_rfc_editor(body):
    """{'status', 'obsoleted_by'} of an RFC Editor record."""
    try:
        record = json.loads(body)
        obsoleted = record['obsoleted_by']
    except (ValueError, KeyError, TypeError) as error:
        raise Unreadable(f'not an RFC Editor record ({error})') from error
    if not isinstance(obsoleted, list):
        raise Unreadable('obsoleted_by is not a list')
    replacements = []
    for value in obsoleted:
        number = rfc_number(str(value))
        if number is None:
            raise Unreadable(f'obsoleted_by holds {value!r}')
        replacements.append(f'RFC {number}')
    return {'status': 'obsoleted' if replacements else 'current', 'obsoleted_by': replacements}


def read_datatracker(body):
    """{'status', 'revision', 'posted', 'rfc'} of an IETF datatracker draft record."""
    try:
        record = json.loads(body)
        revision, state = record['rev'], record['state']
    except (ValueError, KeyError, TypeError) as error:
        raise Unreadable(f'not a datatracker record ({error})') from error
    if not isinstance(revision, str) or not isinstance(state, str):
        raise Unreadable('rev and state are not strings')
    posted = None
    for item in record.get('rev_history') or []:
        if isinstance(item, dict) and item.get('rev') == revision and isinstance(item.get('published'), str):
            posted = item['published'][:10]
    if posted is None and isinstance(record.get('time'), str):
        posted = record['time'][:10]
    rfc = None
    for alias in record.get('aliases') or []:
        if isinstance(alias, str) and re.fullmatch(r'rfc\d+', alias):
            rfc = f'RFC {int(alias[3:])}'
    iesg = (record.get('iesg_state') or '').strip().lower()
    state = state.strip().lower()
    if state == 'rfc':
        status = 'rfc'
    elif state == 'expired':
        status = 'expired'
    elif state == 'replaced':
        status = 'replaced'
    elif record.get('rfceditor_state'):
        status = 'rfc-editor'
    elif iesg in IESG_PROCESSING:
        status = 'iesg'
    elif state == 'active':
        status = 'active'
    else:
        raise Unreadable(f'unknown draft state {state!r}')
    return {'status': status, 'revision': revision, 'posted': _registers.parse_day(posted) and posted, 'rfc': rfc}


def local(tag):
    return tag.rsplit('}', 1)[-1]


def number(text):
    """The integer a decimal or 0x-prefixed hexadecimal value spells, else None."""
    text = text.strip()
    if re.fullmatch(r'0x[0-9a-fA-F]+', text):
        return int(text, 16)
    return int(text, 10) if re.fullmatch(r'\d+', text) else None


def value_of(text):
    """An IANA record's value: its number, or the quoted string of an identification sequence (`0x68 0x33 ("h3")`)."""
    quoted = re.search(r'"([^"]+)"', text)
    return quoted.group(1) if quoted else number(text)


def read_iana(body, titles):
    """{title: [(values, names)]} of the named registries of an IANA registry file."""
    try:
        tree = ElementTree.fromstring(body)
    except ElementTree.ParseError as error:
        raise Unreadable(f'not an IANA registry file ({error})') from error
    found = {}
    for registry in tree.iter():
        if local(registry.tag) != 'registry':
            continue
        title = next(((child.text or '').strip() for child in registry if local(child.tag) == 'title'), '')
        if title not in titles:
            continue
        records = []
        for record in registry:
            if local(record.tag) != 'record':
                continue
            values, names = set(), set()
            for field in record:
                text = ''.join(field.itertext())
                if local(field.tag) in {'value', 'id', 'protocol'}:
                    value = value_of(text)
                    if value is not None:
                        values.add(value)
                if local(field.tag) in {'description', 'name', 'protocol'}:
                    names.add(normal_name(text))
            records.append((values, names))
        found[title] = records
    missing = set(titles) - set(found)
    if missing:
        raise Unreadable(f'no registry titled {sorted(missing)}')
    return found


def normal_name(text):
    return re.sub(r'[^a-z0-9]', '', text.lower())


def judge_codepoint(point, records):
    """('verified' | 'mismatch' | 'name' | 'absent', detail) of a register codepoint against an IANA registry."""
    value = number(point['value'])
    value = point['value'] if value is None else value
    name = normal_name(point['name'])
    by_value = [names for values, names in records if value in values]
    if by_value:
        if any(name in names for names in by_value):
            return 'verified', ''
        return 'name', f'IANA names {point["value"]} {sorted(set().union(*by_value))}'
    if any(name in names for _, names in records):
        return 'mismatch', f'IANA assigns {point["name"]} another value'
    return 'absent', f'IANA has neither {point["name"]} nor {point["value"]}'


class Review:
    """One comparison of fetched records with a register: findings, register changes and amendment proposals."""

    def __init__(self, register, manifest, fetched):
        self.register = register
        self.manifest = manifest
        self.fetched = Path(fetched)
        self.changes = []
        self.proposals = []
        self.unread = []
        self.triggers = []
        self.bodies = {}
        for item in manifest['items']:
            if 'error' in item:
                self.unread.append(f'{item["id"]} ({item["url"]}): {item["error"]}')
                continue
            body = (self.fetched / item['file']).read_bytes()
            if hashlib.sha256(body).hexdigest() != item['sha256']:
                raise ReviewError(f'{item["file"]} does not match the SHA-256 the manifest recorded')
            self.bodies[item['id']] = body

    def change(self, entry, text, apply):
        self.changes.append((entry['id'], text, apply))

    def status_changed(self, entry, old, new):
        if entry['state'] in _registers.PINNED:
            self.triggers.append(entry['id'])
            self.proposals.append(f'{entry["id"]} ({", ".join(entry["rows"])}): the pinned {entry["spec"]["ref"]} is '
                                  f'now {new} (was {old}); amend the rows that implement it before the full '
                                  're-research this triggers')
        else:
            self.proposals.append(f'{entry["id"]}: the watched {entry["spec"]["ref"]} is now {new} (was {old}); '
                                  f'decide whether {", ".join(entry["rows"])} adopts it')

    def judge_rfc(self, entry, source_id):
        record = read_rfc_editor(self.bodies[source_id])
        spec = entry['spec']
        if record['status'] != spec['status'] or record['obsoleted_by'] != spec.get('obsoleted_by', []):
            def apply(target, record=record):
                target['spec']['status'] = record['status']
                if record['obsoleted_by']:
                    target['spec']['obsoleted_by'] = record['obsoleted_by']
                else:
                    target['spec'].pop('obsoleted_by', None)
            detail = f' by {", ".join(record["obsoleted_by"])}' if record['obsoleted_by'] else ''
            self.change(entry, f'{spec["ref"]} is {record["status"]}{detail}', apply)
            if record['status'] != spec['status']:
                self.status_changed(entry, spec['status'], record['status'] + detail)

    def judge_draft(self, entry, source_id):
        record = read_datatracker(self.bodies[source_id])
        spec = entry['spec']
        if record['status'] == 'rfc':
            published = record['rfc'] or 'an RFC'
            self.status_changed(entry, spec['status'], f'published as {published} (re-pin the entry to it with its '
                                'codepoints)')
            return
        if record['revision'] != spec.get('revision') or (record['posted'] and record['posted'] != spec.get('posted')):
            if record['posted'] is None:
                self.unread.append(f'{source_id}: revision {record["revision"]} has no posted date')
            else:
                def apply(target, record=record):
                    target['spec']['revision'] = record['revision']
                    target['spec']['posted'] = record['posted']
                self.change(entry, f'{spec["ref"]}-{record["revision"]} posted {record["posted"]} '
                            f'(was -{spec.get("revision", "?")})', apply)
        if record['status'] != spec['status']:
            def apply(target, record=record):
                target['spec']['status'] = record['status']
            self.change(entry, f'{spec["ref"]} is {record["status"]} (was {spec["status"]})', apply)
            self.status_changed(entry, spec['status'], record['status'])

    def judge_codepoints(self, entry, registries):
        for index, point in enumerate(entry.get('codepoints', [])):
            if point['registry'] not in IANA_REGISTRIES:
                continue
            name, title = IANA_REGISTRIES[point['registry']]
            records = registries.get(f'iana-{name}', {}).get(title)
            if records is None:
                continue
            verdict, detail = judge_codepoint(point, records)
            if verdict == 'verified' and point.get('verified') is False:
                def apply(target, index=index):
                    target['codepoints'][index].pop('verified')
                self.change(entry, f'{point["registry"]} {point["name"]} = {point["value"]} confirmed by IANA', apply)
            elif verdict != 'verified':
                self.proposals.append(f'{entry["id"]}: {point["registry"]} {point["name"]} = {point["value"]}: '
                                      f'{detail}; correct the register and regenerate scripts/gen-register-ids.py')

    def run(self, plan):
        registries = {}
        for source in plan['sources']:
            if source['kind'] == 'iana' and source['id'] in self.bodies:
                titles = [IANA_REGISTRIES[name][1] for name in source['registries']]
                try:
                    registries[source['id']] = read_iana(self.bodies[source['id']], titles)
                except Unreadable as error:
                    self.unread.append(f'{source["id"]}: {error}')
        sources = {(source['kind'], entry_id): source['id'] for source in plan['sources']
                   for entry_id in source['entries']}
        judges = {'rfc': ('rfc-editor', self.judge_rfc), 'draft': ('datatracker', self.judge_draft)}
        for entry in self.register['entries']:
            source_kind, judge = judges.get(entry['spec'].get('type'), (None, None))
            source_id = sources.get((source_kind, entry['id']))
            if source_id in self.bodies:
                try:
                    judge(entry, source_id)
                except Unreadable as error:
                    self.unread.append(f'{source_id}: {error}')
            self.judge_codepoints(entry, registries)

    def report(self, day, scan, applied, plan):
        lines = [f'# Register review of {day} ({scan} scan, {self.register["domain"]} register)', '',
                 '<!-- doc-role: record -->', '',
                 f'Sources: {len(self.manifest["items"])} planned, {len(self.bodies)} read on {self.manifest["day"]}; '
                 f'the register changes below were {"applied" if applied else "proposed, not applied"}. '
                 'This review edits no ROADMAP row: the amendments are proposals for the user.', '']
        sections = [('Register changes', [f'{entry}: {text}' for entry, text, _ in self.changes]),
                    ('Proposed ROADMAP amendments', self.proposals),
                    ('Triggers a full re-research', self.triggers),
                    ('Sources not read', self.unread),
                    ('Read by hand', [f'{item["read"]} ({item["url"]})' +
                                      (f' for {", ".join(item["entries"])}' if item['entries'] else '')
                                      for item in plan['manual']])]
        for title, items in sections:
            lines += [f'## {title}', '']
            lines += [f'- {item}' for item in items] or ['None.']
            lines.append('')
        return '\n'.join(lines)


def record_run(register, day, scan, ref, triggers):
    """Appends a run and moves `next` to the day it now owes."""
    review = register['review']
    review['runs'].append({'day': str(day), 'scan': scan, 'ref': ref, 'triggers': sorted(set(triggers))})
    due, due_scan, reason = _registers.review_due(review)
    review['next'] = str(due)
    return due, due_scan, reason


def repository_ref(path, root):
    """The repository-relative spelling of a path inside the tree, which a run's ref must be."""
    try:
        return Path(path).resolve().relative_to(root).as_posix()
    except ValueError as error:
        raise ReviewError(f'{path} is outside the repository, so a run cannot cite it') from error


def today_of(args):
    if args.today is None:
        return datetime.datetime.now(datetime.timezone.utc).date()
    day = _registers.parse_day(args.today)
    if day is None or len(args.today) != 10:
        raise ReviewError(f'--today must be YYYY-MM-DD, not {args.today!r}')
    return day


def command_due(args, root):
    register = load_register(root / args.register)
    review = register['review']
    due, scan, reason = _registers.review_due(review)
    print(f'{register["domain"]} register: a {scan} review is due on {due}, after {reason}; '
          f'next records {review["next"]}')
    for event in review['calendar']:
        if not event['verified']:
            print(f'  calendar: {event["event"]} on {event["day"]} is unverified; the next review confirms it')
    if due < today_of(args):
        print('  the review is overdue: plan, fetch and compare now')
    return 0


def command_plan(args, root):
    register = load_register(root / args.register)
    sources, manual = plan_sources(register)
    plan = {'schema': PLAN_SCHEMA, 'register': args.register, 'planned': str(today_of(args)), 'sources': sources,
            'manual': manual}
    write_json(args.out, plan)
    print(f'{len(sources)} sources to download once the user approves them, {len(manual)} items read by hand:')
    for source in sources:
        print(f'  {source["url"]}')
    return 0


def command_fetch(args, root):
    manifest = fetch_sources(load_json(args.plan), args.into, today=today_of(args))
    failed = [item for item in manifest['items'] if 'error' in item]
    print(f'{len(manifest["items"]) - len(failed)} of {len(manifest["items"])} sources read into {args.into}')
    for item in failed:
        print(f'  not read: {item["id"]}: {item["error"]}')
    return 0


def command_compare(args, root):
    manifest = load_json(Path(args.fetched) / MANIFEST)
    if not isinstance(manifest, dict) or manifest.get('schema') != FETCH_SCHEMA:
        raise ReviewError(f'{args.fetched}: no {FETCH_SCHEMA} manifest')
    plan = manifest['plan']
    path = root / plan['register']
    register = load_register(path)
    review = Review(register, manifest, args.fetched)
    review.run(plan)
    day = today_of(args)
    report = Path(args.report)
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(review.report(day, args.scan, args.apply, plan), encoding='utf-8', newline='\n')
    print(f'{len(review.changes)} register changes, {len(review.proposals)} proposed amendments, '
          f'{len(review.unread)} sources not read; report {report}')
    if args.apply:
        ref = repository_ref(report, root)
        entries = {entry['id']: entry for entry in register['entries']}
        for entry_id, _, apply in review.changes:
            apply(entries[entry_id])
        register['snapshot'] = {'date': manifest['day'],
                                'method': f'Refreshed on {manifest["day"]} by scripts/register-review.py from the '
                                          f'approved source plan ({len(review.bodies)} records; report {ref}).'}
        due, scan, reason = record_run(register, day, args.scan, ref, review.triggers)
        write_json(path, register)
        print(f'applied; the next review is a {scan} scan due on {due}, after {reason}')
    return 0


def command_record(args, root):
    path = root / args.register
    register = load_register(path)
    day = _registers.parse_day(args.day) if args.day else today_of(args)
    if day is None or (args.day and len(args.day) != 10):
        raise ReviewError(f'--day must be YYYY-MM-DD, not {args.day!r}')
    target, _, anchor = args.ref.partition('#')
    ref = repository_ref(root / target, root) + (f'#{anchor}' if anchor else '')
    if not (root / target).is_file():
        raise ReviewError(f'{target} does not exist; a run cites its document')
    due, scan, reason = record_run(register, day, args.scan, ref, [])
    write_json(path, register)
    print(f'recorded the {args.scan} run of {day}; the next review is a {scan} scan due on {due}, after {reason}')
    return 0


def main(argv=None):
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description='Scheduled re-research of the standards and obligations registers')
    parser.add_argument('--root', default=str(ROOT), help='repository root (tests use a fixture)')
    parser.add_argument('--register', default=_registers.REGISTERS['standards'], help='register, relative to root')
    parser.add_argument('--today', help='act as of this YYYY-MM-DD day instead of the UTC date')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('due', help='print the next review the register owes')
    plan = commands.add_parser('plan', help='write the sources a refresh reads, for the user to approve')
    plan.add_argument('--out', required=True)
    fetch = commands.add_parser('fetch', help='read an approved plan\'s sources (a download)')
    fetch.add_argument('--plan', required=True)
    fetch.add_argument('--into', required=True)
    compare = commands.add_parser('compare', help='judge fetched records and write the report')
    compare.add_argument('--fetched', required=True)
    compare.add_argument('--report', required=True)
    compare.add_argument('--scan', choices=sorted(_registers.SCANS), default='light')
    compare.add_argument('--apply', action='store_true', help='write the register changes and record the run')
    record = commands.add_parser('record', help='record a run made by hand')
    record.add_argument('--scan', choices=sorted(_registers.SCANS), required=True)
    record.add_argument('--ref', required=True, help='the run\'s document, relative to root, with an optional #anchor')
    record.add_argument('--day', help='the day of the run (default today)')
    args = parser.parse_args(argv)
    handlers = {'due': command_due, 'plan': command_plan, 'fetch': command_fetch, 'compare': command_compare,
                'record': command_record}
    try:
        return handlers[args.command](args, Path(args.root).resolve())
    except ReviewError as error:
        print(f'register-review: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
