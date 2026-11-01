#!/usr/bin/env python3
"""The scheduled re-research of the standards and obligations registers (NET-STD and COMPLY.0).

A register's `review` holds a calendar (the IETF meetings for the standards domain; the quarterly reviews, platform
events and dated effective days for the legal domain), the runs recorded so far and `next`, the day
check-registers.review_due computes from them; the guard fails once that day passes. A review run:

  due      prints the next review the register owes, why, and the calendar events still unconfirmed.
  plan     writes the list of primary sources a refresh reads: the RFC Editor record of every RFC, the IETF datatracker
           record of every draft and the IANA registry files holding the register's codepoints, or, for the legal
           domain, the official primary text of every instrument, plus the items that are read by hand. With
           --scan full it adds the full text of every document a full re-research owes a reading of (the documents the
           triggering entries of the light runs since the latest full run pin). Reading them is a download the user
           approves first: the plan is what is approved.
  fetch    reads an approved plan's sources into a directory with a manifest of their SHA-256 digests.
  compare  judges the fetched records against the register and writes a report: the register changes (revisions,
           statuses, codepoints confirmed against IANA with their "Recommended" value), the instruments to read in
           their consolidated text (never read, or changed since the digest recorded when they were read), the
           documents read in full, the ROADMAP amendments it proposes for the user and the sources it could not read.
           With --apply it writes the register changes, records the run and moves `next`; it never edits a ROADMAP
           row. A pinned entry whose status changed, or whose codepoint IANA came to discourage, triggers a full
           re-research. A full scan compares a full plan and, applied, records the re-research document (--ref) that
           cites the report.
  record   records a run made by hand (a full re-research document, or an Official Gazette or EUR-Lex trigger with
           the entries it changed) and moves `next`.
  read     records the consolidated version, day and SHA-256 of the instruments a person read from a fetch the run
           compared (legal domain): the entries' articles were read against their obligations, which the --ref
           document records.

  python scripts/register-review.py due [--register R] [--today YYYY-MM-DD]
  python scripts/register-review.py plan --out sources.json [--scan full]
  python scripts/register-review.py fetch --plan sources.json --into DIR
  python scripts/register-review.py compare --fetched DIR --report docs/sessions/<day>-register-review.md [--apply]
  python scripts/register-review.py compare --scan full --fetched DIR --report R --apply --ref <document#anchor>
  python scripts/register-review.py record --scan full --ref <document#anchor> [--day YYYY-MM-DD] [--trigger ID]
  python scripts/register-review.py --register cmake/legal-register.json read --fetched DIR --ref <document#anchor>
         --entry ID [--entry ID ...]

--register cmake/legal-register.json selects the legal domain.

docs/design/registers.md#snapshot-and-review is the contract. The readers follow the records the first approved
refresh read (2026-10-31); a record that does not have their shape is reported as unreadable and changes nothing.
"""
import argparse
import collections
import datetime
import hashlib
import html
import importlib.util
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAN_SCHEMA = 'cerid-register-sources/1'
FETCH_SCHEMA = 'cerid-register-fetch/1'
MANIFEST = 'manifest.json'
FETCH_LIMIT = 16 * 1024 * 1024
FETCH_TIMEOUT = 60
USER_AGENT = 'cerid-register-review/1'
RFC_EDITOR = 'https://www.rfc-editor.org/rfc/rfc{number}.json'
DATATRACKER = 'https://datatracker.ietf.org/doc/{name}/doc.json'
# The full texts a full re-research reads.
RFC_TEXT = 'https://www.rfc-editor.org/rfc/rfc{number}.txt'
DRAFT_TEXT = 'https://www.ietf.org/archive/id/{name}-{revision}.txt'
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
    'legal': ('https://www.resmigazete.gov.tr/',
              'read the Official Gazette of Turkey, the EUR-Lex Official Journal and the platform announcements since '
              'the latest run for amendments to the registered instruments, and confirm the calendar events marked '
              'unverified'),
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


def source_id(url):
    """A plain identifier for a primary source URL: its host without `www.` and its path and query, slugged."""
    parts = urllib.parse.urlsplit(url)
    host = parts.netloc.lower().removeprefix('www.')
    return re.sub(r'[^a-z0-9.]+', '-', f'{host}{parts.path}?{parts.query}'.lower()).strip('-.')


# The official texts the machine reads (COMPLY.0; the host checks of 2026-11-01): a cited `primary` URL, the page a
# person reads, resolves to the document its host serves as data, read by the host's reader. Where the document's own
# bytes cannot show a later amendment (a dated consolidated version), a version index is read first and the document's
# URL holds the `{version}` it names. A host without a resolver is read as the cited page, by its digest alone.
CELLAR = 'https://publications.europa.eu/resource/celex/{version}'
CELLAR_VERSIONS = 'https://publications.europa.eu/webapi/rdf/sparql?format=application%2Fsparql-results%2Bjson&query='
# The consolidated versions based on the act that have an English XHTML text (a version published with its act has
# none, and CELLAR answers 404 for it). Bound to the act's CELEX number, the query answers in under a second; matching
# CELEX prefixes instead scans the whole store (40 to 120 s, 2026-11-01).
CELLAR_QUERY = ('PREFIX cdm: <http://publications.europa.eu/ontology/cdm#> SELECT DISTINCT ?celex WHERE {{ ?act '
                'cdm:resource_legal_id_celex "{celex}"^^<http://www.w3.org/2001/XMLSchema#string> . ?work '
                'cdm:act_consolidated_based_on_resource_legal ?act ; cdm:resource_legal_id_celex ?celex . ?expression '
                'cdm:expression_belongs_to_work ?work ; cdm:expression_uses_language '
                '<http://publications.europa.eu/resource/authority/language/ENG> . ?manifestation '
                'cdm:manifestation_manifests_expression ?expression ; cdm:manifestation_type ?type . '
                'FILTER(STR(?type) = "xhtml") }}')
EUR_LEX_SECTORS = {'reg': 'R', 'dir': 'L', 'dec': 'D'}
ECFR = 'https://www.ecfr.gov/api/versioner/v1/{endpoint}/title-{title}.{suffix}?{hierarchy}'
MEVZUAT = 'https://www.mevzuat.gov.tr/anasayfa/MevzuatFihristDetayIframe?MevzuatTur={kind}&MevzuatNo={number}' \
          '&MevzuatTertip={tertip}'
