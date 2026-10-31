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
`codepoints` (IANA or Cerid registry, name, value; `verified: false` marks a value taken from knowledge),
`cryptography` (the cryptographic functions the entry performs, [below](#crypto-classification)) and, at the
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

The JSON register is the guarded source of truth. The rules products consume at run time are generated from it,
swapped live per jurisdiction as reviewed generations and queryable over `ceridc` and MCP; the generator, the store
and the query belong to the `comply` owner (`compliance.obligations` in the
[capability registry](capability-ownership.md), module `crd-comply`), and no governed row writes a second rule
engine.

<a id="rule-generations"></a>
### Rule generations

- **One generation per jurisdiction.** [scripts/gen-comply-rules.py](../../scripts/gen-comply-rules.py) writes
  `assets/comply/<code>.rules.toml` (the code in lower case: `tr`, `eu`, `us-ca`, `int`, `platform`) for every
  jurisdiction holding a rule: an `obligation` or `user-action` entry that is `planned` or `implemented`. Watched and
  deprecated entries are not rules. The file is `cerid-rules/1`, canonical UTF-8 with LF in the TOML subset
  `crd-toml` reads (strings, integers, booleans, arrays and arrays of tables; days are `YYYY`, `YYYY-MM` or
  `YYYY-MM-DD` strings, an absent day is the empty string): `format`, `jurisdiction`, `generation`, `register` (the
  snapshot day), then one `[[scope]]` table per scope its rules name (`id`, `kind`, `state`) and one `[[rule]]` table
  per entry in register order (`id`, `kind`, `topic`, `state`, `title`, `instrument`, `article`, `primary`, `read`,
  `obligation`, `applies_to`, `check`, `check_name`, `check_ref`, `effective`, `sunset`, `counsel`,
  `counsel_reviewed`, `rows`). `counsel` is `none`, `needed` or `reviewed`. `--check` (the `crd-comply-rules-drift`
  CTest) byte-compares every file and refuses a stray one.
- **Reviewed.** `rule_generations` in the register records, per jurisdiction with rules, its `generation` (from 1),
  the day it was `reviewed` and the `sha256` of the committed file. The guard fails a file whose digest differs from
  its record (the rules changed and nobody reviewed them), a record without a file or a file without a record, and a
  review day after today. `gen-comply-rules.py --review <code> [--day YYYY-MM-DD]` is the review: it refuses when the
  rules are unchanged, otherwise increments the generation, writes the file and records the day and digest. The
  JSON stays the authored source until CHIR can hold structured values ([LANG.2](../ROADMAP.md#slice-lang.2)); the
  CHIR projection then lowers to the same generation.
- **Swapped live.** `crd::comply::RuleStore` holds one generation per jurisdiction. `apply` decodes and validates
  the whole text before anything changes (bounded sizes, every key known and typed, days real calendar days, scopes
  unique and every `applies_to` naming one, the enumerations closed) and adopts it only above the current number:
  a lower number is a rollback, the same number with other bytes (XXH3-64) a conflict and the same bytes already
  current, all refused with the store unchanged. A query runs under the store's lock, so it sees one whole generation
  of each jurisdiction, never a mix. Authenticity on delivery (a signed generation from the publisher) is the
  envelope NET.2 gives every signed artefact, through `crd-crypto`.
- **Queried.** A query names a scope, the jurisdictions a product serves and a day, and answers every rule of those
  jurisdictions that applies to the scope and is in force that day (effective on or before it, its sunset after it),
  with the scope's state, the check, the counsel status and whether the text was read. A rule is `proven` only when it
  is implemented with an active check (implemented entries carry evidence, the guard holds that); every other rule in
  force is `unproven`, so no consumer, human or agent, can read an obligation as satisfied without evidence. Rules of
  other scopes, or not yet effective, are not answered; `ceridc comply` prints the answer as JSON and the MCP tool
  serves the same query once [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt) gives commands one declaration for every
  surface.

<a id="crypto-classification"></a>
### Cryptography inventory and classification (COMPLY.1)

Export control classifies what an export does with cryptography, so the facts are read from the build and the
standards register, never typed, and a change to them reaches no release without a reviewed classification.

- **Functions and key lengths are register facts.** Every standards entry carries `cryptography`: the functions it
  performs, each once in the order `authentication`, `confidentiality`, `integrity`, `key-exchange`,
  `key-derivation` (empty when none; a protocol whose packets are protected, such as QUIC or TLS, names all five, and
  a protocol riding on one names only what it adds). The class byte of a Cerid algorithm identifier fixes functions
  its entry must name (key exchange for 0x01 and 0x02, authentication for 0x03 and 0x04, confidentiality and
  integrity for 0x05, key derivation for 0x06; `ALGORITHM_CLASSES` in
  [gen-register-ids.py](../../scripts/gen-register-ids.py) is the one table), and each of its codepoints carries
  `key_bits`: one length per component, in the order the name gives. A symmetric or elliptic-curve key states its
  length as its specification encodes it (AES-128 128, ChaCha20 and the 32-byte X25519 and Ed25519 keys 256, P-384
  384), a key derivation its hash output, and a lattice algorithm its encoded public key (ML-KEM-768's 1184 bytes are
  9472 bits; ML-DSA-44, -65 and -87 are 10496, 15616 and 20736), so `x25519-mlkem768` is `[256, 9472]`. Key bits
  elsewhere, a missing or malformed length, and a function its class performs but the entry omits each fail the
  guard.
- **Exports are read from the build.** An export is the relocatable package (every engine module whose
  `CMakeLists.txt` calls `crd_package_library`; id `cerid-package`) and every program module the root `CMakeLists.txt`
  registers outside `engine/` (the host tools, the runtime and the sandbox; id the module's name). Product exports
  join through the same reading when the publishing profiles of [SHIP.0](../ROADMAP.md#slice-ship.0) exist.
- **The inventory.** [scripts/gen-crypto-inventory.py](../../scripts/gen-crypto-inventory.py) walks each export's
  module closure (`DEPENDS`) and writes `assets/comply/crypto-inventory.json` (`cerid-crypto-inventory/1`): per export,
  its third-party packages (`PACKAGES`), every implemented standards entry its modules own whose `cryptography` is not
  empty (id, kind, owner, functions, specification, whether it is a published standard) and every implemented
  algorithm among them (name, identifier, class, functions, key lengths, entry, specification, owner), then the union
  of functions and whether it performs `confidentiality`. Planned entries are in no export. Third-party cryptography
  enters only through the crypto owner's providers (the `crypto.providers` signatures refuse a crypto library header
  in any other module), so the register entries are the whole of it. `--check` is the
  `crd-comply-crypto-inventory-drift` CTest and the file is in
  [generated-sources.json](../../scripts/generated-sources.json).
- **Classification records.** `crypto_classifications` in the legal register maps each export to its records: one per
  release, frozen, and last one head record (`release` null) for the next. A record holds `inventory` (the SHA-256 of
  the export's inventory in canonical JSON: sorted keys, no spaces), `reviewed`, `findings` and `counsel`. A finding
  names an export-control `entry`, its `classification`, the `authorization` it ships under, whether a `report` is
  owed and its `basis` in plain words. The head record answers exactly the pinned obligations whose active check cites
  the generator (today the US EAR, EU dual-use, Wassenaar and the two Turkish entries); the classification needs
  counsel when any of them does, and it is a proposal until counsel answers: interpretation is never guessed.
- **The guard.** `crd-check-registers` recomputes every inventory and fails an export without records, a head record
  that is not last or not alone, an export whose inventory differs from the one its head record was reviewed against,
  findings that miss or repeat an obligation or name a non-export-control entry, a counsel answer outside the record's
  review and today, releases that are not distinct `MAJOR.MINOR.PATCH[-PRE]` versions, a release recorded before
  counsel reviewed it, and a head record kept for something that is no longer an export (its released records stay).
- **The review.** When an inventory changes, the reviewer restates the head record's findings and runs
  `--review <export> [--day D]`, which records the new digest and day and clears `counsel.reviewed` (a changed
  inventory goes back to counsel); it refuses an unchanged inventory and a head record without findings.
  `--release <export> <version>` freezes the current, counsel-reviewed head record for a release.
- **Facts for stores.** `--facts <export>` prints what the store compliance kit ([COMPLY.8](../ROADMAP.md#slice-comply.8))
  turns into the Apple export compliance answer and the EAR self-classification report: the inventory digest, review
  and counsel status, the functions and algorithms with their key lengths, any non-standard algorithm performing
  confidentiality, the findings, `apple.uses_non_exempt_encryption` (true exactly when the export performs
  confidentiality; authentication and integrity alone are exempt uses) and the findings that owe a report. It refuses
  an export whose inventory changed since its review.

<a id="screening"></a>
### Sanctions and embargo screening (COMPLY.1)

Distribution, accounts and payments are screened against every list the product's jurisdictions require (the
`us.sanctions`, `eu.restrictive-measures`, `int.un-sanctions`, `gb.sanctions` and `tr.asset-freeze` entries, whose
active check is the screening tests). The `comply` owner holds the lists, the matcher and the screening; the STORE
rows call it and write no second matcher (`compliance.obligations`, whose signatures refuse a Jaro-Winkler
elsewhere).

- **Lists are generations.** Each authority's list arrives as one `cerid-sanctions/1` document imported from the
  authority's own publication: `authority` (dotted, such as `us.ofac`), `jurisdiction`, `generation` (the
  authority's publications in order), `published`, `max_age_days` (1 to 366), the official file's `source` URL and
  `source_sha256`, then `[[destination]]` tables (`region`: ISO 3166-1 alpha-2 or an ISO 3166-2 subdivision,
  `program`, `measure`: `comprehensive` or `sectoral`) and `[[party]]` tables (`id`, `kind`: individual, entity,
  vessel or aircraft, `names`: the primary name then the aliases, well-formed UTF-8, `programs`, `listed`, `births`:
  YYYY, YYYY-MM or YYYY-MM-DD, individuals only, `countries`: ISO 3166-1 alpha-2, `identifiers`: `type:value`). A list
  is decoded whole within its bounds (64 MiB, 131 072 parties, 1 024 destinations, 512 names and 64 other items a
  party) and `SanctionsStore` adopts it only above its authority's generation, swapping it under the store's lock;
  rollbacks, conflicts and repeats are refused with the store unchanged, as rule generations are. Authenticity on
  delivery is NET.2's signed envelope.
- **Importers come with the first read.** The official formats (OFAC's SDN and consolidated lists, the EU's financial
  sanctions file, the UN Security Council's consolidated list, the UK sanctions list and Turkey's asset-freeze
  decisions) are read from the files themselves, never from memory; the importers that write `cerid-sanctions/1`
  land with the first read, which is a download the user approves.
- **Folding.** Names are compared in search-folded form (`text.search-folding`, crd-core's
  [text_fold.hpp](../../engine/foundation/core/include/crd/core/text_fold.hpp)): lower case; Latin letters with
  diacritics, ligatures and fullwidth forms to ASCII (Ş and ş to s, İ and ı to i, ø to o, ß to ss, æ to ae, the
  Azerbaijani schwa to a); Greek and Cyrillic without case or accents; combining marks and apostrophes elided;
  punctuation and spaces as separators. [gen-text-fold.py](../../scripts/gen-text-fold.py) derives the table from the
  Unicode Character Database for the blocks sanctions lists print names in, holds every fold to a fixed point and is
  the `crd-core-text-fold-drift` CTest; outside those blocks a code point is a letter kept as it is. Scripts are not
  transliterated into each other: the lists carry Latin aliases.
- **Matching** ([names.hpp](../../engine/foundation/comply/include/crd/comply/names.hpp)). Honorifics and legal forms
  go unless nothing else is left (a legal form spelt in single letters, A.Ş. or L.L.C., included). Each word pairs with
  at most one word of the other name, best pairs first, rated by Jaro-Winkler, an initial against a word it starts
  (0.9), and at least 0.9 when both spell one consonant skeleton (Muhammad and Mohamed); pairs below 0.88 do not
  pair. The score (0 to 1000) is the highest of the pairs' similarity weighted by both names' word lengths; when the
  word counts differ and the shorter name has two words or more, its covered share less 0.05 for each unpaired word
  of the longer (a missing middle name or patronymic scores 950); and, when the word counts differ, the Levenshtein
  similarity of the words written together (split and joined spellings). Word order never matters.
- **Dispositions.** `ScreenPolicy` holds the thresholds (review 850 and match 950 by default). A party whose best
  name scores at least the review threshold, or whose identifier agrees, is a hit: **Discounted** when the subject's
  kind or birth contradicts it (reported, never dropped), **Match** when an identifier agrees or the score reaches the
  match threshold with a birth or country that agrees, else **Possible**. A destination is a **Match** when the
  request names the embargoed region or a place inside an embargoed country, and **Possible** when the request names
  only the country an embargoed region belongs to.
- **Fail closed.** A list the request names that the store does not hold is **missing**; one screened after its
  publication day plus its maximum age is **stale**, and one screened before its publication day is **not yet
  published**. A stale list is still screened (what it lists blocks), and any gap keeps the verdict from Clear.
- **Verdict.** **Blocked** when a party matches or a destination lies in a comprehensive embargo; else
  **Unavailable** when a list has a gap; else **Review** for a possible party, a sectoral embargo or a country whose
  embargoed region the request does not exclude; else **Clear**. A malformed request (no authority, an impossible day,
  thresholds out of order, a subject without words or with a bad birth, country or identifier, a bad region) is
  refused with its key.
- **Served.** `ceridc screen --lists <dir> --authority <id,...> --day <YYYY-MM-DD>` screens a destination
  (`--destination`), a subject (`--kind` and repeated `--name`, with `--birth`, repeated `--country` and
  `--identifier`) or both, with optional `--review` and `--match` thresholds. Each authority's list is the file
  `<dir>/<authority>.sanctions.toml`; one that cannot be read is a missing list, so the answer fails closed. A
  subject's kind is required with its names, never assumed, because a wrong kind discounts the very party that
  matches. The module's own request rules judge the request against an empty store before any file is read (an
  authority's shape among them, which keeps every file name inside `<dir>`). A malformed list is refused with its
  file, error, line and key, and a file holding another authority's list is refused. The answer lists each
  authority's generation (0 when absent), the verdict, the gaps and every party and destination hit. The subject is
  screened, never echoed. `ok` is true for every verdict, Blocked included: the verdict is the answer. The MCP tool
  serves the same screening once [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt) gives commands one declaration for
  every surface.
- **Tests.** `crd-comply-tests` (`[names]` and `[sanctions]`: folding, both sides of the review threshold, every
  refusal, the store, every disposition and verdict, a concurrent applier and screener, a list of 20 000 parties),
  the `crd-fuzz-comply-sanctions` corpus and `crd-ceridc-tests` `[screen]` (every verdict and hit field, failing
  closed, every refusal, the real binary's bytes).

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
