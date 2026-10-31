# Standards and obligations registers

<!-- doc-role: contract -->
> Design contract of [NET-STD](../ROADMAP.md#slice-net-std) and, for its legal domain,
> [COMPLY.0](../ROADMAP.md#slice-comply.0); status lives only in the ROADMAP. Read the
> [network foundation](network-foundation.md#negotiation) and the
> [networking research](../research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md#staying-current)
> first.

Purpose: staleness becomes a failing check. Every protocol, extension, algorithm and format Cerid implements, plans
or watches, and (from COMPLY.0) every legal and platform obligation, is one machine-readable entry with its primary
source, the date it was read, its owner, the rows that implement it and the date of its next review. One format, one
guard and one review machinery serve both domains; COMPLY.0 adds its domain to them and writes no second guard.

## Files and guard

| Domain | Register | Row |
|---|---|---|
| `standards` | [cmake/standards-register.json](../../cmake/standards-register.json) | NET-STD |
| `legal` | Lands with COMPLY.0 beside it, named for its domain | COMPLY.0 |

The registers sit beside the module registry and the capability owners in `cmake/`. The guard is
[scripts/check-registers.py](../../scripts/check-registers.py): the `crd-check-registers` CTest of every configured
tree and the "Standards register is current" repository step of both CI definitions. It reads the ROADMAP rows and
their states through `check-master-plan.py`, the registered modules from the root `CMakeLists.txt` and the planned
owners from the capability registry, and judges
dates against the UTC day (`--today YYYY-MM-DD` replays another day; the tests use it).

## Format (`cerid-register/1`)

A register holds `schema`, `domain`, `about`, a `snapshot` (the day the register was assembled and the method), a
`review` (the cadence and the next scheduled review), `governed_rows` (rows that must name at least one entry) and
`entries`.

Every entry holds:

| Field | Meaning |
|---|---|
| `id` | Lower-case dotted identifier, unique; code and other documents cite it |
| `title`, `kind` | What it is; the domain fixes the kinds |
| `state` | `implemented`, `planned` (both are **pinned**), `watch` or `deprecated` |
| `owner` | A registered module, or an owner the [capability registry](capability-ownership.md) names (a row creates it) |
| `rows` | The ROADMAP rows that implement or qualify it |
| `source` | `ref` (a repository document with its anchor), `read` (the day) and `verified` (false for knowledge or secondary coverage) |
| `review` | The day the entry is next reviewed |
| `evidence` | Repository paths (tests, interop records) proving an implemented entry |
| `deprecation` | The day Cerid stops accepting it; required when deprecated |
| `note` | Optional |

The standards domain adds `spec` (the document: `type` `rfc`, `draft`, `w3c`, `nist` or `cerid`; `ref`; for a draft
its `revision` and `posted` day, month or year; `status` in the snapshot; `obsoleted_by` for an obsoleted RFC),
`codepoints` (IANA or Cerid registry, name, value; `verified: false` marks a value taken from knowledge) and, at the
register level, the RFC 7696 `checklist` (section, item, the Cerid mechanism, its rows and evidence). The legal domain
adds, with COMPLY.0: instrument and article, jurisdiction, consolidated version, the products and data flows, the
check that proves the obligation, effective and sunset dates and a counsel-review flag.

## Rules

- **Expired pinned drafts fail.** An Internet-Draft expires 185 days after it was posted unless the IESG or the RFC
  Editor holds it (`iesg`, `rfc-editor`). A posted date known only to its month or year counts from the first day of
  that period, so the guard is never later than the truth. A draft the snapshot records as `expired` or `replaced`
  fails while pinned. Watched drafts never fail on expiry.
- **Obsoleted pinned RFCs fail**; `obsoleted_by` names the replacement and is present exactly when the status is
  `obsoleted`.
- **Overdue reviews fail**, per entry and for the register, as does a source read after the snapshot or after today.
- **Owners and rows exist.** An owner is registered or named in `cmake/capability-owners.json`, which alone records
  planned owners and the rows that create them; a governed row that no entry names fails, so the entries each networked row implements are on record.
- **Finished work is proven.** An entry still `planned` when every one of its rows is Done fails; an `implemented`
  entry needs existing evidence and only verified codepoints; a checklist item whose rows are all Done needs evidence.
- Unknown keys, malformed dates and references to missing files or anchors fail.

## Snapshot and review

The first snapshot (2026-10-31) was assembled from the networking research read on 2026-10-27 (sources N01 to N124)
and the network foundation design; entries the research marks as knowledge or secondary carry `verified: false`.
Codepoints from knowledge are marked the same way, and an entry cannot become `implemented` until they are checked
against the IANA registries.

Refreshing the snapshot from the RFC Editor, the IETF datatracker and the IANA registries is a download the user
approves first. The review cadence is a light scan after each IETF meeting (March, July and November) and a full
re-research every year or when a pinned item changes status; each run proposes ROADMAP amendments for the user and
never edits rows itself. NET-STD owns the scheduled re-research tooling; until it lands, the track working the
NETCRASH window runs the review. The calendar points at which the guard fails without a refresh are the register's
`review.next` (2026-12-15, after IETF 127) and the first pinned draft's expiry, which the guard prints
(`webtransport.h3`, 2027-01-02, counted conservatively from July 2026).

## Cerid identifiers

The Cerid wire protocols of the [network foundation](network-foundation.md#negotiation) are entries of type `cerid`
with their protocol identifiers (session control `0x0001` to relay allocation `0x0008`) in the "Cerid protocol
identifiers" registry. The "Cerid GREASE values" registry holds the pattern `0x?A?A` (the same digit in each `?`:
`0x0A0A`, `0x1A1A` ... `0xFAFA`, the RFC 8701 pattern): reserved as extension identifiers and as version majors,
never assigned, and sent by every build. Identifiers are never reused; a retired one stays in the register as
`deprecated`.

Code never spells a codepoint by hand. [scripts/gen-register-ids.py](../../scripts/gen-register-ids.py) writes, for
each owning module, a committed header of the codepoints its entries hold (for `net`,
[register_ids.hpp](../../engine/foundation/net/include/crd/net/register_ids.hpp): `ProtocolId`, the GREASE table and
the QUIC and ALPN values). A codepoint marked `verified: false` is written into a nested `unverified` namespace, so a
use of it is visible where it is made and verifying it moves it out. A registry without a C++ type, a malformed value,
a pattern that is not 16 bits and a registered owner with codepoints but no output each fail the generator. The
`crd-net-register-ids-drift` CTest runs `--check`, and
[scripts/generated-sources.json](../../scripts/generated-sources.json) records the header's bytes.