# The kinds of mevzuat.gov.tr read besides laws (MevzuatTur=1), with the word their title carries (2026-11-01): an
# institution's by-law or regulation (7, "YÖNETMELİK" or "YÖNETMELİĞİ") and a communiqué (9, "TEBLİĞ").
MEVZUAT_FORMS = {'7': 'YÖNETMELİ', '9': 'TEBLİĞ'}
WASSENAAR_LIST = 'List-of-Dual-Use-Goods-and-Technologies'
# A version a document's URL holds: a CELEX number, a day or an upload path, never a scheme, query or `..`.
VERSION = re.compile(r'(?!.*\.\.)[A-Za-z0-9][A-Za-z0-9._/-]*')


def resolve_mevzuat(match):
    """A law's consolidated text, which names its number. A by-law, regulation or communiqué is known by the Official
    Gazette that published it, which the cited page names (an index read first) and an amended text's closing table
    repeats; any other kind is read by its digest alone."""
    kind, number, tertip = match['kind'], match['number'], match['tertip']
    document = {'id': f'mevzuat-{kind}-{tertip}-{number}', 'kind': 'primary', 'reader': 'mevzuat', 'act': number,
                'url': MEVZUAT.format(kind=kind, number=number, tertip=tertip)}
    if kind == '1':
        return [document]
    if kind not in MEVZUAT_FORMS:
        return [{'id': document['id'], 'kind': 'primary', 'url': document['url']}]
    index = f'{document["id"]}-gazette'
    return [{'id': index, 'kind': 'index', 'reader': 'mevzuat-gazette', 'act': number, 'url': match.string},
            dict(document, form=MEVZUAT_FORMS[kind], index=index)]


def resolve_eur_lex(match):
    """The Publications Office's CELLAR serves EUR-Lex's acts (eur-lex.europa.eu answers a machine with a challenge):
    the consolidated versions of the act's CELEX number, then the latest one in English XHTML."""
    celex = f'3{match["year"]}{EUR_LEX_SECTORS[match["sector"]]}{int(match["number"]):04d}'
    query = urllib.parse.quote(CELLAR_QUERY.format(celex=celex), safe='')
    index = f'celex-{celex.lower()}-versions'
    return [{'id': index, 'kind': 'index', 'reader': 'cellar-versions', 'act': celex, 'url': CELLAR_VERSIONS + query},
            {'id': f'celex-{celex.lower()}', 'kind': 'primary', 'reader': 'cellar', 'act': celex, 'index': index,
             'url': CELLAR, 'headers': {'Accept': 'application/xhtml+xml', 'Accept-Language': 'eng'}}]


def resolve_legislation_gov_uk(match):
    """The point-in-time Crown Legislation Markup of an act (`data.xml`)."""
    path = match['path']
    return [{'id': f'legislation.gov.uk-{path.replace("/", "-")}', 'kind': 'primary', 'reader': 'legislation-gov-uk',
             'act': path, 'url': f'https://www.legislation.gov.uk/{path}/data.xml'}]


def resolve_ecfr(match):
    """The eCFR versioner: the hierarchy's latest amendment day, then its full XML on that day."""
    title, levels = match['title'], match['path'].strip('/').split('/')
    hierarchy = '&'.join(level.replace('-', '=', 1) for level in levels)
    slug = f'ecfr-title-{title}-{"-".join(levels).lower()}'
    act = f'title={title}&{hierarchy}'
    return [{'id': f'{slug}-versions', 'kind': 'index', 'reader': 'ecfr-versions', 'act': act,
             'url': ECFR.format(endpoint='versions', title=title, suffix='json', hierarchy=hierarchy)},
            {'id': slug, 'kind': 'primary', 'reader': 'ecfr', 'act': act, 'index': f'{slug}-versions',
             'url': ECFR.format(endpoint='full/{version}', title=title, suffix='xml', hierarchy=hierarchy)}]


def resolve_wassenaar(match):
    """The control-lists page links the current List of Dual-Use Goods and Technologies, a PDF named by its edition."""
    return [{'id': 'wassenaar-control-lists', 'kind': 'index', 'reader': 'wassenaar-lists', 'act': WASSENAAR_LIST,
             'url': match.string},
            {'id': 'wassenaar-dual-use-list', 'kind': 'primary', 'reader': 'pdf', 'act': WASSENAAR_LIST,
             'index': 'wassenaar-control-lists', 'url': 'https://www.wassenaar.org/wp-content/uploads/{version}'}]


RESOLVERS = (
    (re.compile(r'https://www\.mevzuat\.gov\.tr/mevzuat\?MevzuatNo=(?P<number>\d+)&MevzuatTur=(?P<kind>\d+)'
                r'&MevzuatTertip=(?P<tertip>\d+)'), resolve_mevzuat),
    (re.compile(r'https://eur-lex\.europa\.eu/eli/(?P<sector>reg|dir|dec)/(?P<year>\d{4})/(?P<number>\d+)/oj'),
     resolve_eur_lex),
    (re.compile(r'https://www\.legislation\.gov\.uk/(?P<path>(?:ukpga|uksi|asp|ssi|nia|nisr|asc|wsi)/\d{4}/\d+)'),
     resolve_legislation_gov_uk),
    (re.compile(r'https://www\.ecfr\.gov/current/title-(?P<title>\d+)(?P<path>(?:/(?:subtitle|chapter|subchapter|part)'
                r'-[A-Za-z0-9]+)+)'), resolve_ecfr),
    (re.compile(r'https://www\.wassenaar\.org/control-lists/'), resolve_wassenaar),
)


def resolve_primary(url):
    """The sources the machine reads for an instrument's primary URL, its version index first."""
    for pattern, resolve in RESOLVERS:
        match = pattern.fullmatch(url)
        if match:
            return resolve(match)
    return [{'id': source_id(url), 'kind': 'primary', 'url': url}]


