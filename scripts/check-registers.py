#!/usr/bin/env python3
"""The standards and obligations registers: one format, one guard, two domains (NET-STD, COMPLY.0).

Each register is a JSON file beside the module registry (cmake/<domain>-register.json, schema `cerid-register/1`).
An entry names what Cerid implements, plans or watches, its owning module, the ROADMAP rows that implement it, its
primary source with the date it was read, the date of its next review and the evidence that proves it. The
standards domain (cmake/standards-register.json, NET-STD) adds the specification with its revision and status in the
committed snapshot, the IANA or Cerid codepoints and the RFC 7696 agility checklist. docs/design/registers.md is the
contract; COMPLY.0 adds the legal domain to this guard, never a second one.

The guard fails, among the format errors, on:
  - a pinned draft (an implemented or planned entry) that has expired in the snapshot: an Internet-Draft expires 185
    days after it was posted unless the IESG or the RFC Editor holds it, and a posted date known only to the month or
    year counts from the first day of that period;
  - a pinned RFC that the snapshot records as obsoleted;
  - an entry or the register past its review date, or a source read after the snapshot or after today;
  - a review schedule whose `next` is not the day review_due computes from its calendar and recorded runs (the
    scheduled re-research of scripts/register-review.py), or whose calendar names no event after the latest run;
  - an owning module neither registered nor in the capability registry, an unknown ROADMAP row, a governed row no
    entry names, an entry still planned when every one of its rows is Done, and an implemented entry without existing
    evidence or with an unverified codepoint.

  python scripts/check-registers.py [--root <repository root>] [--today YYYY-MM-DD]
"""
import argparse
import datetime
import importlib.util
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 'cerid-register/1'
REGISTERS = {'standards': 'cmake/standards-register.json'}
DRAFT_LIFETIME = datetime.timedelta(days=185)
ID = re.compile(r'^[a-z0-9]+(?:[.-][a-z0-9]+)*$')
DATE = re.compile(r'^\d{4}(?:-\d{2}(?:-\d{2})?)?$')
REGISTER_KEYS = {'schema', 'domain', 'about', 'snapshot', 'review', 'governed_rows', 'entries'}
REVIEW_KEYS = {'cadence', 'next', 'lag_days', 'full_every_days', 'calendar', 'runs'}
SCANS = {'light', 'full'}
CAPABILITY_OWNERS = 'cmake/capability-owners.json'
ENTRY_KEYS = {'id', 'title', 'kind', 'state', 'owner', 'rows', 'source', 'review', 'evidence', 'deprecation', 'note'}
STATES = {'implemented', 'planned', 'watch', 'deprecated'}
PINNED = {'implemented', 'planned'}
DOMAINS = {
    'standards': {
        'register_keys': {'checklist'},
        'entry_keys': {'spec', 'codepoints'},
        'kinds': {'protocol', 'extension', 'algorithm', 'format', 'guidance'},
    },
}
SPEC_STATUS = {
    'rfc': {'current', 'obsoleted'},
    'draft': {'active', 'iesg', 'rfc-editor', 'expired', 'replaced'},
    'w3c': {'recommendation', 'candidate', 'working-draft'},
    'nist': {'final', 'draft', 'withdrawn'},
    'cerid': {'design', 'stable'},
}
HELD_DRAFT = {'iesg', 'rfc-editor'}

_spec = importlib.util.spec_from_file_location('check_master_plan', Path(__file__).resolve().parent /
                                               'check-master-plan.py')
_plan = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_plan)


def parse_day(text):
    """The first day of a YYYY, YYYY-MM or YYYY-MM-DD period, or None when the text is not one."""
    if not isinstance(text, str) or not DATE.match(text):
        return None
    parts = [int(part) for part in text.split('-')] + [1, 1]
    try:
        return datetime.date(parts[0], parts[1], parts[2])
    except ValueError:
        return None


def draft_expiry(posted):
    """The earliest day a draft posted in this period expires (185 days after posting), or None."""
    day = parse_day(posted)
    return day + DRAFT_LIFETIME if day else None


