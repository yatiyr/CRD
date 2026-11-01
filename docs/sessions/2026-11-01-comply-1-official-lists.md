# COMPLY.1: the official sanctions lists, read and imported

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C, batch 16; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening).

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE".

Among the approved downloads were COMPLY.1's official sanctions lists: OFAC's SDN and Consolidated lists, the EU
financial sanctions file, the UN Security Council Consolidated List, the UK Sanctions List and Turkey's asset-freeze
decisions. This batch reads the first five from their official sources, pins each file and writes the importers
against the real files. The previous batch's Next named the remaining legal instruments first and the lists fourth.
The lists came first here because they are perishable snapshots the user approved for download, and the five
screening entries already cite the screening as their active check. The instrument reading is the next batch's work.

## CI first

No failure comes from a track C change that main has not already fixed.

- **Run 41254049879 (31404ec4), the complete run batch 12 dispatched.** It is still running.
  - win-tidy failed on four findings in `tests/foundation/platform/test_arguments.cpp`, this track's arguments test
    (batch 11): three `modernize-raw-string-literal` and one `bugprone-suspicious-missing-comma`. a40af1bf on main
    already fixes all four: the backslash paths are raw literals, and octal escapes replace the split literals.
    Every other comply, ceridc and crypto unit passed tidy under `-k 0`.
  - linux-gcc-shipping and linux-gcc-release failed at the LTO link of `crd-compression-tests`: GCC's
    `-Wnull-dereference` in `tests/foundation/compression/test_deflate.cpp:755-756`, OWN's compression tests (track B,
    6b818f86 and ff1c2b3b). Track B's own run reports it; this track does not touch that file.
  - win-vs, win-clang-cl, win-clang-cl-shipping, win-arm64-debug, both arm64 Linux lanes and both Repository checks
    jobs passed.
- **Run 41247045494 (67977fda).** Its nine Windows failures are the C2797 that d9be0cfd fixed. linux-gcc-asan never
  finished.
- Every push run since is cancelled by the next push before its jobs start.

## What changed

- **The register names the files.** `cmake/legal-register.json` gains `sanctions_lists`. It names each authority's
  official file:
  - the screening entry the file serves, the canonical URL and the file stores that URL may redirect to;
  - the importer format, the generation's maximum age and the record of the file read (day, publication, bytes,
    SHA-256, parties).

  Turkey's decisions are `pending`, with their reason. US hosts gain `treas.gov` and EU hosts `ec.europa.eu` (OFAC's
  list service and the FSF host). The `gb.sanctions` entry names the UK Sanctions List, which is the file the FCDO
  publishes; GB's rules are reviewed as generation 4.
- **`scripts/import-sanctions.py`** reads the files. Its three commands:
  - `fetch` downloads each file through register-review's client. Its redirect handler now takes the hosts a
    redirect may reach: OFAC answers with a signed S3 link and the UN with a signed Azure blob link, and every other
    host is refused. The command says whether the file is the one the register recorded.
  - `import` writes `<out>/<authority>.sanctions.toml`.
  - `record` records a reviewed read.

  Four readers cover OFAC XML, the EU's FSF 1.1, the UN's consolidated XML and the UK Sanctions List XML.
  - **Failing closed.** An unknown element, identifier type, country name, calendar or date shape refuses the import
    and names the party and the value. A document type declaration is refused by expat before the tree is built.
  - **Generations.** A generation is numbered by its publication instant. A second same-day publication of a
    day-stamped file counts up. A file already imported is left as it is, and an older publication is refused.
  - **Births.** The readers keep what each list says and never narrow it. Ranges are kept as intervals, and circa or
    APPROXIMATELY dates record no period. A Hijri date is the tabular day two days either side. An Islamic year
    without a day is both its lunar and its Solar Hijri year, because the EU prints Iranian births in the Solar Hijri
    year. The UK's `19yy` is the whole century.
  - **Identifiers.** Identifiers carry their issuing country.
- **The decoder and the screening** (crd-comply):
  - Births may be ISO 8601 intervals (`1960/1965`). An interval that ends before it starts is refused.
  - `listed` may be empty, because OFAC's files carry no designation day.
  - Identifiers follow `kind[.cc]:value` and agree only when the issuing countries are equal or one side names none.
    The old exact comparison would have let a Turkish passport number match a German one.
  - A party holds up to 1 024 identifiers. OFAC lists 134 digital currency addresses on ISIL Khorasan.
- **The guard.** `crd-check-registers` fails a list in any of these cases:
  - it names no screening entry, or its URL is off the official hosts of that entry's jurisdiction;
  - its format is unknown, a delivery host is malformed, or its maximum age is out of bounds;
  - its record of the file read is incomplete, published after it was read, or read after today.

  It also fails a screening entry that has neither a list nor a pending reason, or has both.
- **Tests:**
  - **Catch2.** The `[sanctions]` periods and identifier-grammar cases are extended. Two cases are new: an unknown
    listing day with intervals and the identifier bound, and agreement by issuing country.
  - **End to end.** The real-byte excerpts in `tests/foundation/comply/lists` hold whole entries cut from the five
    files. Only OFAC's `Record_Count` is changed, and `.gitattributes` keeps their CRLF bytes. They run through
    `crd-comply-sanctions-clean`, `crd-comply-sanctions-import` and `crd-comply-sanctions-imported`, a CTest fixture
    chain into the hidden `[.imported]` case. That case decodes, adopts and screens every format with its calendars,
    scripts and identifiers.
  - **Fuzz.** The fixed list carries an interval birth, an issuing country and an unknown listing day. The model
    restates the new agreement rule. The corpus is regenerated with five new seeds, 28 in all.
  - **Python.** 10 `SanctionsLists` cases: every format on the excerpts, every refusal, generations, calendars, fetch
    records, redirects and the guard.