def plan_sources(register, scan='light'):
    """The sources a refresh of this register reads: [{id, kind, url, entries, registries}] and the manual items; a
    full scan adds the full text of every document the owed full re-research reads (`text` sources). A legal
    instrument's sources also carry their reader, the act it expects, the request headers and the version index."""
    sources = {}
    manual = {}

    def add(source_id, kind, url, entry, registry=None, **extra):
        source = sources.setdefault(source_id, dict({'id': source_id, 'kind': kind, 'url': url, 'entries': [],
                                                     'registries': []}, **extra))
        if entry not in source['entries']:
            source['entries'].append(entry)
        if registry and registry not in source['registries']:
            source['registries'].append(registry)

    def by_hand(read, url, entry=None):
        item = manual.setdefault((read, url), {'read': read, 'url': url, 'entries': []})
        if entry and entry not in item['entries']:
            item['entries'].append(entry)

    for entry in register['entries']:
        if 'instrument' in entry:
            for source in resolve_primary(entry['instrument']['primary']):
                add(source.pop('id'), source.pop('kind'), source.pop('url'), entry['id'], **source)
            continue
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
    if scan == 'full':
        entries = {entry['id']: entry for entry in register['entries']}
        for entry_id in owed_triggers(register['review']):
            if entry_id not in entries:
                raise ReviewError(f'a recorded run names the triggering entry {entry_id}, which the register lacks')
            spec = entries[entry_id].get('spec', {})
            kind, ref = spec.get('type'), spec.get('ref', '')
            if kind == 'rfc':
                number = rfc_number(ref)
                add(f'rfc{number}-text', 'text', RFC_TEXT.format(number=number), entry_id)
            elif kind == 'draft':
                if 'revision' not in spec:
                    raise ReviewError(f'entry {entry_id}: the full re-research reads {ref} at its revision; refresh '
                                      'the register first')
                add(f'{ref}-{spec["revision"]}-text', 'text', DRAFT_TEXT.format(name=ref, revision=spec['revision']),
                    entry_id)
            elif kind in MANUAL_SPECS:
                by_hand(f'the full text of {ref}', MANUAL_SPECS[kind][1].format(ref=ref), entry_id)
    return sorted(sources.values(), key=lambda source: source['id']), list(manual.values())


def owed_triggers(review):
    """The entries the owed full re-research reads: the triggers of the light runs no full run has followed."""
    return sorted({entry_id for _, run in _registers.owed_runs(review) for entry_id in run['triggers']})


class Unreadable(Exception):
    """A fetched record without the shape this tool reads."""


class SecureRedirects(urllib.request.HTTPRedirectHandler):
    """Follows a redirect on the same host, or to one of `hosts` when given, over https: CELLAR answers a content
    request with an http Location whose https form serves the same bytes (2026-11-01), so http is upgraded; another
    host or scheme is refused."""

    def __init__(self, hosts=None):
        super().__init__()
        self.hosts = frozenset(hosts or ())

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        old = urllib.parse.urlsplit(req.full_url)
        new = urllib.parse.urlsplit(urllib.parse.urljoin(req.full_url, newurl))
        if new.scheme not in {'http', 'https'} or new.hostname not in (self.hosts or {old.hostname}):
            if fp is not None:
                fp.close()
            raise urllib.error.URLError(f'refused a redirect to {newurl}')
        return super().redirect_request(req, fp, code, msg, headers, new._replace(scheme='https').geturl())


def download(opener, url, headers, limit=None):
    """The body of an https URL, gzip decoded, at most `limit` (FETCH_LIMIT) bytes either way."""
    limit = limit or FETCH_LIMIT
    request = urllib.request.Request(url, headers=dict(headers, **{'User-Agent': USER_AGENT,
                                                                  'Accept-Encoding': 'gzip'}))
    with opener(request, timeout=FETCH_TIMEOUT) as response:
        body = response.read(limit + 1)
        encoding = (getattr(response, 'headers', None) or {}).get('Content-Encoding') or 'identity'
    if len(body) > limit:
        raise Unreadable(f'larger than {limit} bytes')
    encoding = encoding.strip().lower()
    if encoding == 'identity':
        return body
    if encoding != 'gzip':
        raise Unreadable(f'the unrequested content encoding {encoding!r}')
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        body = stream.decompress(body, limit + 1)
    except zlib.error as error:
        raise Unreadable(f'a broken gzip body ({error})') from error
    if len(body) > limit:
        raise Unreadable(f'larger than {limit} bytes')
    if not stream.eof:
        raise Unreadable('a truncated gzip body')
    return body


def fetch_sources(plan, into, opener=None, today=None):
    """Reads every planned source (HTTPS only, gzip decoded, at most FETCH_LIMIT bytes) into `into` with a manifest of
    the SHA-256 of each decoded body; a version index is read before the documents whose URL holds the version it
    names. A source that fails is recorded with its error and read again by the next run."""
    if not isinstance(plan, dict) or plan.get('schema') != PLAN_SCHEMA or not isinstance(plan.get('sources'), list):
        raise ReviewError(f'the plan is not a {PLAN_SCHEMA} document')
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    opener = opener or urllib.request.build_opener(SecureRedirects).open
    into = Path(into)
    into.mkdir(parents=True, exist_ok=True)
    versions, items = {}, []
    for source in sorted(plan['sources'], key=lambda source: source['kind'] != 'index'):
        item = {'id': source['id'], 'url': source['url']}
        items.append(item)
        if 'index' in source:
            if source['index'] not in versions:
                item['error'] = f'its version index {source["index"]} was not read'
                continue
            item.update(url=source['url'].replace('{version}', versions[source['index']]),
                        version=versions[source['index']])
        if not item['url'].startswith('https://') or not re.fullmatch(r'[a-z0-9.-]+', source['id']):
            item['error'] = 'refused: a source is an https URL with a plain identifier'
            continue
        try:
            body = download(opener, item['url'], source.get('headers', {}))
            if source['kind'] == 'index':
                version = INDEX_READERS[source['reader']](body, source, today)
                if not VERSION.fullmatch(version):
                    raise Unreadable(f'the version {version!r} cannot be part of a URL')
                versions[source['id']] = version
        except OSError as error:
            item['error'] = ' '.join(f'{type(error).__name__}: {error}'.split())
            continue
        except Unreadable as error:
            item['error'] = str(error)
            continue
        name = source['id'] + Path(urllib.parse.urlsplit(item['url']).path).suffix
        (into / name).write_bytes(body)
        item.update(file=name, bytes=len(body), sha256=hashlib.sha256(body).hexdigest())
    manifest = {'schema': FETCH_SCHEMA, 'day': str(today), 'plan': plan, 'items': items}
    write_json(into / MANIFEST, manifest)
    return manifest


def text_of(body):
    """The visible text of an HTML page: scripts and styles dropped, tags removed, entities decoded, spaces folded."""
    try:
        text = body.decode('utf-8')
    except UnicodeDecodeError as error:
        raise Unreadable(f'not UTF-8 ({error})') from error
    text = re.sub(r'(?is)<(script|style)\b.*?</\1\s*>', ' ', text)
    return re.sub(r'\s+', ' ', html.unescape(re.sub(r'<[^>]*>', ' ', text)))