def review_due(review):
    """(day, scan, reason) of the next review a well-formed schedule owes, whichever comes first: a scan `lag_days`
    after each calendar event no recorded run has covered yet (a run covers the events up to its day), a full
    re-research `full_every_days` after the latest full run, and a full re-research `lag_days` after a light run
    that found a pinned entry's status changed (its `triggers`) since then."""
    lag = datetime.timedelta(days=review['lag_days'])
    runs = [(parse_day(run['day']), run) for run in review['runs']]
    latest = max(day for day, _ in runs)
    latest_full = max(day for day, run in runs if run['scan'] == 'full')
    candidates = [(latest_full + datetime.timedelta(days=review['full_every_days']), 'full',
                   f'the yearly full re-research after {latest_full}')]
    for event in review['calendar']:
        if parse_day(event['day']) > latest:
            candidates.append((parse_day(event['day']) + lag, event['scan'], f'{event["event"]} ({event["day"]})'))
    for day, run in runs:
        if run['scan'] == 'light' and run['triggers'] and day > latest_full:
            candidates.append((day + lag, 'full', f'the status changes the {day} scan found '
                               f'({", ".join(run["triggers"])})'))
    return min(candidates, key=lambda candidate: candidate[0])


class Context:
    """What an entry is judged against: the ROADMAP rows with their states, the owning modules (registered, or
    named by the capability registry, cmake/capability-owners.json, when a row will create them) and today."""

    def __init__(self, root, today):
        self.root = root
        self.today = today
        rows = _plan.parse_rows((root / 'docs/ROADMAP.md').read_text(encoding='utf-8'))
        self.rows = {name.lower(): cells[2] for name, cells in rows}
        self.owners = _plan.registered_modules((root / 'CMakeLists.txt').read_text(encoding='utf-8'))
        capabilities = json.loads((root / CAPABILITY_OWNERS).read_text(encoding='utf-8'))['capabilities']
        self.owners |= {capability['owner'] for capability in capabilities}

    def row_state(self, row):
        return self.rows.get(row.lower()) if isinstance(row, str) else None


def date_errors(where, key, value, required=True):
    if value is None and not required:
        return []
    day = parse_day(value)
    if day is None or len(value) != 10:
        return [f'{where}: {key} must be a YYYY-MM-DD date, not {value!r}']
    return []


def reference_errors(where, key, ref, root):
    """A repository path, optionally with an #anchor that its Markdown defines as an id."""
    path, _, anchor = ref.partition('#')
    target = root / path
    if not path or not target.exists():
        return [f'{where}: {key} {path!r} does not exist']
    if anchor and target.suffix == '.md' and f'id="{anchor}"' not in target.read_text(encoding='utf-8'):
        return [f'{where}: {key} {path} has no anchor {anchor!r}']
    return []


def spec_errors(where, entry, context):
    spec = entry.get('spec')
    if not isinstance(spec, dict):
        return [f'{where}: spec must be an object']
    errors = []
    kind = spec.get('type')
    if kind not in SPEC_STATUS:
        return [f'{where}: spec.type must be one of {sorted(SPEC_STATUS)}, not {kind!r}']
    unknown = set(spec) - {'type', 'ref', 'revision', 'posted', 'status', 'obsoleted_by'}
    if unknown:
        errors.append(f'{where}: spec has unknown keys {sorted(unknown)}')
    if not isinstance(spec.get('ref'), str) or not spec['ref']:
        errors.append(f'{where}: spec.ref names the document')
    status = spec.get('status')
    if status not in SPEC_STATUS[kind]:
        errors.append(f'{where}: spec.status of a {kind} must be one of {sorted(SPEC_STATUS[kind])}, not {status!r}')
    if spec.get('posted') is not None and parse_day(spec['posted']) is None:
        errors.append(f'{where}: spec.posted must be YYYY, YYYY-MM or YYYY-MM-DD, not {spec["posted"]!r}')
    elif parse_day(spec.get('posted')) and parse_day(spec['posted']) > context.today:
        errors.append(f'{where}: spec.posted {spec["posted"]} is after today')
    obsoleted_by = spec.get('obsoleted_by', [])
    if not isinstance(obsoleted_by, list) or (status == 'obsoleted') != bool(obsoleted_by):
        errors.append(f'{where}: spec.obsoleted_by lists the replacing documents exactly when the status is obsoleted')
    pinned = entry.get('state') in PINNED
    if kind == 'draft' and pinned:
        if not spec.get('revision') or not spec.get('posted'):
            errors.append(f'{where}: a pinned draft names its revision and posted date')
        elif status in {'expired', 'replaced'}:
            errors.append(f'{where}: pinned draft {spec.get("ref")}-{spec["revision"]} is {status} in the snapshot; '
                          'pin its successor or refresh the snapshot')
        elif status not in HELD_DRAFT and draft_expiry(spec['posted']) <= context.today:
            errors.append(f'{where}: pinned draft {spec.get("ref")}-{spec["revision"]} (posted {spec["posted"]}) '
                          f'expired on {draft_expiry(spec["posted"])}; refresh the snapshot or pin its successor')
    if kind == 'rfc' and pinned and status == 'obsoleted':
        errors.append(f'{where}: pinned {spec.get("ref")} is obsoleted by {", ".join(obsoleted_by)} in the snapshot; '
                      'pin the replacement')
    return errors