## The first read

Fetched on 2026-11-01 from the official sources. The fetch was repeated on the track's machine, which said "the file
the register recorded" for all five. The bodies stay outside the repository.

| Authority | Official file | Published | Bytes | SHA-256 | Parties |
|---|---|---|---:|---|---:|
| `us.ofac.sdn` | [SDN.XML](https://sanctionslistservice.ofac.treas.gov/api/PublicationPreview/exports/SDN.XML) | 2026-10-28 | 29 271 441 | `0213f3a07b569dabf643e7ea8fd23d81327f83a2ace023a41ef311796accf1a8` | 19 484 |
| `us.ofac.cons` | [CONSOLIDATED.XML](https://sanctionslistservice.ofac.treas.gov/api/PublicationPreview/exports/CONSOLIDATED.XML) | 2026-10-19 | 1 078 267 | `06323d16735a0461d9dc03332ce1fed74d8525244df8b27a47ef12174b8c38f1` | 486 |
| `eu.fsf` | [FSF 1.1](https://webgate.ec.europa.eu/fsd/fsf/public/files/xmlFullSanctionsList_1_1/content?token=dG9rZW4tMjAxNw) | 2026-10-23T15:05:43Z | 25 899 600 | `636800c3e494b2ef8934f51f5e23c914a6208a383ac850e674b1b35a7d49d34f` | 6 269 |
| `un.sc` | [consolidated.xml](https://scsanctions.un.org/resources/xml/en/consolidated.xml) | 2026-10-31T23:00:07Z | 2 177 571 | `9104cf8495637693c0c05ec203fa45c8a50340f7127a0c5bd1616db92ea127cd` | 1 007 |
| `gb.fcdo` | [UK-Sanctions-List.xml](https://sanctionslist.fcdo.gov.uk/docs/UK-Sanctions-List.xml) | 2026-10-30 | 22 084 189 | `ed397aa9c89106c2615eee20ba249e364273c185c41245e7027659fce7d01086` | 6 420 |

What the files showed, each now handled and tested:
- The UN's old URL answers a HEAD request with 404 and a GET with a redirect to a signed blob.
- OFAC's idList holds 173 kinds, most of them descriptive: sanctions information, directive dates, gender and phone
  fragments.
- The UK's dates use `dd`, `mm`, `00` and `19yy` for unknown parts.
- The EU marks Iranian Solar Hijri years as ISLAMIC, and 4 of its birth elements are in that calendar. Its own
  conversion of 24/06/1402 is 19 April 1982, the middle of the importer's interval.
- The UN prints a listing day with a time-zone offset, and one birth only in a Hijri note.
- The EU prints withdrawn ISO codes (AN, CS, YU) and `00`.

Turkey's asset-freeze decisions are not read yet. MASAK's site is a web application with no list file at a stable
address, so the decisions are read from the Official Gazette and MASAK next.

## Verification

- **The track's machine** (linux-gcc-debug, c3c96db4 and 823bf1db):
  - `crd-comply-tests`, `crd-fuzz-comply-sanctions`, `crd-ceridc-tests` and `ceridc` built with no warning.
  - 158 scoped CTests passed, among them the import chain, the regenerated corpus (28 inputs, none rejected),
    `crd-check-registers`, `crd-generated-sources` and `crd-repository-tools`.
  - All five full lists imported. `ceridc screen` on the five generations (9.6 MB) gave these verdicts:
    - "Banco Nacional de Cuba" as an entity: Review, possible at 1000.
    - The vessel TIFON with `imo:7206512`: Blocked.
    - "Ekaterina Romanova" born 1980-08-15: Blocked (the UK's `19yy`).
    - An unlisted Turkish name: Clear.

    Each run of the debug build took about 3.5 s and 206 MB, most of it decoding.
- **Mutations: 21 Python and 8 C++, each caught.**
  - Python: the Crimea mapping, circa (OFAC, EU, UN), the same-day count, the issuing country, the Hijri epoch, the
    DTD refusal, the UK century, the OFAC attribute families, the record count, the already-imported check, the
    Solar Hijri year, the EU countries, the fetch digest, five guard rules and the redirect hosts.
  - C++: both halves of the agreement, the interval order and end, `listed` required, the identifier bound, the
    lower-case country and the agreement call (the last caught by the build).
- **Validators.** `check-registers.py`, `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py` and
  `check-generated.py` pass.
  - One Allman run hit the host's known misexecution, an impossible `TypeError`, and passed when run again.
  - The whole `test-repository-tools.py` passes except `test_generated_target_folders`, which needs Ninja and fails
    the same way without this change on this PC.

## Next

1. COMPLY.0's instrument reading, host by host:
   - the remaining mevzuat.gov.tr laws (Law No. 5651 after its full re-research), the CELLAR and UK acts, COPPA,
     Section 508, the CVAA, OFAC's chapter V and the Wassenaar Category 5 Part 2;
   - readers for the platform pages and corrections to the moved primaries;
   - the secondary instruments.
2. COMPLY.1: Turkey's asset-freeze decisions and their importer. Then the embargoed destinations, authored per
   authority as generations from the regulations read, since none of the designation files carries them.
3. Hosted evidence: the first complete run that includes this batch (the win-tidy lane for `test_imported.cpp`, MSVC
   for the new decoder paths).