def calendar_day(day, month, year):
    try:
        return datetime.date(int(year), int(month), int(day))
    except ValueError as error:
        raise Unreadable(f'the day {day}/{month}/{year} does not exist') from error


# An amendment note of mevzuat.gov.tr's consolidated text (every shape the 2026-11-01 reads found): "(Değişik:2/3/2024-
# 7499/34 md.)", "(Ek cümle: 1/2/2020 – 7226/3 md.)", "(Başlığı ile Birlikte Değişik:...)", "(Mülga: 2/7/2018-KHK-
# 703/163 md.; Aynen kabul: ...)", "(Yeniden düzenleme: ...)", a bare "(1/2/2020-7226/3 md.)", and in a by-law,
# regulation or communiqué the amending instrument's Official Gazette, "(Değişik ibare:RG-23/8/2022-31932)". Each
# day in a note counts, with the act amending on it (`RG <issue>` for a gazette).
MEVZUAT_NOTE = re.compile(r'\(\s*(?:[^():;]{0,40}?\b(?:Değişik|Ek|Mülga|İptal|Yeniden düzenleme)\b[^()]*|'
                          r'\d{1,2}/\d{1,2}/\d{4}\s*[-–][^()]*md\.)\)')
MEVZUAT_DAY = re.compile(r'(?:(RG)\s*-\s*)?(\d{1,2})/(\d{1,2})/(\d{4})(?:\s*[-–]\s*(KHK[-/]\d+|\d+))?')
# A court's annulment, "(İptal: Anayasa Mahkemesinin 1/2/2020 tarihli ve E.: ..., K.: ... sayılı Kararı ile.)" or
# "(Danıştay Onuncu Dairesinin 6/5/2026 tarihli ve E.: ...; K.: ... sayılı kararı ile iptal bent: <the text
# annulled>)", "Tarihli" capitalised in some: only the decision's day counts, since the annulled text it quotes may
# hold other days.
MEVZUAT_COURT = re.compile(r'\(\s*(?:İptal[^:()]*:\s*)?(Anayasa Mahkemesi|Danıştay)\w*\b[^()]*?'
                           r'(\d{1,2})/(\d{1,2})/(\d{4})\s*[Tt]arihli')
# An Official Gazette as mevzuat.gov.tr writes it: the day, the issue and a repeated issue ("33124 (5.Mükerrer)").
GAZETTE_ISSUE = r'(\d+)(?:\s*\(\s*(\d+)\s*\.\s*Mükerrer\s*\))?'
MEVZUAT_CITED_GAZETTE = re.compile(r'Resmî Gazete Tarihi\s*:\s*(\d{1,2})\.(\d{1,2})\.(\d{4})\s*'
                                   r'Resmî Gazete Sayısı\s*:\s*' + GAZETTE_ISSUE)
# The closing table of an amended by-law, regulation or communiqué: its own gazette, then the amending instruments'
# in numbered rows, of which the trailing ones may be empty.
MEVZUAT_PUBLISHED = re.compile(r'Yayımlandığı Resmî Gazete.nin Tarihi Sayısı\s*(\d{1,2})/(\d{1,2})/(\d{4})\s+'
                               + GAZETTE_ISSUE)
MEVZUAT_AMENDING = re.compile(r'Yayımlandığı Resmî Gazetelerin Tarihi Sayısı')
MEVZUAT_ROW = re.compile(r'\s*\d+\s*\.?(?:\s+(\d{1,2})/(\d{1,2})/(\d{4})\s+' + GAZETTE_ISSUE + r')?(?=\s|$)')


def gazette_issue(issue, repeated):
    """An Official Gazette's issue, `-m<n>` for the n-th repeated issue of its day."""
    return issue + (f'-m{repeated}' if repeated else '')


def gazette(day, month, year, issue, repeated):
    """`<day>/<issue>` of an Official Gazette."""
    return f'{calendar_day(day, month, year)}/{gazette_issue(issue, repeated)}'


def mevzuat_changes(text):
    """(day, by) of each change a consolidated text notes: the amending act, `RG <issue>`, or the annulling court."""
    changes = [(calendar_day(*court.group(2, 3, 4)), court.group(1)) for court in MEVZUAT_COURT.finditer(text)]
    for note in MEVZUAT_NOTE.finditer(text):
        if MEVZUAT_COURT.match(note.group(0)):
            continue
        for match in MEVZUAT_DAY.finditer(note.group(0)):
            act = match.group(5) and match.group(5).replace('/', '-')
            changes.append((calendar_day(*match.group(2, 3, 4)), f'RG {act}' if match.group(1) and act else act or ''))
    return changes


def read_mevzuat_gazette(body, source, today):
    """The Official Gazette that published a by-law, regulation or communiqué, which its cited page names."""
    found = {gazette(*match.groups()) for match in MEVZUAT_CITED_GAZETTE.finditer(text_of(body))}
    if len(found) != 1:
        raise Unreadable(f'{len(found)} Official Gazettes of publication on the cited page of No. {source["act"]}, '
                         'not one')
    return found.pop()


def read_mevzuat(body, source, version):
    """A law's version is `<day> (<amending act>)` of its latest change, or `<day> (as enacted)`. A by-law, regulation
    or communiqué (its `form`) is known by its gazette, the `version` its cited page gave."""
    text = text_of(body)
    if 'form' in source:
        return read_mevzuat_instrument(text, source, version)
    if not re.search(rf'Numarası\s*:\s*{source["act"]}\b', text):
        raise Unreadable(f'not the consolidated text of No. {source["act"]}')
    changes = mevzuat_changes(text)
    if changes:
        day, act = max(changes)
        return f'{day} ({act})' if act else str(day)
    enacted = re.search(r'Kabul Tarihi\s*:\s*(\d{1,2})/(\d{1,2})/(\d{4})', text)
    if enacted is None:
        raise Unreadable('neither an amendment note nor the day of enactment')
    return f'{calendar_day(*enacted.groups())} (as enacted)'