def codepoint_errors(where, entry):
    codepoints = entry.get('codepoints', [])
    if not isinstance(codepoints, list):
        return [f'{where}: codepoints must be a list']
    errors = []
    for index, point in enumerate(codepoints):
        if not isinstance(point, dict) or set(point) - {'registry', 'name', 'value', 'verified'} or \
                not all(isinstance(point.get(key), str) and point[key] for key in ('registry', 'name', 'value')):
            errors.append(f'{where}: codepoint {index} has a registry, a name, a value and an optional verified flag')
            continue
        if entry.get('state') == 'implemented' and point.get('verified') is False:
            errors.append(f'{where}: implemented with the unverified codepoint {point["name"]} = {point["value"]}; '
                          'verify it against the IANA registry first')
    return errors


def entry_errors(entry, domain, register, context):
    where = f'{domain} entry {entry.get("id", "?")}'
    rules = DOMAINS[domain]
    errors = []
    unknown = set(entry) - ENTRY_KEYS - rules['entry_keys']
    if unknown:
        errors.append(f'{where}: unknown keys {sorted(unknown)}')
    if not isinstance(entry.get('id'), str) or not ID.match(entry['id']):
        errors.append(f'{where}: id must be lower-case dotted words')
    if not isinstance(entry.get('title'), str) or not entry['title']:
        errors.append(f'{where}: title is required')
    if entry.get('kind') not in rules['kinds']:
        errors.append(f'{where}: kind must be one of {sorted(rules["kinds"])}')
    state = entry.get('state')
    if state not in STATES:
        errors.append(f'{where}: state must be one of {sorted(STATES)}')
    owner = entry.get('owner')
    if owner not in context.owners:
        errors.append(f'{where}: owner {owner!r} is neither a registered module nor an owner of {CAPABILITY_OWNERS}')
    rows = entry.get('rows')
    if not isinstance(rows, list) or not rows:
        errors.append(f'{where}: rows names the ROADMAP rows that implement it')
        rows = []
    for row in rows:
        if context.row_state(row) is None:
            errors.append(f'{where}: unknown ROADMAP row {row!r}')
    if state == 'planned' and rows and all(context.row_state(row) == 'Done' for row in rows):
        errors.append(f'{where}: every row ({", ".join(rows)}) is Done; mark it implemented with its evidence')
    source = entry.get('source')
    if not isinstance(source, dict) or set(source) != {'ref', 'read', 'verified'} or \
            not isinstance(source.get('ref'), str) or not isinstance(source.get('verified'), bool):
        errors.append(f'{where}: source has ref, read (YYYY-MM-DD) and verified')
    else:
        errors += date_errors(where, 'source.read', source['read'])
        read = parse_day(source['read'])
        snapshot = parse_day(register.get('snapshot', {}).get('date'))
        if read and (read > context.today or (snapshot and read > snapshot)):
            errors.append(f'{where}: source.read {source["read"]} is after the snapshot or today')
        errors += reference_errors(where, 'source', source['ref'], context.root)
    errors += date_errors(where, 'review', entry.get('review'))
    if parse_day(entry.get('review')) and parse_day(entry['review']) < context.today:
        errors.append(f'{where}: review was due on {entry["review"]}')
    errors += date_errors(where, 'deprecation', entry.get('deprecation'), required=False)
    if state == 'deprecated' and not entry.get('deprecation'):
        errors.append(f'{where}: a deprecated entry names its deprecation date')
    evidence = entry.get('evidence', [])
    if not isinstance(evidence, list):
        errors.append(f'{where}: evidence must be a list of repository paths')
        evidence = []
    if state == 'implemented' and not evidence:
        errors.append(f'{where}: an implemented entry names its evidence (tests, interop records)')
    for path in evidence:
        if not isinstance(path, str):
            errors.append(f'{where}: evidence {path!r} is not a repository path')
        else:
            errors += reference_errors(where, 'evidence', path, context.root)
    if domain == 'standards':
        errors += spec_errors(where, entry, context)
        errors += codepoint_errors(where, entry)
    return errors


