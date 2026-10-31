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
| `legal` | [cmake/legal-register.json](../../cmake/legal-register.json) | COMPLY.0 |

The registers sit beside the module registry and the capability owners in `cmake/`. The guard is
[scripts/check-registers.py](../../scripts/check-registers.py): the `crd-check-registers` CTest of every configured
tree and the "Standards and legal registers are current" repository step of both CI definitions. It reads the
ROADMAP rows and their states through `check-master-plan.py`, the registered modules from the root `CMakeLists.txt`
and the planned owners from the capability registry, and judges dates against the UTC day (`--today YYYY-MM-DD`
replays another day; the tests use it).

## Format (`cerid-register/1`)

A register holds `schema`, `domain`, `about`, a `snapshot` (the day the register was assembled or last refreshed and
the method), a `review` schedule ([below](#snapshot-and-review)), `governed_rows` (rows that must name at least one
entry) and `entries`.

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
adds the fields [below](#legal-domain).

## Rules

- **Expired pinned drafts fail.** An Internet-Draft expires 185 days after it was posted unless the IESG or the RFC
  Editor holds it (`iesg`, `rfc-editor`). A posted date known only to its month or year counts from the first day of
  that period, so the guard is never later than the truth. A draft the snapshot records as `expired` or `replaced`
  fails while pinned. Watched drafts never fail on expiry.
- **Obsoleted pinned RFCs fail**; `obsoleted_by` names the replacement and is present exactly when the status is
  `obsoleted`.
- **Overdue reviews fail**, per entry and for the register, as does a source read after the snapshot or after today.
- **The schedule is computed, never typed.** `review.next` must be the day the schedule's calendar and runs owe; a
  calendar with no event after the latest run fails, so the light scans cannot silently stop.
- **Owners and rows exist.** An owner is registered or named in `cmake/capability-owners.json`, which alone records
  planned owners and the rows that create them; a governed row that no entry names fails, so the entries each networked row implements are on record.
- **Finished work is proven.** An entry still `planned` when every one of its rows is Done fails; an `implemented`
  entry needs existing evidence and only verified codepoints; a checklist item whose rows are all Done needs evidence.
- Unknown keys, malformed dates and references to missing files or anchors fail.

<a id="legal-domain"></a>
## Legal domain

[cmake/legal-register.json](../../cmake/legal-register.json) holds one entry per obligation of an instrument and
article that applies to Cerid or to products built with it (COMPLY.0; the children COMPLY.1 to COMPLY.9 read and
implement them). Kinds are `obligation` and `user-action` (a registration, filing, account or counsel review the
publisher, the user's Turkish company, takes after implementation and qualification; it never blocks implementation).
`watch` holds an instrument that is pending or a market a product has not entered; `deprecated` one that ended.

| Field | Meaning |
|---|---|
| `topic` | `privacy`, `children`, `security`, `accessibility`, `export-control`, `ai`, `online-safety`, `consumer`, `age-rating`, `intellectual-property`, `communications`, `tax`, `payments`, `e-signature` or `platform` |
| `instrument` | `name`, `article`, `type` (law, regulation, directive, by-law, decision, code of practice, guidance, treaty, policy, licence), `jurisdiction`, `primary` (the https URL of the official text on one of the jurisdiction's official hosts) and `consolidated` (null until read; then the consolidated `version`, the day it was `read` and the `sha256` of the text read) |
| `obligation` | The duty in plain words |
| `applies_to` | The scopes (products, data flows, channels) it governs |
| `check` | `ref`, `name` and `state`: `active` cites the test, script or CI step that proves it (it runs in every tree, so CI fails when it fails); `planned` cites the ROADMAP row that adds it |
| `effective`, `sunset` | The day the instrument applies (a month or year counts from its first day, so the guard is never later than the truth) and the day it ends (null when open) |
| `counsel` | `needed` (interpretation needs counsel; never guessed) and `reviewed` (the day counsel answered) |

At the register level, `jurisdictions` maps each code (ISO 3166 with an optional subdivision such as `US-CA`, `INT`
for treaties, international bodies and licences, `PLATFORM` for platform and store policies) to its name and official
hosts (the Official Gazette and mevzuat.gov.tr and the Turkish regulators, EUR-Lex and the EU bodies,
legislation.gov.uk, the eCFR and so on), and `scopes` lists what obligations apply to: each scope has a kind
(`product`, `flow`, `channel`), a state (`planned` or `live`), the rows that put it in front of people outside the
publisher and the evidence that it shipped. `governed_rows` names every networked, data-handling, AI, user-content,
communication and store row; each must name an entry.

Rules of the legal domain, beside the common ones:

- **Binding obligations are proven.** An obligation is binding on a day when it is pinned, effective, not past its
  sunset and applies to a live scope; a binding obligation without an active check fails. A pinned entry past its
  sunset fails until it is deprecated.
- **Scopes cannot go stale.** A scope turns live when any of its rows is Done (each row puts it in front of people on
  its own), so a planned scope with a Done row fails, and a live scope names its evidence.
- **Primary sources are official.** `primary` is https on one of the jurisdiction's hosts (or a subdomain); a law
  firm's or a news summary is a research source (`source`), never the primary.
- **Reading precedes closing.** An entry whose rows are all Done, or which is implemented, has its consolidated text
  read (its version, day and digest); a read entry is verified and cites the instrument's own document, not the home
  page of its journal or regulator.
- **Counsel precedes implementation.** An implemented entry whose counsel review is needed carries its review day,
  and an agent never marks an obligation satisfied without an active check and evidence.
- **Turkish law is never omitted.** Every topic the register holds, the platform policies aside, has a Turkish entry.

The JSON register is the guarded source of truth. The rules products consume at run time are generated from it as
authored CHIR assets, swapped live per jurisdiction as reviewed generations and queryable over `ceridc` and MCP; that
generator and query belong to the planned `comply` owner (`compliance.obligations` in the
[capability registry](capability-ownership.md)) and land in COMPLY.0, and no governed row writes a second rule
engine.

<a id="snapshot-and-review"></a>
## Snapshot and review

The first snapshot (2026-10-31) was assembled from the networking research read on 2026-10-27 (sources N01 to N124)
and the network foundation design; entries the research marks as knowledge or secondary carry `verified: false`.
Codepoints from knowledge are marked the same way, and an entry cannot become `implemented` until they are checked
against the IANA registries.

The legal register's first snapshot (2026-10-31) was assembled from the obligations the COMPLY rows name (the user's
decision of 2026-10-27) and the publishing and networking research read on 2026-10-27 (sources S02 to S54, N21 and
N120). Those sources are law-firm, regulator-news and secondary summaries, so every entry carries `verified: false`
and a null `consolidated`, and the primary URLs are taken from knowledge; a URL that is still a journal's or
regulator's home page is replaced by the instrument's own document when it is read, which the guard requires.

The review cadence is a light scan after each IETF meeting (March, July and November) and a full re-research every
year or when a pinned item changes status; each run proposes ROADMAP amendments for the user and never edits rows
itself. The register's `review` holds it as data:

| Field | Meaning |
|---|---|
| `cadence` | The cadence in words |
| `lag_days` | Days after a calendar event by which its scan is owed (25: minutes and revised drafts are out) |
| `full_every_days` | Days after the latest full run by which the next full re-research is owed (365) |
| `calendar` | The events that owe a scan, in day order: `event`, `day` (its last day), `scan` (`light` or `full`), `verified` (false for a day taken from knowledge, which the next review confirms) |
| `runs` | Every run, in day order: `day`, `scan`, `ref` (its report or research document) and `triggers` (the pinned entries whose status it found changed) |
| `next` | The due day `review_due` computes, which the guard holds |

The due day is the earliest of: `lag_days` after each calendar event later than the latest run (a run covers the
events up to its day); `full_every_days` after the latest full run; and `lag_days` after a light run with triggers
that no full run has followed. The legal register's calendar holds its quarterly reviews, the yearly platform events
(the April SDK rule, Google I/O, WWDC, the August target API rule) and the dated effective days it holds (Law No.
5651's game-platform duties on 2026-11-01 first, so its first scan is due on 2026-11-26); an Official Gazette,
EUR-Lex or Steamworks change found between them is recorded as a run with its triggering entries.

[scripts/register-review.py](../../scripts/register-review.py) runs a review:

1. `due` prints the review owed, why, and the calendar events still unverified.
2. `plan --out <file>` writes the primary sources a refresh reads: the RFC Editor record
   (`rfc-editor.org/rfc/rfcNNNN.json`) of every RFC, the IETF datatracker record
   (`datatracker.ietf.org/doc/<draft>/doc.json`) of every draft and the IANA registry files
   (`iana.org/assignments/<name>/<name>.xml`) of the codepoints, plus the items read by hand: W3C and NIST documents,
   object identifiers (NIST CSOR) and the IETF meeting calendar. For the legal register
   (`--register cmake/legal-register.json`) it lists the official primary text of every instrument, once per URL,
   and the Official Gazette, EUR-Lex and platform announcements read by hand since the latest run.
   **Reading them is a download the user approves first; the plan is what is approved.**
3. `fetch --plan <file> --into <dir>` reads the approved sources (HTTPS only, at most 16 MiB each) with a manifest of
   their SHA-256 digests; a source that fails is recorded, never retried silently.
4. `compare --fetched <dir> --report <file> [--scan light|full] [--apply]` verifies the digests and judges each record:
   a draft's new revision, posted day and status (active, IESG processing, RFC Editor queue, expired, replaced,
   published as an RFC), an RFC obsoleted, and each IANA codepoint confirmed, named differently, assigned another value
   or absent; for a legal instrument, its text's SHA-256 against the digest recorded when it was read in its
   consolidated form (never read, or changed since). The report lists the register changes, the instruments to read
   in their consolidated text, the **proposed ROADMAP amendments for the user**, the pinned
   entries whose status changed (each triggers a full re-research), the sources not read and the items read by hand.
   `--apply` writes the register changes (statuses, revisions, a confirmed codepoint loses `verified: false`, the
   snapshot day), records the run with its triggers and moves `next`; it never opens the ROADMAP for writing. A
   codepoint IANA contradicts is a proposal, never applied: the generated headers change only by review.
5. `record --scan <scan> --ref <document#anchor> [--day <day>] [--trigger <entry>]` records a run made by hand (a
   full re-research document, or a gazette change with the pinned entries it changed, which owe a full re-research)
   and moves `next`.

The record shapes the readers expect were written from knowledge of the three services; a record without them is
reported as unreadable and changes nothing, and the first approved refresh confirms them. The calendar points at which
the guard fails without a refresh are the register's `review.next` (2026-12-15, after IETF 127) and the first pinned
draft's expiry, which the guard prints (`webtransport.h3`, 2027-01-02, counted conservatively from July 2026).

## Cerid identifiers

The Cerid wire protocols of the [network foundation](network-foundation.md#negotiation) are entries of type `cerid`
with their protocol identifiers (session control `0x0001` to relay allocation `0x0008`) in the "Cerid protocol
identifiers" registry. The "Cerid GREASE values" registry holds the pattern `0x?A?A` (the same digit in each `?`:
`0x0A0A`, `0x1A1A` ... `0xFAFA`, the RFC 8701 pattern): reserved as extension identifiers and as version majors,
never assigned, and sent by every build. Identifiers are never reused; a retired one stays in the register as
`deprecated`.

The "Cerid algorithm identifiers" registry names every algorithm a policy generation or another signed artefact
refers to, so a signed artefact never carries an IANA value taken from knowledge or a provider's constant. The high
byte is the class and the low byte (never 0) numbers the algorithm within it:

| Class byte | Class | Assigned |
|---|---|---|
| `0x01` | Post-quantum/traditional hybrid key exchange | X25519MLKEM768 `0x0101`, SecP256r1MLKEM768 `0x0102`, SecP384r1MLKEM1024 `0x0103` |
| `0x02` | Traditional elliptic-curve key exchange | X25519 `0x0201`, secp256r1 `0x0202`, secp384r1 `0x0203` |
| `0x03` | Post-quantum signature | ML-DSA-44 `0x0301`, ML-DSA-65 `0x0302`, ML-DSA-87 `0x0303` |
| `0x04` | Traditional signature | Ed25519 `0x0401`, ECDSA P-256 with SHA-256 `0x0402`, ECDSA P-384 with SHA-384 `0x0403` |
| `0x05` | Authenticated encryption | AES-128-GCM `0x0501`, AES-256-GCM `0x0502`, ChaCha20-Poly1305 `0x0503` |
| `0x06` | Key derivation | HKDF-SHA256 `0x0601`, HKDF-SHA384 `0x0602` |

The class is what the trust set's hybrid rule and a provider's dispatch read; an identifier is never reused.

Code never spells a codepoint by hand. [scripts/gen-register-ids.py](../../scripts/gen-register-ids.py) writes, for
each owning module, a committed header of the codepoints its entries hold (for `net`,
[register_ids.hpp](../../engine/foundation/net/include/crd/net/register_ids.hpp): `ProtocolId`, the GREASE table and
the QUIC and ALPN values; for `crypto`,
[register_ids.hpp](../../engine/foundation/crypto/include/crd/crypto/register_ids.hpp): `AlgorithmId`,
`AlgorithmClass` and the TLS group, signature scheme, extension, AEAD and object identifier values). A codepoint
marked `verified: false` is written into a nested `unverified` namespace, so a use of it is visible where it is made
and verifying it moves it out. A registry without a C++ type, a malformed value or object identifier, a pattern that
is not 16 bits, an algorithm identifier outside the class table or numbered 0 within its class and a registered owner
with codepoints but no output each fail the generator. The `crd-net-register-ids-drift` and
`crd-crypto-register-ids-drift` CTests run `--check`, and
[scripts/generated-sources.json](../../scripts/generated-sources.json) records each header's bytes.