def read_mevzuat_instrument(text, source, published):
    """`<gazette> at <day> (<by>)` of the latest change, noted or in the closing table, or `<gazette> as published`;
    the title names the instrument's form, and an amended text's table repeats the gazette of its cited page."""
    if source['form'] not in text[:600]:
        raise Unreadable(f'not a {source["form"]} text: its title does not name one')
    if not published:
        raise Unreadable(f'the Official Gazette of No. {source["act"]} is unknown; its cited page is read first')
    table = MEVZUAT_PUBLISHED.search(text)
    if table and gazette(*table.groups()) != published:
        raise Unreadable(f'published in the Official Gazette {gazette(*table.groups())}, not {published} as its cited '
                         'page says')
    changes = mevzuat_changes(text)
    amending = MEVZUAT_AMENDING.search(text, table.end()) if table else None
    position = amending.end() if amending else None
    while position is not None and (row := MEVZUAT_ROW.match(text, position)) and row.end() > position:
        if row.group(1):
            changes.append((calendar_day(*row.group(1, 2, 3)), f'RG {gazette_issue(*row.group(4, 5))}'))
        position = row.end()
    if not changes:
        return f'{published} as published'
    day, by = max(changes)
    return f'{published} at {day} ({by})'


def read_legislation_gov_uk(body, source, version):
    """The point-in-time day (`dct:valid`) of the act's Crown Legislation Markup."""
    try:
        tree = ElementTree.fromstring(body)
    except ElementTree.ParseError as error:
        raise Unreadable(f'not Crown Legislation Markup ({error})') from error
    identifier = tree.find('.//{http://purl.org/dc/elements/1.1/}identifier')
    valid = tree.find('.//{http://purl.org/dc/terms/}valid')
    if identifier is None or not (identifier.text or '').strip().endswith(f'/{source["act"]}'):
        raise Unreadable(f'not the data of {source["act"]}')
    day = (valid.text or '').strip() if valid is not None else ''
    if len(day) != 10 or _registers.parse_day(day) is None:
        raise Unreadable('no point-in-time day (dct:valid)')
    return day


def read_cellar_versions(body, source, today):
    """The latest consolidated version of a CELEX act in force by `today`, or the act itself when none is."""
    try:
        values = [binding['celex']['value'] for binding in json.loads(body)['results']['bindings']]
    except (ValueError, KeyError, TypeError) as error:
        raise Unreadable(f'not a SPARQL result of CELEX numbers ({error})') from error
    pattern = re.compile(re.escape(f'0{source["act"][1:]}') + r'-(\d{4})(\d{2})(\d{2})')
    versions = []
    for value in values:
        match = pattern.fullmatch(value) if isinstance(value, str) else None
        if match is None:
            raise Unreadable(f'{value!r} is not a consolidated version of {source["act"]}')
        day = calendar_day(match[3], match[2], match[1])
        if day <= today:
            versions.append((day, value))
    return max(versions)[1] if versions else source['act']


def read_cellar(body, source, version):
    """The XHTML of an act names it by year and number (`2016/679`, `2002/58/EC`)."""
    year, number = source['act'][1:5], int(source['act'][6:10])
    text = text_of(body)
    if '<html' not in body[:4096].decode('utf-8', 'replace').lower() or \
            f'{year}/{number}' not in text and f'{number}/{year}' not in text:
        raise Unreadable(f'not the XHTML of {source["act"]}')
    return version


def read_ecfr_versions(body, source, today):
    """The latest amendment day of an eCFR hierarchy, whose `meta` echoes the hierarchy asked for."""
    try:
        meta = json.loads(body)['meta']
        day = meta['latest_amendment_date']
    except (ValueError, KeyError, TypeError) as error:
        raise Unreadable(f'not an eCFR versions record ({error})') from error
    asked = dict(urllib.parse.parse_qsl(source['act']))
    if any(str(meta.get(level)) != value for level, value in asked.items()):
        raise Unreadable(f'the versions of another hierarchy than {source["act"]}')
    if not isinstance(day, str) or len(day) != 10 or _registers.parse_day(day) is None:
        raise Unreadable(f'the latest amendment day {day!r} is not YYYY-MM-DD')
    return day


ECFR_LEVELS = {'title': 'TITLE', 'subtitle': 'SUBTITLE', 'chapter': 'CHAPTER', 'subchapter': 'SUBCHAP', 'part': 'PART'}


def ecfr_record(body, source):
    """The cited hierarchy's subtree of an eCFR document: the versioner's full XML is the part itself for a part, but
    the whole title above one (it ignores subtitle, chapter and subchapter, 2026-11-01), so an amendment elsewhere in
    the title must not change the digest of the text read."""
    try:
        root = ElementTree.fromstring(body)
    except ElementTree.ParseError as error:
        raise Unreadable(f'not eCFR XML ({error})') from error
    levels = urllib.parse.parse_qsl(source['act'])
    node = root
    for index, (level, value) in enumerate(levels):
        wanted = (ECFR_LEVELS.get(level), value)
        found = next((element for element in node.iter() if (element.get('TYPE'), element.get('N')) == wanted), None)
        if found is None and node is root and index + 1 < len(levels):
            continue  # a part's document starts below the title and chapter that hold it
        if found is None:
            raise Unreadable(f'no {level} {value} in the eCFR document')
        node = found
    return ElementTree.tostring(node)


def read_ecfr(body, source, version):
    ecfr_record(body, source)
    return version


def read_wassenaar_lists(body, source, today):
    """The upload path of the one current list the control-lists page links."""
    links = set(re.findall(r'https://www\.wassenaar\.org/wp-content/uploads/(\d{4}/\d{2}/' + re.escape(source['act']) +
                           r'[A-Za-z0-9-]*\.pdf)', body.decode('utf-8', 'replace')))
    if len(links) != 1:
        raise Unreadable(f'{len(links)} links to the {source["act"]} PDF, not one')
    return links.pop()


def read_pdf(body, source, version):
    if not body.startswith(b'%PDF-'):
        raise Unreadable('not a PDF')
    return version


# reader(body, source, version) -> the version of an instrument's document; an index reader(body, source, today)
# -> the version its document's URL holds.
READERS = {'mevzuat': read_mevzuat, 'legislation-gov-uk': read_legislation_gov_uk, 'cellar': read_cellar,
           'ecfr': read_ecfr, 'pdf': read_pdf}
# record(body, source) -> the bytes of the instrument within its document, whose SHA-256 is the text read; the whole
# document for a reader without one.
RECORDS = {'ecfr': ecfr_record}
INDEX_READERS = {'cellar-versions': read_cellar_versions, 'ecfr-versions': read_ecfr_versions,
                 'wassenaar-lists': read_wassenaar_lists, 'mevzuat-gazette': read_mevzuat_gazette}


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