def checklist_errors(register, context):
    """RFC 7696 items: each names its mechanism and rows; once every row is Done it names its evidence."""
    errors = []
    items = register.get('checklist')
    if not isinstance(items, list) or not items:
        return ['standards register: checklist holds the RFC 7696 agility items']
    for item in items:
        where = f'checklist {item.get("section", "?")}'
        if set(item) != {'section', 'item', 'mechanism', 'rows', 'evidence'}:
            errors.append(f'{where}: has section, item, mechanism, rows and evidence')
            continue
        unknown = [row for row in item['rows'] if context.row_state(row) is None]
        if not item['rows'] or unknown:
            errors.append(f'{where}: names known ROADMAP rows (unknown: {unknown})')
        elif all(context.row_state(row) == 'Done' for row in item['rows']) and not item['evidence']:
            errors.append(f'{where}: every row is Done; name the evidence')
        for path in item['evidence']:
            errors += reference_errors(where, 'evidence', path, context.root)
    return errors


def review_errors(where, review, entries, context):
    """The schedule: a calendar of the events after which a scan is owed, the recorded runs (the first full
    research included) and `next`, which must be the day review_due computes and not be past."""
    if not isinstance(review, dict) or set(review) != REVIEW_KEYS:
        return [f'{where}: review has {", ".join(sorted(REVIEW_KEYS))}']
    errors = date_errors(where, 'review.next', review['next'])
    for key in ('lag_days', 'full_every_days'):
        if not isinstance(review[key], int) or isinstance(review[key], bool) or review[key] <= 0:
            errors.append(f'{where}: review.{key} is a positive number of days')
    calendar, runs = review['calendar'], review['runs']
    if not isinstance(calendar, list) or not calendar or not isinstance(runs, list) or not runs:
        return errors + [f'{where}: review.calendar and review.runs are non-empty lists']
    previous = None
    for event in calendar:
        if not isinstance(event, dict) or set(event) != {'event', 'day', 'scan', 'verified'} or \
                not isinstance(event.get('event'), str) or event.get('scan') not in SCANS or \
                not isinstance(event.get('verified'), bool):
            errors.append(f'{where}: a calendar event has event, day, scan (light or full) and verified')
            continue
        errors += date_errors(where, f'calendar {event["event"]} day', event['day'])
        if previous and parse_day(event['day']) and parse_day(event['day']) <= previous:
            errors.append(f'{where}: calendar events are in strictly increasing day order ({event["event"]})')
        previous = parse_day(event['day']) or previous
    ids = {entry.get('id') for entry in entries if isinstance(entry, dict)}
    previous = None
    for run in runs:
        if not isinstance(run, dict) or set(run) != {'day', 'scan', 'ref', 'triggers'} or \
                run.get('scan') not in SCANS or not isinstance(run.get('ref'), str) or \
                not isinstance(run.get('triggers'), list):
            errors.append(f'{where}: a run has day, scan (light or full), ref (its report) and triggers')
            continue
        errors += date_errors(where, 'run day', run['day'])
        day = parse_day(run['day'])
        if day and day > context.today:
            errors.append(f'{where}: the run of {run["day"]} is after today')
        if previous and day and day < previous:
            errors.append(f'{where}: runs are recorded in day order ({run["day"]})')
        previous = day or previous
        errors += reference_errors(where, f'run {run["day"]} ref', run['ref'], context.root)
        unknown = [trigger for trigger in run['triggers'] if trigger not in ids]
        if unknown:
            errors.append(f'{where}: the run of {run["day"]} names unknown triggering entries {unknown}')
    if errors:
        return errors
    if not any(run['scan'] == 'full' for run in runs):
        return [f'{where}: review.runs records the full research the register was assembled from']
    latest = max(parse_day(run['day']) for run in runs)
    if not any(parse_day(event['day']) > latest for event in calendar):
        errors.append(f'{where}: the review calendar names no event after the latest run ({latest}); add the next '
                      'meetings and events from their published schedules')
    due, scan, reason = review_due(review)
    if parse_day(review['next']) != due:
        errors.append(f'{where}: review.next {review["next"]} is not the due day {due} ({scan} scan after {reason}); '
                      'scripts/register-review.py due prints it')
    elif due < context.today:
        errors.append(f'{where}: the scheduled {scan} review was due on {review["next"]} ({reason}); '
                      'run scripts/register-review.py')
    return errors


