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
`codepoints` (IANA or Cerid registry, name, value; `verified: false` marks a value taken from knowledge;
`recommended`, IANA's "Recommended" value, [below](#recommended-column)),
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
- <a id="recommended-column"></a>**IANA's recommendation is recorded.** The TLS Supported Groups, SignatureScheme
  and ExtensionType registries carry a "Recommended" column (RFC 9847): `Y` (recommended for its purpose), `N` (not
  evaluated, or of limited applicability) or `D` (discouraged). Every verified codepoint of those registries records
  IANA's value as `recommended`; no other codepoint carries one; a pinned entry with a `D` codepoint fails until the
  codepoint is dropped or the entry deprecated. The review reads the column and keeps it current
  (`tls.registry-recommended`).
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
| `counsel` | `needed` (interpretation needs counsel; never guessed) and `reviewed` (the day counsel answered; a [counsel review](#counsel-reviews) answered that day asked about it) |

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
- **Counsel never blocks, and is never guessed.** An entry is implemented, and its rows close, once its engineering
  is proven; an outstanding counsel item is an open question in a [counsel review](#counsel-reviews), never a missing
  clause (user decision 2026-11-01). Nothing in the register claims legal sign-off: a rule whose counsel is outstanding
  says so in every answer, and an agent never marks an obligation satisfied without an active check and evidence.
- **Turkish law is never omitted.** Every topic the register holds, the platform policies aside, has a Turkish entry.

<a id="counsel-reviews"></a>
### Counsel reviews

The user decided on 2026-11-01 that counsel review never blocks a row: the publisher (the user's Turkish company)
consults counsel later, and every item that needs counsel is recorded as a dated, named review with the exact question
counsel is asked. `counsel_reviews` lists the reviews:

| Field | Meaning |
|---|---|
| `id`, `title` | The review's name (for example `counsel.2026-11-01`) and what it covers |
| `opened` | The day the review was opened |
| `answered` | The day counsel answered every question (null while open) |
| `questions` | One item each: `entry` (an entry's id) or `classification` (an export whose head classification record needs counsel) and the exact `question`, ending with "?" |

The guard (`crd-check-registers`) holds the reviews to the items' `counsel` fields:

- **Every outstanding item is asked about.** An entry or a head classification record whose counsel is needed and not
  yet reviewed is asked about in exactly one open review.
- **An open review asks only what is outstanding.** A question about an item whose counsel is not needed, or already
  answered, fails until it is dropped or its answer recorded.
- **Every answer has its review.** An item's `counsel.reviewed` day is the `answered` day of a review that asked about
  it; answering a review records that day on each item it asked about, in the same change.
- **Dates are in order.** A review opens on or before today and is answered between its opening and today; ids are
  unique and an item is asked about once per review.

A changed cryptography inventory clears its head record's answer (`gen-crypto-inventory.py --review`), so its question
goes into an open review again. Releasing a classification still needs counsel's answer
([below](#crypto-classification)): a release is the publisher's act, never a row's clause. The guard's summary line
counts the open questions.

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
  inventory goes back to counsel: its question joins an open [counsel review](#counsel-reviews)); it refuses an
  unchanged inventory and a head record without findings.
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
  vessel or aircraft, `names`: the primary name then the aliases, well-formed UTF-8, `programs`, `listed`: the
  designation day, empty when the list does not say (OFAC's files), `births`: YYYY, YYYY-MM or YYYY-MM-DD, or two of
  them joined by `/` (an ISO 8601 interval, from the first's first day to the second's last), individuals only,
  `countries`: ISO 3166-1 alpha-2, `identifiers`: `kind[.cc]:value`). An identifier's kind is lower-case words
  joined by `-` (`passport`, `national-id`, `tax-id`, `registration`, `imo`, `mmsi`, `swift`, `lei`, `crypto-xbt`,
  `email`, `website`, ...), `.cc` the issuing country of a document or registration in lower case when the list
  names it, and the value is compared as its ASCII letters and digits in lower case; two identifiers agree when their
  kinds and values are equal and their countries are equal or one of them names none, so a Turkish passport number
  never agrees with the German passport of the same digits. A list is decoded whole within its bounds (64 MiB,
  131 072 parties, 1 024 destinations, 512 names, 1 024 identifiers and 64 other items a party: OFAC lists 134
  digital currency addresses on one party) and `SanctionsStore` adopts it only above its authority's generation,
  swapping it under the store's lock; rollbacks, conflicts and repeats are refused with the store unchanged, as rule
  generations are. Authenticity on delivery is NET.2's signed envelope.
- **Importers read the official files** ([scripts/import-sanctions.py](../../scripts/import-sanctions.py)). The legal
  register's `sanctions_lists` names each authority's official file: the screening entry it serves, the canonical
  URL (on an official host of that entry's jurisdiction), the hosts the URL may redirect to (OFAC and the UN answer
  with a signed link to their file stores; any other host is refused), the importer format, the generation's maximum
  age and the record of the file read (day, publication, bytes, SHA-256, parties). `fetch` downloads each file over
  https with the register-review client and says whether it is the file the register recorded; `import` writes
  `<out>/<authority>.sanctions.toml`; `record` records a reviewed refresh. The raw files stay outside the repository
  (re-fetched and verified by digest); real-byte excerpts of each format, entries cut whole, are the importer's test
  inputs (`tests/foundation/comply/lists`). Authorities, formats and the first read (2026-11-01):

  | Authority | List | Format | Maximum age |
  |---|---|---|---|
  | `us.ofac.sdn` | OFAC's SDN List | OFAC XML | 31 days |
  | `us.ofac.cons` | OFAC's Consolidated (non-SDN) List | OFAC XML | 92 days: it changes rarely (the file read was 13 days old) |
  | `eu.fsf` | the EU's financial sanctions file (FSF 1.1) | FSF XML | 31 days |
  | `un.sc` | the UN Security Council Consolidated List | UN XML | 7 days: regenerated daily |
  | `gb.fcdo` | the UK Sanctions List | UK XML | 31 days |

  A generation's number is its publication instant as YYYYMMDDhhmmss (the EU's and the UN's files carry it); a file
  stamped only with its day (OFAC's, the UK's) takes YYYYMMDD000000 and counts up from the previous generation for a
  second publication that day; a file already imported is left as it is and an older publication is refused. Every
  reader fails closed: an element, identifier type, country name, calendar or date shape it does not know refuses
  the import with the party and the value, and a document type declaration is refused before the tree is built.
  Names are the primary name, every alias (weak ones too: a hit is at most possible without an agreeing birth,
  country or identifier) and the names in their own scripts; countries are nationality, citizenship, residence,
  registration, an identifier's issuing country and a vessel's flag (an authority's "false" flag, a former state or a
  disputed region records none); OFAC's descriptive idList items (sanctions information, dates, gender, a phone number
  printed in fragments) are read and never compared. Births keep what the list says and never narrow it: a range
  (OFAC's "1961 to 1962", the EU's year range with an open end running to 1900 or 9999, the UN's BETWEEN) is one
  interval; an approximate date (circa, APPROXIMATELY) records no period, so it never contradicts a subject; a Hijri
  date is its tabular Gregorian day two days either side (the observed and Umm al-Qura months start up to two days
  from it: the UN's 24/06/1402 is 17 to 21 April 1982, the EU printing 19 April); an Islamic-calendar year without a
  day is both its lunar and its Solar Hijri year (the EU prints Iranian births in the Solar Hijri year under
  ISLAMIC); the UK's `dd/mm/19yy` is its century. Turkey's asset-freeze decisions (Laws No. 6415 and 7262) are
  `pending`: MASAK publishes them through a web application with no list file at a stable address, so they are read
  from the Official Gazette and MASAK next. A list's maximum age is a backstop measured from its publication day;
  that a held list is still the authority's current one is the refresh's knowledge, carried with NET.2's signed
  delivery. The embargoed destinations are none of these files: each authority's comprehensive and sectoral regimes
  are authored generations of their own from the regulations read (31 CFR chapter V, the EU's Article 215
  regulations, the UK's regime regulations, the UN resolutions). Until those generations land (a remaining clause of
  COMPLY.1), a destination screened against the designation lists alone finds no embargo.
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
  refusal, the store, every disposition and verdict, identifiers with and without their issuing country, birth
  intervals, the identifier bound, a concurrent applier and screener, a list of 20 000 parties), the
  `crd-fuzz-comply-sanctions` corpus, `crd-ceridc-tests` `[screen]` (every verdict and hit field, failing closed,
  every refusal, the real binary's bytes), the importer's Python cases (`SanctionsLists` in
  `scripts/test-repository-tools.py`: every format on the excerpts, every refusal, generations, calendars, the
  register's `sanctions_lists` guard) and the official formats end to end: `crd-comply-sanctions-import` imports the
  excerpts into an emptied directory and `crd-comply-sanctions-imported` (the hidden `[.imported]` case) decodes,
  adopts and screens what it wrote.

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
   `--scan full` adds the full text of every document the owed full re-research reads: the documents the triggering
   entries of the light runs since the latest full run pin (`rfc-editor.org/rfc/rfcNNNN.txt`, a draft's
   `ietf.org/archive/id/<draft>-<revision>.txt`, or a W3C or NIST document read by hand); the plan records its scan.
   **Reading them is a download the user approves first; the plan is what is approved.**
3. `fetch --plan <file> --into <dir>` reads the approved sources (HTTPS only, at most 16 MiB each) with a manifest of
   their SHA-256 digests; a source that fails is recorded, never retried silently.
4. `compare --fetched <dir> --report <file> [--scan light|full] [--apply]` verifies the digests and judges each record:
   a draft's new revision, posted day and status (active, IESG processing, RFC Editor queue, expired, replaced,
   published as an RFC), an RFC obsoleted, and each IANA codepoint confirmed, named differently, assigned another value
   or absent, with its "Recommended" value where the registry has the column (a pinned codepoint that becomes `D`
   triggers a full re-research); for a legal instrument, its text's SHA-256 against the digest recorded when it was read in its
   consolidated form (never read, or changed since). The report lists the register changes, the instruments to read
   in their consolidated text, the **proposed ROADMAP amendments for the user**, the pinned
   entries whose status changed (each triggers a full re-research), the sources not read and the items read by hand.
   `--apply` writes the register changes (statuses, revisions, a confirmed codepoint loses `verified: false`, the
   Recommended values, the snapshot day), records the run with its triggers and moves `next`; it never opens the
   ROADMAP for writing. A codepoint IANA contradicts is a proposal, never applied: the generated headers change only
   by review. `--scan full` compares only a full plan, and applied it records `--ref`, the re-research document,
   which must cite the report; the report lists the documents read in full.
5. `record --scan <scan> --ref <document#anchor> [--day <day>] [--trigger <entry>]` records a run made by hand (a
   full re-research document, or a gazette change with the pinned entries it changed, which owe a full re-research)
   and moves `next`.
6. `read --fetched <dir> --ref <document#anchor> --entry <id> ...` (legal register) records the instruments a person
   read from the fetch the applied run compared (the register's snapshot day): each entry's `consolidated` version
   (its host's reader), the fetch day and the document's SHA-256, and its `source` verified and cited by `--ref`, the
   document recording what was read against each obligation. An entry whose document was not read, is unreadable or
   sits on a host without a reader is refused, and a refusal writes nothing.

<a id="official-texts"></a>
**Official texts.** A legal instrument's `primary` is the page a person cites; the plan resolves it to the document its
host serves as data, read by the host's reader, which checks the document is the act cited and finds its version
(host checks of 2026-11-01, [session](../sessions/2026-11-01-comply-0-counsel-reviews.md#host-checks)):

| Host | Document read | Version |
|---|---|---|
| mevzuat.gov.tr, a law (`mevzuat?MevzuatNo=..&MevzuatTur=1`) | the consolidated text the page frames (`anasayfa/MevzuatFihristDetayIframe`), which names the law's number | the latest day of its amendment notes, with the amending act (`2024-03-02 (7499)`), a Constitutional Court annulment's decision day, or the day of enactment |
| mevzuat.gov.tr, a by-law or regulation (`MevzuatTur=7`) or communiqué (`MevzuatTur=9`) | an index read first: the cited page, which names the Official Gazette that published it; then the framed text, whose title names its form and whose closing table, once amended, repeats that gazette | the gazette and the latest change, from the amending gazettes' notes and the closing table or a court's annulment (`2014-11-27/29188 at 2026-05-06 (Danıştay)`), or `as published`; any other kind is read by digest alone |
| eur-lex.europa.eu (`eli/<reg\|dir\|dec>/<year>/<number>/oj`) | CELLAR (publications.europa.eu, which serves EUR-Lex's acts; eur-lex.europa.eu answers a machine with a challenge): the English XHTML of the version | an index read first: CELLAR's SPARQL endpoint lists the act's consolidated versions with an English XHTML text, and the latest in force on the fetch day is read (`02016R0679-20160504`), else the act as adopted (`32019L0790`) |
| legislation.gov.uk (`ukpga/<year>/<number>`) | the act's Crown Legislation Markup (`data.xml`) | its point-in-time day (`dct:valid`) |
| ecfr.gov (`current/title-N/...`) | the eCFR versioner's full XML on the hierarchy's latest amendment day: the part itself, or the whole title above a part (the versioner ignores subtitle, chapter and subchapter), of which the cited hierarchy's subtree is the text read | an index read first: the versioner's `meta.latest_amendment_date` of the hierarchy, which must echo the hierarchy asked for |
| wassenaar.org (`control-lists/`) | the List of Dual-Use Goods and Technologies (PDF) | an index read first: the control-lists page's one link to the list, whose upload path names its edition |

A court's annulment note counts on its decision's day only, never on the days of the text it quotes. A version index
is fetched before the documents whose URL holds the `{version}` it names, and a version is never a scheme, query or
`..`. Every request asks for gzip (eCFR refuses one that does not), and the manifest pins the
SHA-256 of the decoded body, at most 16 MiB either way; any other content encoding is refused. An instrument's
`consolidated.sha256` is that of the text read: the document, or an eCFR hierarchy's subtree, so an amendment elsewhere
in the title does not read as a change. A redirect is followed on the same host
only, with an http Location upgraded to https (CELLAR answers with http, and its https form serves the same bytes);
another host or scheme is refused, so a moved document is a source not read until its `primary` is corrected. A host
without a resolver is read as the cited page by its digest alone and cannot be recorded as read: its instrument is
cited on a host with a reader first. A scan judges a read instrument twice: a new version is an amendment to read
(`amended to <version>`), and a new digest under the same version is a change to the text.

The readers follow the records the first approved refresh read (2026-10-31,
[report](../sessions/2026-10-31-standards-register-refresh.md)); a record without their shape is reported as unreadable
and changes nothing. An IANA record's `value` is a number, or the record's name where the registry keys by name (ALPN's
`HTTP/3`); its `description` is a name, or the value an identification sequence spells (`0x68 0x33 ("h3")`); a range
names nothing. A registry whose records carry no name (QUIC Versions) confirms a value only when the record cites the
entry's own document (`<xref type="rfc">`, or a draft without its revision); a named record confirms by its name alone.
The "Recommended" column is `<rec>` in the TLS Supported Groups and ExtensionType files and `<recommended>` in the
SignatureScheme file; a confirmed codepoint whose record has no single value of it is reported as not read.
The report pins every record read (source, URL, bytes, SHA-256, retrieval day); the bodies stay outside the
repository, under `~/.cerid/downloads/register-review/`, and the same plan fetches them again.

The first refresh confirmed every IANA codepoint (the generated headers lost their `iana::unverified` namespaces), read
the NIST object identifiers, the FIPS and W3C statuses and the IETF meeting calendar by hand, and found RFC 8446
obsoleted by RFC 9846: `tls.v13` is re-pinned to RFC 9846, and the run's trigger owed the full re-research by
2026-11-25. The full run of 2026-11-01 read RFC 9846 in full
([re-research](../research/2026-11-01-tls-13-rfc-9846-re-research.md), [report](../sessions/2026-11-01-standards-register-full-review.md)):
it recorded the Recommended column for the first time, added the ML-DSA signature schemes for TLS 1.3
(`tls.ml-dsa`) and RFC 9847 (`tls.registry-recommended`), and moved `next` to the light scan after IETF 127. The calendar's other point is the first pinned draft's expiry, which the guard
prints (`webtransport.h3`, posted 2026-07-06).

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