# An unassigned or reserved span of values (`3-255`, `0x00000002-0x0000ffff`), which names nothing.
RANGE = re.compile(r'(?:0x[0-9a-fA-F]+|\d+)\s*-\s*(?:0x[0-9a-fA-F]+|\d+)')


def number(text):
    """The integer a decimal or 0x-prefixed hexadecimal value spells, else None."""
    text = text.strip()
    if re.fullmatch(r'0x[0-9a-fA-F]+', text):
        return int(text, 16)
    return int(text, 10) if re.fullmatch(r'\d+', text) else None


def document_of(kind, ref):
    """The register's spelling of a cited document (`RFC 9000`, a draft name without its revision), else None."""
    if kind == 'rfc':
        cited = rfc_number(ref)
        return None if cited is None else f'RFC {cited}'
    if kind == 'draft' and re.fullmatch(r'draft-[a-z0-9-]+', ref):
        return re.sub(r'-\d{2}$', '', ref)
    return None


def sequence_of(text):
    """The string an identification sequence spells (`0x68 0x33 ("h3")`, the ALPN registry's form), else None."""
    match = re.fullmatch(r'(?:0x[0-9a-fA-F]{2}\s+)+\("([^"]+)"\)', text.strip())
    return match.group(1) if match else None


# One record of an IANA registry: its values, names, cited documents and "Recommended" value (None without one).
IanaRecord = collections.namedtuple('IanaRecord', 'values names documents recommended')


def read_iana(body, titles):
    """{title: [IanaRecord]} of the named registries of an IANA registry file.

    A record's `value` is its number, or its name where the registry keys records by name (ALPN's `HTTP/3`); its
    `description` is its name, or the value an identification sequence spells; `documents` are the RFCs it cites
    (`<xref type="rfc" data="rfc9000"/>`), which confirm a value in a registry whose records carry no name (QUIC
    Versions); `recommended` is the "Recommended" column (RFC 9847), spelled `<rec>` in the TLS Supported Groups and
    ExtensionType files and `<recommended>` in the SignatureScheme file."""
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
            values, names, documents, recommended = set(), set(), set(), None
            for field in record:
                tag, text = local(field.tag), ''.join(field.itertext()).strip()
                if tag == 'value':
                    value = number(text)
                    if value is not None:
                        values.add(value)
                    elif not RANGE.fullmatch(text):
                        names.add(normal_name(text))
                elif tag in {'description', 'name'} and text:
                    sequence = sequence_of(text)
                    if sequence is None:
                        names.add(normal_name(text))
                    else:
                        values.add(sequence)
                elif tag == 'xref':
                    documents.add(document_of(field.get('type'), field.get('data', '')))
                elif tag in {'rec', 'recommended'} and text:
                    recommended = text
            documents.discard(None)
            records.append(IanaRecord(values, names, documents, recommended))
        found[title] = records
    missing = set(titles) - set(found)
    if missing:
        raise Unreadable(f'no registry titled {sorted(missing)}')
    return found


def normal_name(text):
    return re.sub(r'[^a-z0-9]', '', text.lower())


def point_value(point):
    """The value a register codepoint spells: its number, else its text (an ALPN identifier)."""
    value = number(point['value'])
    return point['value'] if value is None else value