def register_errors(domain, register, context):
    where = f'{domain} register'
    if not isinstance(register, dict) or register.get('schema') != SCHEMA or register.get('domain') != domain:
        return [f'{where}: schema must be {SCHEMA!r} and domain {domain!r}']
    errors = []
    unknown = set(register) - REGISTER_KEYS - DOMAINS[domain]['register_keys']
    if unknown:
        errors.append(f'{where}: unknown keys {sorted(unknown)}')
    snapshot = register.get('snapshot')
    if not isinstance(snapshot, dict) or set(snapshot) != {'date', 'method'}:
        errors.append(f'{where}: snapshot has date and method')
    else:
        errors += date_errors(where, 'snapshot.date', snapshot['date'])
        if parse_day(snapshot['date']) and parse_day(snapshot['date']) > context.today:
            errors.append(f'{where}: snapshot.date {snapshot["date"]} is after today')
    entries = register.get('entries')
    errors += review_errors(where, register.get('review'), entries if isinstance(entries, list) else [], context)
    if not isinstance(entries, list) or not entries:
        return errors + [f'{where}: entries is a non-empty list']
    seen = set()
    for entry in entries:
        if not isinstance(entry, dict):
            errors.append(f'{where}: every entry is an object')
            continue
        if entry.get('id') in seen:
            errors.append(f'{where}: duplicate entry {entry.get("id")}')
        seen.add(entry.get('id'))
        errors += entry_errors(entry, domain, register, context)
    named = {row.lower() for entry in entries if isinstance(entry, dict) for row in entry.get('rows') or []
             if isinstance(row, str)}
    for row in register.get('governed_rows', []):
        if context.row_state(row) is None:
            errors.append(f'{where}: governed row {row!r} is not a ROADMAP row')
        elif row.lower() not in named:
            errors.append(f'{where}: governed row {row} names no register entry')
    if domain == 'standards':
        errors += checklist_errors(register, context)
    return errors


def next_expiry(register, today):
    """(day, entry id) of the pinned draft that expires first, for the summary line."""
    upcoming = []
    for entry in register.get('entries', []):
        spec = entry.get('spec', {})
        if entry.get('state') in PINNED and spec.get('type') == 'draft' and spec.get('status') not in HELD_DRAFT:
            day = draft_expiry(spec.get('posted'))
            if day and day > today:
                upcoming.append((day, entry['id']))
    return min(upcoming, default=None)


def check(root, today):
    """[(domain, register or None, errors)] for every register of the tree."""
    context = Context(root, today)
    results = []
    for domain, relative in REGISTERS.items():
        path = root / relative
        try:
            register = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError) as error:
            results.append((domain, None, [f'{relative}: {error}']))
            continue
        results.append((domain, register, register_errors(domain, register, context)))
    return results


def main(argv=None):
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description='Standards and obligations register guard')
    parser.add_argument('--root', default=str(ROOT), help='repository root (tests use a fixture)')
    parser.add_argument('--today', help='judge as of this YYYY-MM-DD day instead of the UTC date')
    args = parser.parse_args(argv)
    today = parse_day(args.today) if args.today else datetime.datetime.now(datetime.timezone.utc).date()
    if today is None:
        parser.error(f'--today must be YYYY-MM-DD, not {args.today!r}')
    failed = False
    for domain, register, errors in check(Path(args.root).resolve(), today):
        for error in errors:
            print('FAIL: ' + error)
        failed = failed or bool(errors)
        if register is not None and not errors:
            entries = register['entries']
            pinned = sum(entry['state'] in PINNED for entry in entries)
            soonest = next_expiry(register, today) if domain == 'standards' else None
            tail = f'; the first pinned draft to expire is {soonest[1]} on {soonest[0]}' if soonest else ''
            print(f'PASS: {domain} register: {len(entries)} entries, {pinned} pinned, next review '
                  f'{register["review"]["next"]}{tail}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
