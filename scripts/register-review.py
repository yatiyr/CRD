#!/usr/bin/env python3
"""The scheduled re-research of the standards and obligations registers (NET-STD and COMPLY.0).

A register's `review` holds a calendar (the IETF meetings for the standards domain; the quarterly reviews, platform
events and dated effective days for the legal domain), the runs recorded so far and `next`, the day
check-registers.review_due computes from them; the guard fails once that day passes. A review run:

  due      prints the next review the register owes, why, the calendar events still unconfirmed and the instruments
           the scans still owed read.
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
           document records; each is dropped from the calendar events that schedule its reading.

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
import http.client
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
    'TLS Alerts': ('tls-parameters', 'TLS Alerts'),
    'TLS ExtensionType Values': ('tls-extensiontype-values', 'TLS ExtensionType Values'),
    'TLS Certificate Compression Algorithm IDs': ('tls-extensiontype-values',
                                                  'TLS Certificate Compression Algorithm IDs'),
    'TLS ALPN Protocol IDs': ('tls-extensiontype-values',
                              'TLS Application-Layer Protocol Negotiation (ALPN) Protocol IDs'),
    'AEAD Algorithms': ('aead-parameters', 'AEAD Algorithms'),
    'HPKE KEM Identifiers': ('hpke', 'HPKE KEM Identifiers'),
    'HPKE KDF Identifiers': ('hpke', 'HPKE KDF Identifiers'),
    'HPKE AEAD Identifiers': ('hpke', 'HPKE AEAD Identifiers'),
    'MLS Cipher Suites': ('mls', 'MLS Cipher Suites'),
}
# Registries and documents without a machine-readable record: the plan lists them to be read by hand.
MANUAL_REGISTRIES = {
    'Object identifiers': 'https://csrc.nist.gov/projects/computer-security-objects-register/algorithm-registration',
}
MANUAL_SPECS = {
    'w3c': ('the status of {ref} on its W3C Technical Reports page', '{ref}'),
    'nist': ('the status of {ref} on the NIST CSRC publications list', 'https://csrc.nist.gov/publications'),
    'paper': ('whether the paper {ref} was withdrawn or superseded', '{ref}'),
    'freedesktop': ('the version and status of {ref} on specifications.freedesktop.org', '{ref}'),
    'tcg': ('the newest published version of {ref} on trustedcomputinggroup.org', '{ref}'),
    'oasis': ('the newest OASIS Standard of {ref} on docs.oasis-open.org', '{ref}'),
}
CALENDAR_SOURCE = {
    'standards': ('https://www.ietf.org/meeting/upcoming/',
                  'confirm the calendar events marked unverified and add the next IETF meetings'),
    'legal': ('https://www.resmigazete.gov.tr/',
              'read the Official Gazette of Turkey, the EUR-Lex Official Journal, the Federal Register (new Executive '
              'orders and OFAC determinations under the orders read) and the platform announcements since the latest '
              'run for amendments to the registered instruments, and confirm the calendar events marked unverified'),
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
CELLAR_HEADERS = {'Accept': 'application/xhtml+xml', 'Accept-Language': 'eng'}
CELLAR_ENGLISH = '<http://publications.europa.eu/resource/authority/language/ENG>'
# The consolidated versions based on the act that have an English XHTML text (a version published with its act has
# none, and CELLAR answers 404 for it), then the acts that change its English text, each with its day and the
# consolidated versions of the act that consolidate it: an act amending it, correcting it, or correcting an act that
# amends it (a corrigendum of an amending regulation), with an English text (a corrigendum of another language leaves
# the English text read unchanged). A consolidated version lists only the acts it adds, so an act is in the version
# read when any version up to it consolidates it. Bound to the act's CELEX number, the query answers in under a
# second; matching CELEX prefixes instead scans the whole store (40 to 120 s, 2026-11-01).
CELLAR_QUERY = ('PREFIX cdm: <http://publications.europa.eu/ontology/cdm#> SELECT DISTINCT ?celex ?day ?by WHERE {{ '
                '?act cdm:resource_legal_id_celex "{celex}"^^<http://www.w3.org/2001/XMLSchema#string> . {{ ?work '
                'cdm:act_consolidated_based_on_resource_legal ?act ; cdm:resource_legal_id_celex ?celex . ?expression '
                'cdm:expression_belongs_to_work ?work ; cdm:expression_uses_language ' + CELLAR_ENGLISH + ' . '
                '?manifestation cdm:manifestation_manifests_expression ?expression ; cdm:manifestation_type ?type . '
                'FILTER(STR(?type) = "xhtml") }} UNION {{ {{ ?change cdm:resource_legal_amends_resource_legal ?act }} '
                'UNION {{ ?change cdm:resource_legal_corrects_resource_legal ?act }} UNION {{ ?change '
                'cdm:resource_legal_corrects_resource_legal ?amending . ?amending '
                'cdm:resource_legal_amends_resource_legal ?act }} ?change cdm:resource_legal_id_celex ?celex ; '
                'cdm:work_date_document ?day . ?text cdm:expression_belongs_to_work ?change ; '
                'cdm:expression_uses_language ' + CELLAR_ENGLISH + ' . OPTIONAL {{ ?consolidation '
                'cdm:act_consolidated_based_on_resource_legal ?act ; cdm:act_consolidated_consolidates_resource_legal '
                '?change ; cdm:resource_legal_id_celex ?by }} }} }}')
EUR_LEX_SECTORS = {'reg': 'R', 'dir': 'L', 'dec': 'D'}
ECFR = 'https://www.ecfr.gov/api/versioner/v1/{endpoint}/title-{title}.{suffix}?{hierarchy}'
MEVZUAT = 'https://www.mevzuat.gov.tr/anasayfa/MevzuatFihristDetayIframe?MevzuatTur={kind}&MevzuatNo={number}' \
          '&MevzuatTertip={tertip}'
# The kinds of mevzuat.gov.tr read besides laws (MevzuatTur=1), with the word their title carries (2026-11-01): an
# institution's by-law or regulation (7, "YÖNETMELİK" or "YÖNETMELİĞİ") and a communiqué (9, "TEBLİĞ").
MEVZUAT_FORMS = {'7': 'YÖNETMELİ', '9': 'TEBLİĞ'}
FEDERAL_REGISTER = 'https://www.federalregister.gov/api/v1/documents/{number}.json'
FEDERAL_REGISTER_TEXT = 'https://www.federalregister.gov/documents/full_text/text/{version}.txt'
WASSENAAR_LIST = 'List-of-Dual-Use-Goods-and-Technologies'
SANCTIONS_MAP = 'https://www.sanctionsmap.eu/api/v1/regime'
UK_REGIMES = '/government/collections/uk-sanctions-regimes-under-the-sanctions-act'
GOV_UK_CONTENT = 'https://www.gov.uk/api/content{path}'
UN_LIBRARY = 'https://digitallibrary.un.org/record/{record}'
UN_LIST = 'https://scsanctions.un.org/resources/xml/en/consolidated.xml'
# A version a document's URL holds: a CELEX number, a day or an upload path (a UN file name carries its symbol's
# parentheses), never a scheme, query, escape or `..`.
VERSION = re.compile(r'(?!.*\.\.)[A-Za-z0-9][A-Za-z0-9._/()-]*')


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


def celex_of(sector, year, number):
    """The CELEX number of an act an ELI names: `reg`, 2014, 833 -> `32014R0833`."""
    return f'3{year}{EUR_LEX_SECTORS[sector]}{int(number):04d}'


def resolve_eur_lex(match):
    """The Publications Office's CELLAR serves EUR-Lex's acts (eur-lex.europa.eu answers a machine with a challenge):
    the consolidated versions of the act's CELEX number and the acts changing it, then the latest version in English
    XHTML, and each act that version lacks as published."""
    celex = celex_of(match['sector'], match['year'], match['number'])
    query = urllib.parse.quote(CELLAR_QUERY.format(celex=celex), safe='')
    index = f'celex-{celex.lower()}-versions'
    return [{'id': index, 'kind': 'index', 'reader': 'cellar-versions', 'act': celex, 'url': CELLAR_VERSIONS + query},
            {'id': f'celex-{celex.lower()}', 'kind': 'primary', 'reader': 'cellar', 'act': celex, 'index': index,
             'url': CELLAR, 'headers': dict(CELLAR_HEADERS)}]


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


def resolve_federal_register(match):
    """A Federal Register document (an Executive order, an agency's determination, rule or notice) is never
    consolidated: an amendment is a document of its own. Its API record is read first: it names the document's raw
    text, and an Executive order's notes name the orders amending or revoking it."""
    number = match['number']
    document = f'federalregister-{number.lower()}'
    return [{'id': f'{document}-record', 'kind': 'index', 'reader': 'federal-register-record', 'act': number,
             'url': FEDERAL_REGISTER.format(number=number)},
            {'id': document, 'kind': 'primary', 'reader': 'federal-register', 'act': number,
             'index': f'{document}-record', 'url': FEDERAL_REGISTER_TEXT}]


def resolve_wassenaar(match):
    """The control-lists page links the current List of Dual-Use Goods and Technologies, a PDF named by its edition."""
    return [{'id': 'wassenaar-control-lists', 'kind': 'index', 'reader': 'wassenaar-lists', 'act': WASSENAAR_LIST,
             'url': match.string},
            {'id': 'wassenaar-dual-use-list', 'kind': 'primary', 'reader': 'pdf', 'act': WASSENAAR_LIST,
             'index': 'wassenaar-control-lists', 'url': 'https://www.wassenaar.org/wp-content/uploads/{version}'}]


def resolve_sanctions_map(match):
    """The EU Sanctions Map's regime index, the data behind the map's page (a script shell): every regime of EU
    restrictive measures with its country, legal acts and measures."""
    return [{'id': 'sanctionsmap-regimes', 'kind': 'primary', 'reader': 'sanctions-map', 'act': 'regime',
             'url': match.string, 'headers': {'Accept': 'application/json'}}]


def resolve_uk_regimes(match):
    """The FCDO's collection of the current UK sanctions regimes, read through gov.uk's content API: it names each
    regime's documents, read after it."""
    return [{'id': 'gov.uk-uk-sanctions-regimes', 'kind': 'primary', 'reader': 'gov-uk-regimes', 'act': UK_REGIMES,
             'url': GOV_UK_CONTENT.format(path=UK_REGIMES), 'headers': {'Accept': 'application/json'}}]


def resolve_un_library(match):
    """A Security Council resolution by its record in the UN Digital Library (the Dag Hammarskjöld Library's catalogue
    of UN documents): the record (MARC XML) is read first and names the symbol, the adoption day and the English
    text's file and size; the text is that PDF. A resolution is never consolidated: a later resolution renewing,
    modifying or lifting its measures is an instrument of its own."""
    record = match['record']
    document = f'un-library-{record}'
    return [{'id': f'{document}-record', 'kind': 'index', 'reader': 'un-library-record', 'act': record,
             'url': UN_LIBRARY.format(record=record) + '?of=xm'},
            {'id': document, 'kind': 'primary', 'reader': 'un-resolution', 'act': record, 'index': f'{document}-record',
             'url': UN_LIBRARY.format(record='{version}')}]


def resolve_un_list(match):
    """The UN Security Council Consolidated List, the text of the umbrella entry `int.un-sanctions`: its regimes (the
    list types of its parties) index the Council's sanctions regimes. The official host answers with a signed link to
    its file store, which the register's `sanctions_lists` names as the list's delivery host."""
    return [{'id': 'un-sc-consolidated-list', 'kind': 'primary', 'reader': 'un-sc-list', 'act': 'CONSOLIDATED_LIST',
             'url': match.string}]


def resolve_gazette(match):
    """An item of the Official Gazette of Turkey (a Presidential decision is published there alone, as a PDF): its
    day's page, which names the gazette and the item's title, is read first, as an index of this item alone (the
    index carries the item it looks for, so two items of one day never share one)."""
    stem, item = match['stem'], f'{match["stem"]}-{match["item"]}.{match["format"]}'
    document = f'resmigazete-{stem.lower()}-{match["item"]}'
    return [{'id': f'{document}-day', 'kind': 'index', 'reader': 'gazette-day', 'act': item,
             'url': f'https://www.resmigazete.gov.tr/eskiler/{match["year"]}/{match["month"]}/{stem}.htm'},
            {'id': document, 'kind': 'primary', 'reader': 'gazette', 'act': item, 'index': f'{document}-day',
             'url': match.string}]


RESOLVERS = (
    (re.compile(r'https://www\.mevzuat\.gov\.tr/mevzuat\?MevzuatNo=(?P<number>\d+)&MevzuatTur=(?P<kind>\d+)'
                r'&MevzuatTertip=(?P<tertip>\d+)'), resolve_mevzuat),
    (re.compile(r'https://eur-lex\.europa\.eu/eli/(?P<sector>reg|dir|dec)/(?P<year>\d{4})/(?P<number>\d+)/oj'),
     resolve_eur_lex),
    (re.compile(r'https://www\.legislation\.gov\.uk/(?P<path>(?:ukpga|uksi|asp|ssi|nia|nisr|asc|wsi|eur)/\d{4}/\d+)'),
     resolve_legislation_gov_uk),
    (re.compile(r'https://www\.ecfr\.gov/current/title-(?P<title>\d+)(?P<path>(?:/(?:subtitle|chapter|subchapter|part)'
                r'-[A-Za-z0-9]+)+)'), resolve_ecfr),
    (re.compile(r'https://www\.federalregister\.gov/d/(?P<number>(?:\d{2}|\d{4}|E\d)-\d{3,5})'),
     resolve_federal_register),
    (re.compile(r'https://www\.wassenaar\.org/control-lists/'), resolve_wassenaar),
    (re.compile(re.escape(SANCTIONS_MAP)), resolve_sanctions_map),
    (re.compile(re.escape(f'https://www.gov.uk{UK_REGIMES}')), resolve_uk_regimes),
    (re.compile(r'https://digitallibrary\.un\.org/record/(?P<record>[1-9]\d*)'), resolve_un_library),
    (re.compile(re.escape(UN_LIST)), resolve_un_list),
    (re.compile(r'https://www\.resmigazete\.gov\.tr/eskiler/(?P<year>\d{4})/(?P<month>\d{2})/'
                r'(?P<stem>(?P=year)(?P=month)\d{2}(?:M[1-9]\d*)?)-(?P<item>[1-9]\d*)\.(?P<format>pdf|htm)'),
     resolve_gazette),
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

    # An official list file read as an instrument's text keeps the delivery hosts its sanctions list names.
    deliveries = {config['url']: sorted(config['delivery'])
                  for config in register.get('sanctions_lists', {}).get('lists', {}).values()}
    for entry in register['entries']:
        if 'instrument' in entry:
            for source in resolve_primary(entry['instrument']['primary']):
                if source['url'] in deliveries:
                    source['delivery'] = deliveries[source['url']]
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


def download(opener, url, headers, limit=None, data=None):
    """The body of an https URL (posted `data` when given), gzip decoded, at most `limit` (FETCH_LIMIT) bytes either
    way."""
    limit = limit or FETCH_LIMIT
    request = urllib.request.Request(url, data=data, headers=dict(headers, **{'User-Agent': USER_AGENT,
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
    the SHA-256 of each decoded body; a version index is read before the documents whose URL holds the version (or the
    part of it) it names, and the documents an index names beside the version (the acts a consolidated text lacks; a
    revised UK text is its own index of the acts in force it has not applied) are read after it, each once. A source
    that fails (a refused connection, a timeout, a body cut short) is recorded with its error and read again by the
    next run."""
    if not isinstance(plan, dict) or plan.get('schema') != PLAN_SCHEMA or not isinstance(plan.get('sources'), list):
        raise ReviewError(f'the plan is not a {PLAN_SCHEMA} document')
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    into = Path(into)
    into.mkdir(parents=True, exist_ok=True)
    versions, items = {}, []
    taken = {source['id'] for source in plan['sources']}

    def take(item, headers, index=None, delivery=()):
        """The item's body written beside the manifest, with the version found when it is an index; the error is
        recorded instead. A redirect stays on the item's host, or goes to one of its delivery hosts."""
        if not item['url'].startswith('https://') or not re.fullmatch(r'[a-z0-9.-]+', item['id']):
            item['error'] = 'refused: a source is an https URL with a plain identifier'
            return None
        hosts = {urllib.parse.urlsplit(item['url']).hostname, *delivery} if delivery else None
        try:
            body = download(opener or urllib.request.build_opener(SecureRedirects(hosts)).open, item['url'], headers)
            found = None
            if index is not None:
                found = INDEX_READERS[index['reader']](body, index, today)
                path, version, *documents = (found, found) if isinstance(found, str) else found
                if not VERSION.fullmatch(path):
                    raise Unreadable(f'the version {path!r} cannot be part of a URL')
                found = path, version, documents[0] if documents else []
        except (OSError, http.client.HTTPException) as error:
            item['error'] = ' '.join(f'{type(error).__name__}: {error}'.split())
            return None
        except Unreadable as error:
            item['error'] = str(error)
            return None
        name = item['id'] + Path(urllib.parse.urlsplit(item['url']).path).suffix
        (into / name).write_bytes(body)
        item.update(file=name, bytes=len(body), sha256=hashlib.sha256(body).hexdigest())
        return found

    for source in sorted(plan['sources'], key=lambda source: source['kind'] != 'index'):
        item = {'id': source['id'], 'url': source['url']}
        items.append(item)
        if 'index' in source:
            if source['index'] not in versions:
                item['error'] = f'its version index {source["index"]} was not read'
                continue
            path, version, documents = versions[source['index']]
            item.update(url=source['url'].replace('{version}', path), version=version)
            if documents:
                item['documents'] = [document['id'] for document in documents]
        indexed = source['kind'] == 'index' or source.get('reader') in INDEX_READERS
        found = take(item, source.get('headers', {}), source if indexed else None, source.get('delivery', ()))
        if found:
            versions[source['id']] = found
            if source['kind'] != 'index':
                item['version'] = found[1]
                if found[2]:
                    item['documents'] = [document['id'] for document in found[2]]
            for document in found[2]:
                if document['id'] not in taken:
                    taken.add(document['id'])
                    items.append({key: document[key] for key in ('id', 'url', 'act', 'reader')})
                    take(items[-1], document['headers'])
    manifest = {'schema': FETCH_SCHEMA, 'day': str(today), 'plan': plan, 'items': items}
    write_json(into / MANIFEST, manifest)
    return manifest


def decoded(body, encoding):
    try:
        return body.decode(encoding)
    except (UnicodeDecodeError, LookupError) as error:
        raise Unreadable(f'not {encoding} ({error})') from error


def visible(markup):
    """The visible text of HTML: scripts and styles dropped, tags removed, entities decoded, spaces folded."""
    markup = re.sub(r'(?is)<(script|style)\b.*?</\1\s*>', ' ', markup)
    return re.sub(r'\s+', ' ', html.unescape(re.sub(r'<[^>]*>', ' ', markup))).strip()


def text_of(body):
    """The visible text of a UTF-8 HTML page."""
    return visible(decoded(body, 'UTF-8'))


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


def legislation_markup(body, source):
    """The Crown Legislation Markup of the document of `source["act"]`, which its `dc:identifier` names."""
    try:
        tree = ElementTree.fromstring(body)
    except ElementTree.ParseError as error:
        raise Unreadable(f'not Crown Legislation Markup ({error})') from error
    identifier = tree.find('.//{http://purl.org/dc/elements/1.1/}identifier')
    if identifier is None or not (identifier.text or '').strip().endswith(f'/{source["act"]}'):
        raise Unreadable(f'not the data of {source["act"]}')
    return tree


def legislation_day(tree):
    valid = tree.find('.//{http://purl.org/dc/terms/}valid')
    day = (valid.text or '').strip() if valid is not None else ''
    if len(day) != 10 or _registers.parse_day(day) is None:
        raise Unreadable('no point-in-time day (dct:valid)')
    return day


# The changes legislation.gov.uk has not applied to a revised text (`ukm:UnappliedEffects`, the 2026-11-01 reads): each
# `ukm:UnappliedEffect` names its affecting act (`AffectingURI`, `http://www.legislation.gov.uk/id/<type>/<year>/
# <number>`), whether its editors owe it (`RequiresApplied`: "false" for a conditional amendment that took no effect or
# a commencement) and its `ukm:InForce` days (`Date`, or `Prospective="true"` before an order commences it). An act is
# read as published in the form its type is published in: an Act as enacted, an instrument as made.
LEGISLATION_METADATA = '{http://www.legislation.gov.uk/namespaces/metadata}'
LEGISLATION_PUBLISHED = 'https://www.legislation.gov.uk/{act}/{form}/data.xml'
AFFECTING_ACT = re.compile(r'http://www\.legislation\.gov\.uk/id/(?P<type>[a-z]+)/\d{4}/[1-9]\d*')
PUBLISHED_FORMS = {'ukpga': 'enacted', 'asp': 'enacted', 'asc': 'enacted', 'anaw': 'enacted', 'nia': 'enacted',
                   'uksi': 'made', 'ssi': 'made', 'wsi': 'made', 'nisr': 'made'}


def read_legislation_gov_uk_effects(body, source, today):
    """A revised text is its own version index: its point-in-time day, or `(day, '<day> + <act> + ...', documents)`
    when its editors owe changes in force by `today` that it has not applied (on 2026-11-01 the seven texts read had
    56 unapplied effects, 28 owed, none in force). The version names each affecting act once, and the fetch reads each
    as published."""
    tree = legislation_markup(body, source)
    day, acts = legislation_day(tree), set()
    for effect in tree.iter(f'{LEGISLATION_METADATA}UnappliedEffect'):
        owed = effect.get('RequiresApplied')
        if owed not in ('true', 'false'):
            raise Unreadable(f'an unapplied effect whose RequiresApplied is {owed!r}')
        if owed == 'false':
            continue
        in_force = list(effect.iter(f'{LEGISLATION_METADATA}InForce'))
        if not in_force:
            raise Unreadable(f'the unapplied effect {effect.get("EffectId")} has no in-force day')
        dates = [ISO_DAY.fullmatch(entry.get('Date')) for entry in in_force if entry.get('Date') is not None]
        if None in dates:
            raise Unreadable(f'the unapplied effect {effect.get("EffectId")} has an in-force day that is not '
                             'YYYY-MM-DD')
        if not any(calendar_day(date[3], date[2], date[1]) <= today for date in dates):
            continue
        affecting = AFFECTING_ACT.fullmatch(effect.get('AffectingURI') or '')
        if affecting is None or affecting['type'] not in PUBLISHED_FORMS:
            raise Unreadable(f'the unapplied effect {effect.get("EffectId")} names {effect.get("AffectingURI")!r}, '
                             'not an act whose published form is known')
        acts.add(affecting[0].removeprefix('http://www.legislation.gov.uk/id/'))
    if not acts:
        return day
    acts = sorted(acts)
    forms = [PUBLISHED_FORMS[act.split('/')[0]] for act in acts]
    return day, ' + '.join([day] + acts), [
        {'id': f'legislation.gov.uk-{act.replace("/", "-")}-{form}', 'act': f'{act}/{form}', 'headers': {},
         'reader': 'legislation-gov-uk-published', 'url': LEGISLATION_PUBLISHED.format(act=act, form=form)}
        for act, form in zip(acts, forms)]


def read_legislation_gov_uk(body, source, version):
    """The point-in-time day (`dct:valid`) of the act's Crown Legislation Markup, with the owed changes in force its
    fetch named (`2026-09-30 + ukpga/2026/20`)."""
    day = legislation_day(legislation_markup(body, source))
    if version is not None and version.split(' + ')[0] != day:
        raise Unreadable(f'the version {version} is not of the text read, valid on {day}')
    return version or day


def read_legislation_gov_uk_published(body, source, version):
    """An act as published (`ukpga/2026/20/enacted`, `uksi/2026/386/made`): its final text, which names it."""
    status = legislation_markup(body, source).find(f'.//{LEGISLATION_METADATA}DocumentStatus')
    if status is None or status.get('Value') != 'final':
        raise Unreadable(f'not the final text of {source["act"]}')
    return version


# An act changing a CELEX act: a regulation, directive or decision of the CELEX sector of legislation, or one of its
# corrigenda (`32026R0506R(03)`).
CELLAR_CHANGE = re.compile(r'3\d{4}[RLD]\d{4}(?:R\(\d{2}\))?')
ISO_DAY = re.compile(r'(\d{4})-(\d{2})-(\d{2})')


def read_cellar_versions(body, source, today):
    """The latest consolidated version of a CELEX act in force by `today` (the act itself when none is), and the acts
    changing its English text that this version lacks: those no consolidated version up to it consolidates, of which
    CELLAR consolidates in no version at all only the acts dated after it are counted (on 2026-11-01 CELLAR recorded
    no consolidation of 3 corrigenda of 833/2014's amending acts, 5 of 765/2006's and 7 of 36/2012's, dated 2011-2025,
    years before the versions read, which are taken as a gap of its metadata). The version, or `(version, '<version> +
    <act> + ...', documents)` when it lacks an act: the version named names each, and the fetch reads each document as
    published."""
    try:
        rows = [{name: value['value'] for name, value in binding.items()}
                for binding in json.loads(body)['results']['bindings']]
    except (ValueError, KeyError, TypeError, AttributeError) as error:
        raise Unreadable(f'not a SPARQL result of CELEX numbers ({error})') from error
    pattern = re.compile(re.escape(f'0{source["act"][1:]}') + r'-(\d{4})(\d{2})(\d{2})')

    def consolidated(value):
        match = pattern.fullmatch(value) if isinstance(value, str) else None
        if match is None:
            raise Unreadable(f'{value!r} is not a consolidated version of {source["act"]}')
        return calendar_day(match[3], match[2], match[1])

    versions, changes = [], {}
    for row in rows:
        if 'day' not in row:
            if set(row) != {'celex'}:
                raise Unreadable(f'a consolidated version with {sorted(row)}')
            day = consolidated(row['celex'])
            if day <= today:
                versions.append((day, row['celex']))
            continue
        celex, day = row.get('celex'), ISO_DAY.fullmatch(row['day']) if isinstance(row['day'], str) else None
        if not isinstance(celex, str) or not CELLAR_CHANGE.fullmatch(celex) or celex == source['act']:
            raise Unreadable(f'{celex!r} is not an act changing {source["act"]}')
        if day is None:
            raise Unreadable(f'{celex} is dated {row["day"]!r}, not YYYY-MM-DD')
        day = calendar_day(day[3], day[2], day[1])
        change = changes.setdefault(celex, (day, []))
        if change[0] != day:
            raise Unreadable(f'{celex} is dated both {change[0]} and {day}')
        if 'by' in row:
            change[1].append(consolidated(row['by']))
    read, version = max(versions) if versions else (None, source['act'])
    lacked = sorted(celex for celex, (day, by) in changes.items()
                    if not any(read and consolidation <= read for consolidation in by) and
                    (by or read is None or day > read))
    if not lacked:
        return version
    return version, ' + '.join([version] + lacked), [
        {'id': f'celex-{re.sub(r"[^a-z0-9]+", "-", celex.lower()).strip("-")}-published', 'act': celex,
         'url': CELLAR.format(version=urllib.parse.quote(celex, safe='')), 'headers': dict(CELLAR_HEADERS),
         'reader': 'cellar'} for celex in lacked]


def read_cellar(body, source, version):
    """The XHTML of an act names it by year and number (`2016/679`, `2002/58/EC`, `No 833/2014`), an act before 1999
    by a two-digit year (`No 3541/92`)."""
    year, number = source['act'][1:5], int(source['act'][6:10])
    names = [f'{year}/{number}', f'{number}/{year}'] + ([f'{number}/{year[2:]}'] if year < '1999' else [])
    named = re.compile(r'(?<![0-9])(?:' + '|'.join(map(re.escape, names)) + r')(?![0-9])')
    if '<html' not in body[:4096].decode('utf-8', 'replace').lower() or not named.search(text_of(body)):
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


# The Federal Register's API record of a document (the 2026-11-01 reads): `document_number`, `publication_date`,
# `citation` (`87 FR 10293`), `raw_text_url` (`.../full_text/text/<year>/<month>/<day>/<number>.txt`), `corrections`
# (the API URLs of the correcting documents, `C1-2023-00593`) and, for an Executive order, `executive_order_notes`,
# whose lines name the orders amending or revoking it ("Amended by: EO 14114, December 12, 2023").
FEDERAL_REGISTER_CHANGE = re.compile(r'^[ \t]*((?:Amended|Revoked|Superseded|Modified) by:.*?)[ \t\r]*$',
                                     re.MULTILINE)
FEDERAL_REGISTER_CORRECTION = 'https://www.federalregister.gov/api/v1/documents/'


def read_federal_register_record(body, source, today):
    """(the raw text's path, the document's version): `<publication day> (<citation>) as published`, then each change
    the record names, an order amending or revoking an Executive order and a correction, so a change reads as a new
    version."""
    try:
        record = json.loads(body)
        number, day, citation = record['document_number'], record['publication_date'], record['citation']
        text, notes, corrections = record['raw_text_url'], record['executive_order_notes'], record['corrections']
    except (ValueError, KeyError, TypeError) as error:
        raise Unreadable(f'not a Federal Register document record ({error})') from error
    if number != source['act']:
        raise Unreadable(f'the record of document {number!r}, not of {source["act"]}')
    if not isinstance(day, str) or len(day) != 10 or _registers.parse_day(day) is None:
        raise Unreadable(f'the publication day {day!r} is not YYYY-MM-DD')
    path = f'{day.replace("-", "/")}/{number}'
    if text != FEDERAL_REGISTER_TEXT.format(version=path):
        raise Unreadable(f'its raw text {text!r} is not the document published on {day}')
    if not isinstance(citation, str) or not re.fullmatch(r'\d+ FR \d+', citation):
        raise Unreadable(f'the citation {citation!r} is not "<volume> FR <page>"')
    if not isinstance(notes, (str, type(None))) or not isinstance(corrections, list) or \
            not all(isinstance(url, str) and url.startswith(FEDERAL_REGISTER_CORRECTION) for url in corrections):
        raise Unreadable('its notes or corrections have another shape')
    changes = [' '.join(change.split()) for change in FEDERAL_REGISTER_CHANGE.findall(notes or '')]
    changes += [f'Corrected by: {url.removeprefix(FEDERAL_REGISTER_CORRECTION)}' for url in corrections]
    return path, '; '.join([f'{day} ({citation}) as published', *changes])


def read_federal_register(body, source, version):
    """The raw text names its document (`[FR Doc No: 2022-04020]`); its version is the record's."""
    if f'[FR Doc No: {source["act"]}]' not in decoded(body, 'UTF-8')[:4096]:
        raise Unreadable(f'not the raw text of Federal Register document {source["act"]}')
    if not version:
        raise Unreadable(f'the record of {source["act"]} is unknown; it is read first')
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


# The Official Gazette's day page (the pages of 2017 and 2025, 2026-11-01): in the charset its meta tag declares
# (windows-1254), headed "25 Aralık 2025 Tarihli ve 33118 Sayılı Resmî Gazete", with "- 5. Mükerrer" on a repeated
# issue, and one link to each item whose text is its title; a decision's title names its number, "(Karar Sayısı:
# 10767)" or "(Kararname Numarası: 191)", its closing parenthesis sometimes missing. An item's file is named by its day,
# its repeated issue and its place: `20251225-21.pdf`, `20251231M5-1.pdf`.
TURKISH_MONTHS = ('Ocak', 'Şubat', 'Mart', 'Nisan', 'Mayıs', 'Haziran', 'Temmuz', 'Ağustos', 'Eylül', 'Ekim', 'Kasım',
                  'Aralık')
PAGE_CHARSET = re.compile(rb'(?i)<meta\b[^>]*\bcharset\s*=\s*["\']?([A-Za-z0-9_-]+)')
GAZETTE_HEADER = re.compile(r'(\d{1,2}) (\w+) (\d{4}) Tarihli ve (\d+) Sayılı Resm[îi] Gazete'
                            r'(?:\s*-\s*(\d+)\s*\.\s*Mükerrer)?')
GAZETTE_ITEM = re.compile(r'(\d{4})(\d{2})(\d{2})(?:M(\d+))?-\d+\.(?:pdf|htm)')
GAZETTE_LINK = re.compile(r'(?is)<a\b[^>]*?\bhref\s*=\s*"([^"]*)"[^>]*>(.*?)</a\s*>')
GAZETTE_NUMBER = re.compile(r'(?:Karar Sayısı|Kararname Numarası)\s*:\s*(\d+)')


def read_gazette_day(body, source, today):
    """`<day>/<issue>` of the gazette whose day page links the item, and `/<number>` when the item's title names a
    decision's: the page must be the item's day and repeated issue, and link the item once."""
    charsets = set(PAGE_CHARSET.findall(body[:4096]))
    if len(charsets) != 1:
        raise Unreadable(f'{len(charsets)} declared charsets on the Official Gazette\'s day page, not one')
    markup = decoded(body, charsets.pop().decode('ascii'))
    headers = set(GAZETTE_HEADER.findall(visible(markup)))
    if len(headers) != 1:
        raise Unreadable(f'{len(headers)} Official Gazette headings on the day page, not one')
    day, month, year, issue, repeated = headers.pop()
    if month not in TURKISH_MONTHS:
        raise Unreadable(f'{month!r} is not a Turkish month')
    published = calendar_day(day, TURKISH_MONTHS.index(month) + 1, year)
    item = GAZETTE_ITEM.fullmatch(source['act'])
    if item is None or calendar_day(*item.group(3, 2, 1)) != published or (item[4] or '') != repeated:
        raise Unreadable(f'the day page of the Official Gazette {gazette_issue(issue, repeated)} of {published}, not '
                         f'of {source["act"]}')
    titles = [visible(title) for href, title in GAZETTE_LINK.findall(markup)
              if href.rsplit('/', 1)[-1] == source['act']]
    if len(titles) != 1:
        raise Unreadable(f'{len(titles)} links to {source["act"]} on its day page, not one')
    numbers = set(GAZETTE_NUMBER.findall(titles[0]))
    if len(numbers) > 1:
        raise Unreadable(f'the title of {source["act"]} names {len(numbers)} decision numbers')
    return '/'.join([str(published), gazette_issue(issue, repeated), *numbers])


def read_gazette(body, source, version):
    """`<day>/<issue> No. <number> as published`: an item of the gazette is never consolidated, so a change is a new
    item, which the review's reading of the gazette finds."""
    if source['act'].endswith('.pdf'):
        read_pdf(body, source, version)
    elif '<html' not in body[:4096].decode('ascii', 'replace').lower():
        raise Unreadable('not an HTML page')
    if not version:
        raise Unreadable(f'the Official Gazette of {source["act"]} is unknown; its day page is read first')
    published, issue, *number = version.split('/')
    return f'{published}/{issue}' + ''.join(f' No. {value}' for value in number) + ' as published'


# The EU Sanctions Map's regime index (`meta.version` "1", read 2026-11-01): `data`, one record per regime with its
# `id`, `country.data` (`{code, title}`, empty for a thematic regime), `legal_acts.data` (each act's `type.data.title`,
# `number` and `url`) and `measures.data` (each measure's `type.data.title`). A Council regulation's basic act is
# numbered "<day> consolidated basic legal act - (EU) No 833/2014" and links its CELEX number or ELI; an act its
# consolidated text lacks says "amendments not yet included". Notes, guidance, icons, descriptions and amendment times
# change without changing which acts carry which measures, so the text read is the regimes' structure alone.
SANCTIONS_MAP_SCHEMA = '1'
BASIC_REGULATION = re.compile(r'E[CU] Council Regulation')
REGULATION_CELEX = re.compile(r'CELEX(?:%3A|:)[03](\d{4})R(\d{4})(?![0-9])|data\.europa\.eu/eli/reg/(\d{4})/(\d{1,4})/')
# The measures that attach to the persons a regime lists or to their claims, which the EU financial sanctions file and
# the regulations' annexes name: a regime with a country and any other measure restricts a supply to a place.
PERSON_MEASURES = frozenset({'Asset freeze and prohibition to make funds available', 'Restrictions on admission',
                             'Prohibition to satisfy claims'})


def sanctions_map_regimes(body):
    """[{id, country, measures, regulations}] of the regime index, by id: the country's code (None for a thematic
    regime), the sorted measure titles and the sorted CELEX numbers of its basic Council regulations."""
    try:
        document = json.loads(body)
        if document['meta']['version'] != SANCTIONS_MAP_SCHEMA:
            raise Unreadable(f'regime index version {document["meta"]["version"]!r}, not {SANCTIONS_MAP_SCHEMA!r}')
        regimes = [{'id': regime['id'], 'country': regime['country']['data'] or None,
                    'measures': sorted({measure['type']['data']['title'] for measure in regime['measures']['data']}),
                    'acts': [(act['type']['data']['title'], act['number'], act['url'])
                             for act in regime['legal_acts']['data']]} for regime in document['data']]
    except (ValueError, KeyError, TypeError, AttributeError) as error:
        raise Unreadable(f'not the EU Sanctions Map regime index ({error!r})') from error
    if not regimes or len({regime['id'] for regime in regimes}) != len(regimes):
        raise Unreadable('a regime index without regimes or with a regime twice')
    for regime in regimes:
        country = regime['country']
        if not isinstance(regime['id'], int) or country is not None and not (
                isinstance(country, dict) and isinstance(country.get('code'), str) and
                re.fullmatch(r'[A-Z]{2}', country['code'])) or \
                not all(isinstance(title, str) and title for title in regime['measures']) or \
                not all(isinstance(part, str) for act in regime['acts'] for part in act):
            raise Unreadable(f'regime {regime["id"]!r} has another shape')
        regime['country'] = country and country['code']
        regulations = set()
        for title, number, url in regime.pop('acts'):
            if not BASIC_REGULATION.fullmatch(title) or 'amendments' in number.lower():
                continue
            match = REGULATION_CELEX.search(url)
            if match is None:
                raise Unreadable(f'regime {regime["id"]}: the regulation {number!r} links no CELEX number or ELI')
            regulations.add(f'3{match[1]}R{match[2]}' if match[1] else celex_of('reg', match[3], match[4]))
        regime['regulations'] = sorted(regulations)
    return sorted(regimes, key=lambda regime: regime['id'])


def place_regime(regime):
    return regime['country'] is not None and not set(regime['measures']) <= PERSON_MEASURES


def read_sanctions_map(body, source, version):
    """`<n> regimes, <m> attached to a place`: the index is one document, never consolidated, so any other change is
    a new digest of the regimes read."""
    regimes = sanctions_map_regimes(body)
    return f'{len(regimes)} regimes, {sum(map(place_regime, regimes))} attached to a place'


def sanctions_map_record(body, source):
    return json.dumps(sanctions_map_regimes(body), sort_keys=True, separators=(',', ':')).encode('utf-8')


# The FCDO's collection of the current UK sanctions regimes, the UK's regime index (read 2026-11-01): gov.uk serves the
# page a person reads as data from its content API. `details.collection_groups` lists content ids by group, of which
# "Regimes listed by country" and "Regimes listed by theme" hold each regime's guidance and its list of designations;
# `links.documents` holds the record of each document gov.uk publishes (a listed id without one is not shown on the
# page: one of the country group on 2026-11-01). A document's `details.body` names the regulation its regime rests on
# by its first legislation.gov.uk link, the summary's opening sentence; a later link is an amending instrument, which
# the revised text applies or names among its unapplied effects, and a list of designations links none or the same.
UK_REGIME_GROUPS = {'Regimes listed by country': 'country', 'Regimes listed by theme': 'theme',
                    'More information': None}
LEGISLATION_LINK = re.compile(r'href="https?://www\.legislation\.gov\.uk/'
                              r'(?P<act>(?:ukpga|uksi|asp|ssi|nia|nisr|asc|wsi)/\d{4}/[1-9]\d*)(?![0-9])')


def gov_uk_record(body, source, document_type=None):
    """The content API record of the gov.uk page `source["act"]` names."""
    try:
        record = json.loads(body)
        found = record['base_path'], record['document_type'], record['details']
    except (ValueError, KeyError, TypeError) as error:
        raise Unreadable(f'not a gov.uk content record ({error!r})') from error
    if found[0] != source['act'] or not isinstance(found[2], dict) or \
            document_type is not None and found[1] != document_type:
        raise Unreadable(f'not the gov.uk {document_type or "page"} {source["act"]}')
    return record


def uk_regime_documents(body, source):
    """(day, {'country': [path, ...], 'theme': [...]}, unpublished) of the regime collection: its public update day,
    the base paths of the documents each regime group lists that gov.uk publishes, sorted, and the number of ids those
    groups list without a published record."""
    collection = gov_uk_record(body, source, 'document_collection')
    try:
        records = {document['content_id']: document['base_path'] for document in collection['links']['documents']}
        groups = {group['title']: group['documents'] for group in collection['details']['collection_groups']}
        day = collection['public_updated_at'][:10]
    except (KeyError, TypeError, AttributeError) as error:
        raise Unreadable(f'not the UK sanctions regimes collection ({error!r})') from error
    if set(groups) != set(UK_REGIME_GROUPS):
        raise Unreadable(f'the collection groups {sorted(groups)}, not {sorted(UK_REGIME_GROUPS)}')
    if not all(isinstance(path, str) and re.fullmatch(r'(?:/[a-z0-9-]+)+', path) for path in records.values()) or \
            not all(isinstance(ids, list) and all(isinstance(each, str) for each in ids) for ids in groups.values()):
        raise Unreadable('a collection document with another shape')
    if _registers.parse_day(day) is None:
        raise Unreadable(f'no public update day ({day!r})')
    regimes, unpublished = {}, 0
    for title, ids in groups.items():
        if UK_REGIME_GROUPS[title] is None:
            continue
        regimes[UK_REGIME_GROUPS[title]] = sorted(records[each] for each in ids if each in records)
        unpublished += sum(each not in records for each in ids)
    if not regimes['country'] or set(regimes['country']) & set(regimes['theme']):
        raise Unreadable('a collection without regimes by country, or with a document in both regime groups')
    return day, regimes, unpublished


def gov_uk_id(path):
    return 'gov.uk' + path.replace('/', '-')


def uk_regimes_version(regimes, unpublished):
    version = (f'{sum(map(len, regimes.values()))} documents: {len(regimes["country"])} by country, '
               f'{len(regimes["theme"])} by theme')
    return version + (f', {unpublished} listed unpublished' if unpublished else '')


def read_uk_regimes_index(body, source, today):
    """The collection is its own index: its public update day, its version and the documents of its regimes, each
    read after it."""
    day, regimes, unpublished = uk_regime_documents(body, source)
    return day, uk_regimes_version(regimes, unpublished), [
        {'id': gov_uk_id(path), 'act': path, 'reader': 'gov-uk-document', 'headers': {'Accept': 'application/json'},
         'url': GOV_UK_CONTENT.format(path=path)} for group in regimes.values() for path in group]


def read_uk_regimes(body, source, version):
    """`<n> documents: <c> by country, <t> by theme`: any other change is a new digest of the regimes read."""
    _, regimes, unpublished = uk_regime_documents(body, source)
    found = uk_regimes_version(regimes, unpublished)
    if version is not None and version != found:
        raise Unreadable(f'the version {version} is not of the collection read, {found}')
    return found


def uk_regimes_record(body, source):
    _, regimes, unpublished = uk_regime_documents(body, source)
    return json.dumps({'regimes': regimes, 'unpublished': unpublished}, sort_keys=True,
                      separators=(',', ':')).encode('utf-8')


def read_gov_uk_document(body, source, version):
    """A regime's document, which names its page."""
    gov_uk_record(body, source)
    return version


def uk_regime_regulation(body, source):
    """The legislation.gov.uk act (`uksi/2019/855`) a regime's document names first, else None."""
    text = gov_uk_record(body, source)['details'].get('body', '')
    if not isinstance(text, str):
        raise Unreadable(f'the gov.uk page {source["act"]} has a body that is no text')
    found = LEGISLATION_LINK.search(text)
    return found and found['act']


# The UN Digital Library's MARC XML record of a document (2026-11-01): `001` its record number, `191 $a` its symbol
# (`S/RES/1718(2006)`, or with a space before the year since 2014), `269 $a` its day and one `856` field per language
# file with `$y` the language, `$u` the file's URL (`record/<n>/files/<name>`, its symbol percent-encoded) and `$s` its
# size; the subfields come in no fixed order.
MARC = '{http://www.loc.gov/MARC21/slim}'
UN_RESOLUTION = re.compile(r'S/RES/([1-9]\d*) ?\((\d{4})\)')
UN_TEXT = re.compile(r'https://digitallibrary\.un\.org/record/(?P<record>[1-9]\d*)/files/(?P<name>[A-Za-z0-9_.%-]+)')


def un_library_record(body, source):
    """(symbol, day, [(language, path, size)]) of the Security Council resolution whose record `source["act"]` names:
    the symbol in its typographic form (`S/RES/1718 (2006)`), and each language file's record path and size."""
    try:
        records = _registers._sanctions.parse_xml(body, 'the UN Digital Library record').findall(MARC + 'record')
    except _registers._sanctions.SanctionsError as error:
        raise Unreadable(str(error)) from error
    if len(records) != 1 or (records[0].findtext(MARC + 'controlfield[@tag="001"]') or '').strip() != source['act']:
        raise Unreadable(f'not the one UN Digital Library record {source["act"]}')
    fields = collections.defaultdict(list)
    for field in records[0].iterfind(MARC + 'datafield'):
        fields[field.get('tag')].append({code.get('code'): (code.text or '').strip()
                                         for code in field.iterfind(MARC + 'subfield')})
    symbols = {each.get('a') for each in fields['191']}
    symbol = UN_RESOLUTION.fullmatch(symbols.pop()) if len(symbols) == 1 else None
    days = {each.get('a') for each in fields['269']}
    day = days.pop() if len(days) == 1 else None
    if symbol is None or day is None or len(day) != 10 or _registers.parse_day(day) is None:
        raise Unreadable(f'record {source["act"]} is no Security Council resolution with one symbol and day')
    files = []
    for each in fields['856']:
        found = UN_TEXT.fullmatch(each.get('u', ''))
        if found is None or found['record'] != source['act'] or not re.fullmatch(r'[1-9]\d*', each.get('s', '')):
            raise Unreadable(f'record {source["act"]} links a file {each.get("u")!r} of another shape')
        path = f'{found["record"]}/files/{urllib.parse.unquote(found["name"])}'
        if not VERSION.fullmatch(path):
            raise Unreadable(f'record {source["act"]} links a file whose name cannot be part of a URL: {path!r}')
        files.append((each.get('y'), path, int(each['s'])))
    return f'S/RES/{symbol[1]} ({symbol[2]})', day, files


def read_un_library_record(body, source, today):
    """The record is the index of its English text: (the text's record path, `S/RES/1718 (2006) of 2006-10-14,
    English text of 38780 bytes`)."""
    symbol, day, files = un_library_record(body, source)
    english = [(path, size) for language, path, size in files if language == 'English']
    if len(english) != 1:
        raise Unreadable(f'record {source["act"]} ({symbol}) has {len(english)} English texts, not one')
    path, size = english[0]
    return path, f'{symbol} of {day}, English text of {size} bytes'


UN_TEXT_VERSION = re.compile(r'S/RES/[1-9]\d* \(\d{4}\) of \d{4}-\d{2}-\d{2}, English text of ([1-9]\d*) bytes')


def read_un_resolution(body, source, version):
    """The English text of a resolution: a PDF of the size its record names (the text read is pinned by digest)."""
    found = UN_TEXT_VERSION.fullmatch(version or '')
    if found is None:
        raise Unreadable(f'no record version of a resolution\'s English text: {version!r}')
    if not body.startswith(b'%PDF-') or len(body) != int(found[1]):
        raise Unreadable(f'not the {found[1]}-byte PDF its record names ({len(body)} bytes)')
    return version


# The Security Council Consolidated List (2026-11-01): `CONSOLIDATED_LIST` holds `INDIVIDUALS/INDIVIDUAL` and
# `ENTITIES/ENTITY`, each with its regime's list type (`UN_LIST_TYPE`). The list types are the Council's sanctions
# regimes with a list, each kept by the Committee the resolution named here establishes; a regime whose measures attach
# to a place without listing anyone (Lebanon's, resolution 1701 (2006)) is no list type and is read on its own.
UN_REGIMES = {'Al-Qaida': 'S/RES/1267 (1999)', 'CAR': 'S/RES/2127 (2013)', 'DPRK': 'S/RES/1718 (2006)',
              'DRC': 'S/RES/1533 (2004)', 'GB': 'S/RES/2048 (2012)', 'Haiti': 'S/RES/2653 (2022)',
              'Iran': 'S/RES/1737 (2006)', 'Iraq': 'S/RES/1518 (2003)', 'Libya': 'S/RES/1970 (2011)',
              'Somalia': 'S/RES/2713 (2023)', 'SouthSudan': 'S/RES/2206 (2015)', 'Sudan': 'S/RES/1591 (2005)',
              'Taliban': 'S/RES/1988 (2011)', 'Yemen': 'S/RES/2140 (2014)'}


def un_list_regimes(body):
    """The sorted list types of the Consolidated List's parties."""
    try:
        root = _registers._sanctions.parse_xml(body, 'the UN Consolidated List')
    except _registers._sanctions.SanctionsError as error:
        raise Unreadable(str(error)) from error
    parties = [*root.iterfind('INDIVIDUALS/INDIVIDUAL'), *root.iterfind('ENTITIES/ENTITY')]
    regimes = sorted({' '.join((party.findtext('UN_LIST_TYPE') or '').split()) for party in parties})
    if root.tag != 'CONSOLIDATED_LIST' or not parties or '' in regimes:
        raise Unreadable('not the UN Consolidated List, or a party without its list type')
    return regimes


def read_un_list(body, source, version):
    """`<n> regimes`: the list is one document, never consolidated, so a new or ended regime is a new digest of the
    regimes read."""
    return f'{len(un_list_regimes(body))} regimes'


def un_list_record(body, source):
    """The regimes: designations change daily and are the imported list's (`un.sc`), not the text read here."""
    return json.dumps(un_list_regimes(body), separators=(',', ':')).encode('utf-8')


# reader(body, source, version) -> the version of an instrument's document; an index reader(body, source, today)
# -> the version its document's URL holds, or (the part of the URL, the version) when the version says more.
READERS = {'mevzuat': read_mevzuat, 'legislation-gov-uk': read_legislation_gov_uk,
           'legislation-gov-uk-published': read_legislation_gov_uk_published, 'cellar': read_cellar,
           'ecfr': read_ecfr, 'pdf': read_pdf, 'gazette': read_gazette, 'federal-register': read_federal_register,
           'sanctions-map': read_sanctions_map, 'gov-uk-regimes': read_uk_regimes,
           'gov-uk-document': read_gov_uk_document, 'un-resolution': read_un_resolution, 'un-sc-list': read_un_list}
# record(body, source) -> the bytes of the instrument within its document, whose SHA-256 is the text read; the whole
# document for a reader without one.
RECORDS = {'ecfr': ecfr_record, 'sanctions-map': sanctions_map_record, 'gov-uk-regimes': uk_regimes_record,
           'un-sc-list': un_list_record}
# A document whose reader is here (a revised UK text, which names the changes it has not applied; the UK's regime
# collection, which names its regimes' documents) is its own index.
INDEX_READERS = {'cellar-versions': read_cellar_versions, 'ecfr-versions': read_ecfr_versions,
                 'wassenaar-lists': read_wassenaar_lists, 'mevzuat-gazette': read_mevzuat_gazette,
                 'gazette-day': read_gazette_day, 'federal-register-record': read_federal_register_record,
                 'legislation-gov-uk': read_legislation_gov_uk_effects, 'gov-uk-regimes': read_uk_regimes_index,
                 'un-library-record': read_un_library_record}
# The readers whose document indexes a class of instruments the register holds one by one, with the Review method
# judging the class; the documents such an index names are its own, never acts a consolidated text lacks.
CLASS_JUDGES = {'sanctions-map': 'judge_regimes', 'gov-uk-regimes': 'judge_uk_regimes',
                'un-sc-list': 'judge_un_regimes'}


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
    `description` is its name (`kem`, `kdf` and `aead` in the HPKE file), or the value an identification sequence
    spells; `documents` are the RFCs it cites
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
                elif tag in {'description', 'name', 'kem', 'kdf', 'aead'} and text:
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
        form: an amendment shows as a new version, a change under the same version as a new digest. False when the
        document is unreadable."""
        try:
            version, digest = self.reading(source)
        except Unreadable as error:
            self.unread.append(f'{source["id"]}: {error}')
            return False
        instrument = entry['instrument']
        what = f'{entry["id"]} ({", ".join(entry["rows"])}): {instrument["name"]}, {instrument["article"]}'
        found = f'version {version}, ' if version else ''
        lacked = [] if source.get('reader') in CLASS_JUDGES else self.items[source['id']].get('documents', [])
        lacks = f'; read the acts its consolidated text lacks as published ({", ".join(lacked)})' if lacked else ''
        consolidated = instrument['consolidated']
        if consolidated is None:
            self.readings.append(f'{what}: never read from its primary source; read {self.items[source["id"]]["url"]} '
                                 f'({found}SHA-256 {digest}) in its consolidated text and record it with `read`{lacks}')
        elif version is not None and consolidated['version'] != version:
            self.readings.append(f'{what}: amended to {version} since it was read on {consolidated["read"]} '
                                 f'({consolidated["version"]}); read the amendment, record it with `read` (SHA-256 '
                                 f'{digest}) and record a trigger if it changes a pinned obligation{lacks}')
        elif consolidated['sha256'] != digest:
            self.readings.append(f'{what}: the primary text changed since it was read on {consolidated["read"]} '
                                 f'({consolidated["version"]}); read the change, record the new version and '
                                 f'SHA-256 {digest}, and record a trigger if it changes a pinned obligation')
        return True

    def judge_regimes(self, entry, source):
        """The EU Sanctions Map's regimes against the register's Council regulations: every basic regulation of a
        regime attached to a place is an entry, read and judged like any other instrument (one read without a
        destination when it restricts no supply to the place), and a Council regulation entry no regime lists any more
        has ended."""
        what = f'{entry["id"]} ({", ".join(entry["rows"])})'
        entries, council = {}, {}
        for other in self.register['entries']:
            instrument = other.get('instrument', {})
            for act in [found['act'] for found in resolve_primary(instrument.get('primary', ''))
                        if found.get('reader') == 'cellar' and found['act'][5] == 'R']:
                entries[act] = other['id']
                if instrument['name'].startswith('Council Regulation ') and other['sunset'] is None:
                    council[act] = other['id']
        regimes = sanctions_map_regimes(self.bodies[source['id']])
        for regime in filter(place_regime, regimes):
            for act in regime['regulations']:
                if act not in entries:
                    self.readings.append(f'{what}: regime {regime["id"]} ({regime["country"]}) restricts a supply to a '
                                         f'place, and its regulation {act} is no entry of the register; add it, read '
                                         'it with `read` and author what it restricts in the embargo generation')
        listed = {act for regime in regimes for act in regime['regulations']}
        for act, entry_id in sorted(council.items()):
            if act not in listed:
                self.readings.append(f'{what}: no regime lists {entry_id}\'s regulation {act} any more; read its '
                                     'repeal and record the entry\'s sunset')

    def judge_uk_regimes(self, entry, source):
        """The FCDO's regimes against the register's UK regulations: the regulation each regime listed by country
        rests on is an entry, read and judged like any other instrument (one read without a destination when it
        restricts no supply to a place); a regime listed by theme reaches the persons it lists, whom the UK Sanctions
        List names. A GB entry of COMPLY.1 on legislation.gov.uk that no regime names any more has ended; none is
        judged ended while a regime document is unread."""
        what = f'{entry["id"]} ({", ".join(entry["rows"])})'
        _, regimes, _ = uk_regime_documents(self.bodies[source['id']], source)
        named, unread = {}, 0
        for group, paths in regimes.items():
            for path in paths:
                body = self.bodies.get(gov_uk_id(path))
                try:
                    act = None if body is None else uk_regime_regulation(body, {'act': path})
                except Unreadable as error:
                    self.unread.append(f'{gov_uk_id(path)}: {error}')
                    body = None
                unread += body is None
                if body is not None and act:
                    named.setdefault(act, set()).add((group, path))
        entries = {}
        for other in self.register['entries']:
            instrument = other.get('instrument', {})
            for found in resolve_primary(instrument.get('primary', '')):
                if found.get('reader') == 'legislation-gov-uk':
                    entries[found['act']] = other
        for act, documents in sorted(named.items()):
            country = sorted(path for group, path in documents if group == 'country')
            if country and act not in entries:
                self.readings.append(f'{what}: the regime of {country[0]} rests on {act}, which is no entry of the '
                                     'register; add it, read it with `read` and author what it restricts in the '
                                     'embargo generation')
        if unread:
            self.unread.append(f'{what}: {unread} regime documents were not read, so no regulation is judged ended')
            return
        for act, other in sorted(entries.items()):
            if other['instrument']['jurisdiction'] == 'GB' and 'COMPLY.1' in other['rows'] and \
                    other['sunset'] is None and act not in named:
                self.readings.append(f'{what}: no regime names {other["id"]}\'s regulation {act} any more; read its '
                                     'revocation and record the entry\'s sunset')

    def judge_un_regimes(self, entry, source):
        """The Consolidated List's regimes against the register's Security Council resolutions: the resolution
        establishing each regime is an entry (`Security Council resolution 1718 (2006)`), read and judged like any
        other instrument (one read without a destination when its measures attach only to the persons listed); a list
        type this reader does not know is a new regime, and a known regime the list no longer names may have ended."""
        what = f'{entry["id"]} ({", ".join(entry["rows"])})'
        regimes = un_list_regimes(self.bodies[source['id']])
        resolutions = {}
        for other in self.register['entries']:
            instrument = other.get('instrument', {})
            named = _registers.UN_RESOLUTION_NAME.match(instrument.get('name', ''))
            if named and any(found.get('reader') == 'un-resolution'
                             for found in resolve_primary(instrument['primary'])):
                resolutions[f'S/RES/{named[1]} ({named[2]})'] = other
        for regime in regimes:
            symbol = UN_REGIMES.get(regime)
            if symbol is None:
                self.readings.append(f'{what}: the list names the regime {regime!r}, which UN_REGIMES does not know; '
                                     'read the resolution establishing it, name it there and in the register, and '
                                     'author what it restricts in the embargo generation')
            elif symbol not in resolutions:
                self.readings.append(f'{what}: the {regime} regime rests on {symbol}, which is no entry of the '
                                     'register; add it, read it with `read` and author what it restricts in the '
                                     'embargo generation')
        for regime, symbol in sorted(UN_REGIMES.items()):
            other = resolutions.get(symbol)
            if regime not in regimes and other is not None and other['sunset'] is None:
                self.readings.append(f'{what}: the list names no party of the {regime} regime any more; read whether '
                                     f'a resolution ended {other["id"]}\'s {symbol} and record the entry\'s sunset')

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
                if source is not None and source['id'] in self.bodies and self.judge_primary(entry, source) and \
                        source.get('reader') in CLASS_JUDGES:
                    getattr(self, CLASS_JUDGES[source['reader']])(entry, source)
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
        planned = len(self.manifest['plan']['sources'])
        lacked = len(self.manifest['items']) - planned
        lines = [f'# Register review of {day} ({scan} scan, {self.register["domain"]} register)', '',
                 '<!-- doc-role: record -->', '',
                 f'Sources: {planned} planned' + (f' and {lacked} documents their indexes name' if lacked else '')
                 + f', {len(self.bodies)} read on {self.manifest["day"]}; the register changes below were '
                 f'{"applied" if applied else "proposed, not applied"}. '
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
    latest = max(_registers.parse_day(run['day']) for run in review['runs'])
    for event in review['calendar']:
        if event.get('reads') and _registers.parse_day(event['day']) > latest:
            print(f'  calendar: the scan after {event["event"]} ({event["day"]}) reads {len(event["reads"])} '
                  'instruments not yet read')
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
    if any(source.get('reader') == 'cellar-versions' for source in sources):
        print(f'  and, as published, each act a consolidated text lacks ({CELLAR.format(version="<CELEX>")})')
    if any(source.get('reader') == 'legislation-gov-uk' for source in sources):
        print('  and, as published, each act in force a revised UK text has not applied '
              f'({LEGISLATION_PUBLISHED.format(act="<act>", form="<enacted|made>")})')
    if any(source.get('reader') == 'gov-uk-regimes' for source in sources):
        print(f'  and each document of the regimes the UK\'s collection lists ({GOV_UK_CONTENT.format(path="<page>")})')
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
    (its host's reader, naming the acts the consolidated text lacks, each of which must have been read as
    published), the fetch day and the SHA-256 of the text read, its source verified and cited by --ref. Nothing is
    written unless every entry can be recorded."""
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
                for document in review.items[source['id']].get('documents', []):
                    if document not in review.bodies:
                        raise Unreadable(f'the document it names, {document}, was not read '
                                         f'({review.items[document].get("error")})')
                    item = review.items[document]
                    READERS[item['reader']](review.bodies[document], item, None)
            except Unreadable as error:
                errors.append(f'{entry_id}: {source["id"]}: {error}')
    if errors:
        raise ReviewError('nothing recorded: ' + '; '.join(errors))
    for entry, version, digest in readings:
        entry['instrument']['consolidated'] = {'version': version, 'read': manifest['day'], 'sha256': digest}
        entry['source'] = {'ref': ref, 'read': manifest['day'], 'verified': True}
        print(f'{entry["id"]}: read {entry["instrument"]["name"]} at {version} on {manifest["day"]}')
    recorded = {entry['id'] for entry, _, _ in readings}
    for event in register['review']['calendar']:
        if 'reads' in event:
            event['reads'] = [entry_id for entry_id in event['reads'] if entry_id not in recorded]
            if not event['reads']:
                del event['reads']
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