def judge_codepoint(point, records, document):
    """('verified' | 'mismatch' | 'name' | 'absent', detail) of a register codepoint against an IANA registry.

    A record that carries no name (QUIC Versions) confirms the value when it cites `document`, the entry's spec."""
    value = point_value(point)
    name = normal_name(point['name'])
    by_value = [(record.names, record.documents) for record in records if value in record.values]
    if by_value:
        if any(name in names for names, _ in by_value):
            return 'verified', ''
        if any(not names and document in documents for names, documents in by_value):
            return 'verified', f' (an unnamed record citing {document})'
        named = sorted(set().union(*(names for names, _ in by_value)))
        cited = sorted(set().union(*(documents for _, documents in by_value)))
        return 'name', f'IANA names {point["value"]} {named} and cites {cited}'
    if any(name in record.names for record in records):
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
        self.readings = []
        self.triggers = []
        self.bodies = {}
        self.items = {item['id']: item for item in manifest['items']}
        for item in manifest['items']:
            if 'error' in item:
                self.unread.append(' '.join(f'{item["id"]} ({item["url"]}): {item["error"]}'.split()))
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
                earlier = ' '.join(([f'-{spec["revision"]}'] if 'revision' in spec else []) +
                                   ([f'posted {spec["posted"]}'] if 'posted' in spec else []))
                was = f'was {earlier}' if earlier else 'no revision recorded'
                self.change(entry, f'{spec["ref"]}-{record["revision"]} posted {record["posted"]} ({was})', apply)
        if record['status'] != spec['status']:
            def apply(target, record=record):
                target['spec']['status'] = record['status']
            self.change(entry, f'{spec["ref"]} is {record["status"]} (was {spec["status"]})', apply)
            self.status_changed(entry, spec['status'], record['status'])

    def judge_codepoints(self, entry, registries):
        document = document_of(entry['spec'].get('type'), entry['spec'].get('ref', ''))
        for index, point in enumerate(entry.get('codepoints', [])):
            if point['registry'] not in IANA_REGISTRIES:
                continue
            name, title = IANA_REGISTRIES[point['registry']]
            records = registries.get(f'iana-{name}', {}).get(title)
            if records is None:
                continue
            verdict, detail = judge_codepoint(point, records, document)
            if verdict == 'verified' and point.get('verified') is False:
                def apply(target, index=index):
                    target['codepoints'][index].pop('verified')
                self.change(entry, f'{point["registry"]} {point["name"]} = {point["value"]} confirmed by IANA{detail}',
                            apply)
            if verdict == 'verified' and point['registry'] in _registers.RECOMMENDED_REGISTRIES:
                self.judge_recommended(entry, index, point, records)
            elif verdict != 'verified':
                self.proposals.append(f'{entry["id"]}: {point["registry"]} {point["name"]} = {point["value"]}: '
                                      f'{detail}; correct the register and regenerate scripts/gen-register-ids.py')

    def judge_recommended(self, entry, index, point, records):
        """IANA's "Recommended" value of a confirmed codepoint (RFC 9847): recorded, and a pinned entry whose
        codepoint IANA came to discourage ("D") triggers a full re-research."""
        value = point_value(point)
        found = {record.recommended for record in records if value in record.values}
        what = f'{point["registry"]} {point["name"]}'
        if len(found) != 1 or found - _registers.RECOMMENDED:
            self.unread.append(f'{entry["id"]}: {what} has no single Recommended value in the IANA file '
                               f'({", ".join(sorted(str(item) for item in found))})')
            return
        recommended, old = found.pop(), point.get('recommended')
        if recommended == old:
            return

        def apply(target, index=index, recommended=recommended):
            target['codepoints'][index]['recommended'] = recommended
        was = f'was "{old}"' if old else 'not recorded'
        self.change(entry, f'{what} is Recommended "{recommended}" by IANA ({was})', apply)
        if recommended == 'D' and entry['state'] in _registers.PINNED:
            self.triggers.append(entry['id'])
            self.proposals.append(f'{entry["id"]} ({", ".join(entry["rows"])}): IANA now discourages {point["name"]} '
                                  '(Recommended "D"); drop it from the accepted policy and the entry before the full '
                                  're-research this triggers')

    def reading(self, source):
        """(version, SHA-256 of the text read) of a fetched instrument's document: the version its host's reader finds
        (None for a host without one) and the digest of the instrument within the document."""
        body = self.bodies[source['id']]
        if 'reader' not in source:
            return None, hashlib.sha256(body).hexdigest()
        version = READERS[source['reader']](body, source, self.items[source['id']].get('version'))
        record = RECORDS[source['reader']](body, source) if source['reader'] in RECORDS else body
        return version, hashlib.sha256(record).hexdigest()

    def judge_primary(self, entry, source):
        """An instrument's document against the version and digest recorded when it was last read in its consolidated
        form: an amendment shows as a new version, a change under the same version as a new digest."""
        try:
            version, digest = self.reading(source)
        except Unreadable as error:
            self.unread.append(f'{source["id"]}: {error}')
            return
        instrument = entry['instrument']
        what = f'{entry["id"]} ({", ".join(entry["rows"])}): {instrument["name"]}, {instrument["article"]}'
        found = f'version {version}, ' if version else ''
        consolidated = instrument['consolidated']
        if consolidated is None:
            self.readings.append(f'{what}: never read from its primary source; read {self.items[source["id"]]["url"]} '
                                 f'({found}SHA-256 {digest}) in its consolidated text and record it with `read`')
        elif version is not None and consolidated['version'] != version:
            self.readings.append(f'{what}: amended to {version} since it was read on {consolidated["read"]} '
                                 f'({consolidated["version"]}); read the amendment, record it with `read` (SHA-256 '
                                 f'{digest}) and record a trigger if it changes a pinned obligation')
        elif consolidated['sha256'] != digest:
            self.readings.append(f'{what}: the primary text changed since it was read on {consolidated["read"]} '
                                 f'({consolidated["version"]}); read the change, record the new version and '
                                 f'SHA-256 {digest}, and record a trigger if it changes a pinned obligation')

    def run(self, plan):
        registries = {}
        for source in plan['sources']:
            if source['kind'] == 'iana' and source['id'] in self.bodies:
                titles = [IANA_REGISTRIES[name][1] for name in source['registries']]
                try:
                    registries[source['id']] = read_iana(self.bodies[source['id']], titles)
                except Unreadable as error:
                    self.unread.append(f'{source["id"]}: {error}')
        sources = {(source['kind'], entry_id): source for source in plan['sources'] for entry_id in source['entries']}
        judges = {'rfc': ('rfc-editor', self.judge_rfc), 'draft': ('datatracker', self.judge_draft)}
        for entry in self.register['entries']:
            if 'instrument' in entry:
                source = sources.get(('primary', entry['id']))
                if source is not None and source['id'] in self.bodies:
                    self.judge_primary(entry, source)
                continue
            source_kind, judge = judges.get(entry['spec'].get('type'), (None, None))
            source_id = sources.get((source_kind, entry['id']), {}).get('id')
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
                    ('Read in the consolidated text', self.readings),
                    ('Proposed ROADMAP amendments', self.proposals),
                    ('Triggers a full re-research', self.triggers),
                    ('Read in full', [f'{", ".join(source["entries"])}: {source["url"]}' for source in plan['sources']
                                      if source['kind'] == 'text']),
                    ('Sources not read', self.unread),
                    ('Read by hand', [f'{item["read"]} ({item["url"]})' +
                                      (f' for {", ".join(item["entries"])}' if item['entries'] else '')
                                      for item in plan['manual']])]
        for title, items in sections:
            lines += [f'## {title}', '']
            lines += [f'- {item}' for item in items] or ['None.']
            lines.append('')
        # The pin of every record read: the bodies stay outside the repository, re-fetched by the same plan.
        lines += ['## Records read', '', f'Retrieved on {self.manifest["day"]} by `scripts/register-review.py fetch`.',
                  '', '| Source | Bytes | SHA-256 |', '|---|---:|---|']
        lines += [f'| [{item["id"]}]({item["url"]}) | {item["bytes"]} | `{item["sha256"]}` |'
                  for item in self.manifest['items'] if 'error' not in item]
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
    sources, manual = plan_sources(register, args.scan)
    plan = {'schema': PLAN_SCHEMA, 'register': args.register, 'planned': str(today_of(args)), 'scan': args.scan,
            'sources': sources, 'manual': manual}
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


def load_manifest(fetched):
    manifest = load_json(Path(fetched) / MANIFEST)
    if not isinstance(manifest, dict) or manifest.get('schema') != FETCH_SCHEMA:
        raise ReviewError(f'{fetched}: no {FETCH_SCHEMA} manifest')
    return manifest


def command_compare(args, root):
    manifest = load_manifest(args.fetched)
    plan = manifest['plan']
    if args.scan == 'full' and plan.get('scan') != 'full':
        raise ReviewError('a full scan compares a plan made with `plan --scan full`, which reads the triggering '
                          'documents in full')
    if args.ref is not None and (args.scan != 'full' or not args.apply):
        raise ReviewError('--ref names the re-research document of an applied full scan; a light run cites its report')
    if args.apply and args.scan == 'full' and args.ref is None:
        raise ReviewError('an applied full scan records its re-research document: --ref <document#anchor>')
    path = root / plan['register']
    register = load_register(path)
    review = Review(register, manifest, args.fetched)
    review.run(plan)
    day = today_of(args)
    report = Path(args.report)
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(review.report(day, args.scan, args.apply, plan), encoding='utf-8', newline='\n')
    print(f'{len(review.changes)} register changes, {len(review.readings)} instruments to read, '
          f'{len(review.proposals)} proposed amendments, {len(review.unread)} sources not read; report {report}')
    if args.apply:
        ref = repository_ref(report, root)
        run_ref = ref if args.ref is None else cited_document(root, args.ref, report)
        entries = {entry['id']: entry for entry in register['entries']}
        for entry_id, _, apply in review.changes:
            apply(entries[entry_id])
        register['snapshot'] = {'date': manifest['day'],
                                'method': f'Refreshed on {manifest["day"]} by scripts/register-review.py from the '
                                          f'approved source plan ({len(review.bodies)} records; report {ref}).'}
        due, scan, reason = record_run(register, day, args.scan, run_ref, review.triggers)
        write_json(path, register)
        print(f'applied; the next review is a {scan} scan due on {due}, after {reason}')
    return 0


def document_ref(root, text):
    """(ref, path) of a run's document named relative to the root with an optional #anchor; it must exist."""
    target, _, anchor = text.partition('#')
    ref = repository_ref(root / target, root) + (f'#{anchor}' if anchor else '')
    if not (root / target).is_file():
        raise ReviewError(f'{target} does not exist; a run cites its document')
    return ref, root / target


def cited_document(root, text, report):
    """The ref of a full re-research document, which must cite the report of the scan it records."""
    ref, path = document_ref(root, text)
    if report.name not in path.read_text(encoding='utf-8'):
        raise ReviewError(f'{ref} does not cite the report {report.name}; the re-research document cites its scan')
    return ref


def command_record(args, root):
    path = root / args.register
    register = load_register(path)
    day = _registers.parse_day(args.day) if args.day else today_of(args)
    if day is None or (args.day and len(args.day) != 10):
        raise ReviewError(f'--day must be YYYY-MM-DD, not {args.day!r}')
    ref, _ = document_ref(root, args.ref)
    unknown = sorted(set(args.trigger) - {entry['id'] for entry in register['entries']})
    if unknown:
        raise ReviewError(f'unknown triggering entries {unknown}')
    due, scan, reason = record_run(register, day, args.scan, ref, args.trigger)
    write_json(path, register)
    triggered = f' with triggers {", ".join(sorted(set(args.trigger)))}' if args.trigger else ''
    print(f'recorded the {args.scan} run of {day}{triggered}; the next review is a {scan} scan due on {due}, '
          f'after {reason}')
    return 0


def command_read(args, root):
    """Records the instruments a person read from a fetch the applied run compared: each entry's consolidated version
    (its host's reader), the fetch day and the SHA-256 of the text read, its source verified and cited by --ref.
    Nothing is written unless every entry can be recorded."""
    manifest = load_manifest(args.fetched)
    plan = manifest['plan']
    path = root / plan['register']
    register = load_register(path)
    if register['domain'] != 'legal':
        raise ReviewError('read records the instruments of the legal register')
    if register['snapshot']['date'] != manifest['day']:
        raise ReviewError(f'the fetch of {manifest["day"]} is not the register\'s snapshot '
                          f'({register["snapshot"]["date"]}); compare --apply it first')
    ref, _ = document_ref(root, args.ref)
    review = Review(register, manifest, args.fetched)
    sources = {entry_id: source for source in plan['sources'] if source['kind'] == 'primary'
               for entry_id in source['entries']}
    entries = {entry['id']: entry for entry in register['entries']}
    errors, readings = [], []
    for entry_id in dict.fromkeys(args.entry):
        entry, source = entries.get(entry_id), sources.get(entry_id)
        if entry is None or 'instrument' not in entry or source is None:
            errors.append(f'{entry_id}: not an instrument this fetch planned')
        elif source['id'] not in review.bodies:
            errors.append(f'{entry_id}: {source["id"]} was not read ({review.items[source["id"]].get("error")})')
        elif 'reader' not in source:
            errors.append(f'{entry_id}: {entry["instrument"]["primary"]} has no reader of its host, so its version is '
                          'unknown; cite the instrument\'s own document on a host with a reader')
        else:
            try:
                readings.append((entry, *review.reading(source)))
            except Unreadable as error:
                errors.append(f'{entry_id}: {source["id"]}: {error}')
    if errors:
        raise ReviewError('nothing recorded: ' + '; '.join(errors))
    for entry, version, digest in readings:
        entry['instrument']['consolidated'] = {'version': version, 'read': manifest['day'], 'sha256': digest}
        entry['source'] = {'ref': ref, 'read': manifest['day'], 'verified': True}
        print(f'{entry["id"]}: read {entry["instrument"]["name"]} at {version} on {manifest["day"]}')
    write_json(path, register)
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
    plan.add_argument('--scan', choices=sorted(_registers.SCANS), default='light',
                      help='full adds the documents the owed full re-research reads in full')
    fetch = commands.add_parser('fetch', help='read an approved plan\'s sources (a download)')
    fetch.add_argument('--plan', required=True)
    fetch.add_argument('--into', required=True)
    compare = commands.add_parser('compare', help='judge fetched records and write the report')
    compare.add_argument('--fetched', required=True)
    compare.add_argument('--report', required=True)
    compare.add_argument('--scan', choices=sorted(_registers.SCANS), default='light')
    compare.add_argument('--apply', action='store_true', help='write the register changes and record the run')
    compare.add_argument('--ref', help='an applied full scan\'s re-research document (citing the report), relative to '
                         'root, with an optional #anchor')
    record = commands.add_parser('record', help='record a run made by hand')
    record.add_argument('--scan', choices=sorted(_registers.SCANS), required=True)
    record.add_argument('--ref', required=True, help='the run\'s document, relative to root, with an optional #anchor')
    record.add_argument('--day', help='the day of the run (default today)')
    record.add_argument('--trigger', action='append', default=[],
                        help='an entry whose status or instrument the run found changed (repeatable)')
    read = commands.add_parser('read', help='record the instruments a person read from the applied fetch')
    read.add_argument('--fetched', required=True)
    read.add_argument('--ref', required=True, help='the document recording the reading, relative to root, with an '
                      'optional #anchor')
    read.add_argument('--entry', action='append', required=True, help='a legal entry whose articles were read '
                      '(repeatable)')
    args = parser.parse_args(argv)
    handlers = {'due': command_due, 'plan': command_plan, 'fetch': command_fetch, 'compare': command_compare,
                'record': command_record, 'read': command_read}
    try:
        return handlers[args.command](args, Path(args.root).resolve())
    except ReviewError as error:
        print(f'register-review: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
