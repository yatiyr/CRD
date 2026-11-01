# COMPLY.1: the EU regime index read by the review, and Iraq's claims regulation

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.1](../ROADMAP.md#slice-comply.1) and [COMPLY.0](../ROADMAP.md#slice-comply.0) (track
C, batch 25; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts). The register's fetch and its comparison are the
[report](2026-11-01-legal-register-eu-regime-index.md). The previous batch is the
[EU regimes](2026-11-01-comply-1-eu-arms-and-repression-regimes.md), whose first next item this batch closes.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The regime index and Regulation (EEC) No 3541/92 read here are part of the
first read of the official legal texts that decision approved. Their bodies stay outside the repository
(`~/.cerid/downloads/legal-2026-11-01-h/`), pinned below and in the report by size and SHA-256.

## CI first

No failure comes from a track C change.

- **Complete run 41274380605 (de5e10b0)** was still running at 07:50 UTC: 8 of its 30 jobs passed, 3 failed and the
  rest were running or queued.
  - Passed: win-vs; win-arm64-debug, the first ARM64 Windows build with track A's `checksum.cpp` repair; the three
    linux-arm64 lanes that finished; linux-gcc-asan's first shard; and the Windows Repository checks job.
  - win-clang-cl and win-clang-cl-shipping failed at the link of `crd-perf-tests`, as batch 24 recorded. lld-link
    reports `__udivti3` and `__divti3`, referenced from `tests/foundation/perf/test_instruction_access.cpp`. That file
    is HW-RESILIENCE.2's (track A) and is unchanged on main at d4f461ae, so the fix stays track A's.
  - linux-gcc-release failed at the LTO link of `crd-hesap-stats-tests`, on GCC `-Wmaybe-uninitialized` in
    `tests/numerics/hesap-stats/test_multivariate.cpp:125`. 76330344 on main already fixes it (not a track C file).
- **Push run 41284892084 (c43cede7)**, this track's last batch, was pending; every push run before it since 41273181833
  was cancelled by a newer push before it started.

## The regime index in the plan

Batch 24 enumerated the EU's regimes from the EU Sanctions Map's regime index read once by hand, while
`eu.restrictive-measures` cited the map's page, a script shell whose digest never moves. A new regime that restricts a
supply to a place could therefore appear without any scan noticing it. The index is now the entry's document, read and
judged by `scripts/register-review.py` like any other instrument:

- **Primary.** `eu.restrictive-measures` cites `https://www.sanctionsmap.eu/api/v1/regime`, the data the map shows. The
  register's guard refuses a read instrument whose primary is a home page, and the index is the document read.
- **Reader** (`sanctions-map`). It accepts only the index's shape: `meta.version` "1", regimes with distinct integer
  ids, a two-letter country code or none (a thematic regime), titled measures and legal acts with a type, number and
  URL. A regime's basic Council regulations are its "EC Council Regulation" and "EU Council Regulation" acts whose
  number is not an amendment "not yet included" in the consolidated text. Each must link its CELEX number
  (`CELEX:0<year>R<number>-<day>`, `CELEX:3...`) or its ELI (`data.europa.eu/eli/reg/<year>/<number>/...`), else the
  index is unreadable. Implementing regulations amend the annexes of listed persons.
- **The text read.** The digest is that of the regimes' structure alone: each regime's id, country, measure titles and
  basic regulations, canonically serialised. Notes, guidance, icons, measure descriptions, amendment times and a
  link's `qid` change without changing which act carries which measure. The amended regulations themselves are read
  and judged in their own entries.
- **Version.** `<n> regimes, <m> attached to a place`. The index's own `amendment` times were rejected as the version:
  they move whenever any regime's list of persons is amended, so every scan would report the umbrella amended. A
  regime added or turned into a place regime moves the version, and any other structural change moves the digest.
- **Place regimes.** A regime attaches to a place when it has a country and a measure other than an asset freeze, an
  admission ban or a claims ban. Those three attach to the persons and contracts that the EU financial sanctions file
  (`eu.fsf`) and the regulations name. That is batch 24's classification, now computed.
- **The judgement.** Besides version and digest, each scan checks the index against the register:
  - every basic regulation of a place regime must be an entry (it is then read and judged like any other instrument,
    and one restricting no supply is read without a destination);
  - every Council regulation entry without a sunset must still be listed by some regime, else the review owes a
    reading of its repeal and the entry's sunset.
  Both appear under "Read in the consolidated text". A regime with no regulation (China's 1989 declaration) owes
  nothing: the Member States apply it.
- **CELLAR.** The reader of an act's XHTML now matches a name only at number boundaries and also accepts the
  two-digit year of an act before 1999 (`No 3541/92`), the form the Official Journal printed then.

## What the index found

On the index of 2026-11-01 (55 regimes, 26 attached to a place), every basic regulation of the 26 place regimes was
already an entry and read, except one: Regulation (EEC) No 3541/92, the second regulation of the Iraq regime beside
(EC) No 1210/2003. No Council regulation entry is missing from the index.

<a id="reading"></a>
## What was read

The register was refreshed with the new entry `eu.reg-3541-92.iraq-claims` (`legal-2026-11-01-h`, 172 of 185 sources
read; the 13 not read are the hosts that refused before). The fetch was compared and applied, and `read` recorded both
entries:

| Entry | Document | Version | Bytes | SHA-256 of the body | SHA-256 of the text read |
|---|---|---|---:|---|---|
| `eu.restrictive-measures` | the regime index | 55 regimes, 26 attached to a place | 392161 | `9baf902c0abb2c1a0f0aac720ec1a5137126cf1456871b45895d9a4dcefe912f` | `9e7d9d029d36d9972ed7453c180765c8dd8ad682879b7f930aed99378b09398f` |
| `eu.reg-3541-92.iraq-claims` | (EEC) No 3541/92, CELLAR's English XHTML of the act as published | 31992R3541 | 22734 | `d128cd59082da7e210060eafc05bce7a13cce31aec70adb532b937eb61f3f9e0` | the same |

The index's body is byte-identical to the one batch 24 read by hand. CELLAR lists no consolidated version of
3541/92, and the index lists no amendment: the act stands as published in OJ L 361 of 10 December 1992.

**Regulation (EEC) No 3541/92.** Article 2 prohibits satisfying, or taking any step to satisfy, a claim made by, on
behalf of or through a person or body in Iraq (the State, a person in or resident in Iraq, a body seated there or
controlled from there) under a contract or transaction whose performance the measures under UN Security Council
Resolution 661 (1990) affected. The prohibition applies within the Community and to every national and body of a
Member State. Article 3 excepts claims proven unaffected and some insurance, blocked-account, employment, export and
loan claims, and Article 4 puts the proof on whoever seeks enforcement. Under this classification it restricts no
supply to Iraq: it is a claims ban, so the EU embargo names no new Iraqi destination, and `eu.council.embargo` is
unchanged (its reading holds only the texts its destinations rest on). The store and payment flows obey it through the
entry's obligation.

## Verification

- **Python.**
  - `RegisterReview` and `SanctionsLists`: 44 cases pass, two of them new, and the 80 register cases with
    `LegalRegister`, `ComplyRules`, `Registers`, `CryptoInventory` and `RegisterIds` pass.
    - The sanctions-map case plans the index with its `Accept` header and judges a five-regime fixture: Iraq's second
      regulation is reported missing and Libya's entry is reported as listed by no regime. Nothing is owed for
      Burundi's freezes and claims, the thematic regime's arms export to listed persons, China's regime without a
      regulation, the implementing act, the unconsolidated amendment, the ended entry or the dual-use regulation.
      `read` records the version and digest, and the register's guard passes. The digest holds under reworded notes,
      amendment times, icons, descriptions, a changed `qid` and reordered regimes and measures. Burundi gaining an
      arms embargo reads as a new version, a regulation added to China's regime as a new digest, and an index of
      another version as one source not read.
    - The reader case refuses eleven other shapes.
    - The resolver and CELLAR cases cover the index's URL, the home page and near spellings left as pages, the
      two-digit year, and names inside longer numbers.
    - The EU embargo case now finds the umbrella read at its index's version, with the reading and generation
      unchanged, and 3541/92 read without a destination.
  - Ten mutations of `register-review.py` were each caught: the claims ban counted as a place measure, unconsolidated
    amendments or implementing regulations counted as basic, thematic regimes counted, ended entries judged, the
    two-digit year or the number boundaries dropped, the whole body digested, any map URL resolved, and the regime
    judgement skipped.
- **C++.** On the track's machine (07:46-07:50 UTC, linux-gcc-debug tree at a89ad9cb), `crd-comply-tests`,
  `crd-fuzz-comply-sanctions`, `ceridc` and `crd-ceridc-tests` built with no warning. The 61 scoped CTests (comply,
  ceridc and the `crd-no-` and `crd-check` guards, without `crd-check-isa-isolation` as in batch 24) passed, among them
  the committed generations decoding EU generation 9. The four `[.imported]` cases passed (600 assertions), and
  `[sanctions]` ran 4 594 assertions in 18 cases. `ceridc screen` on the written lists answered as in batch 24, at the
  same `eu.council.embargo` generation 20261101000000: IR and KP blocked; RU, VE, MM, HT and CF review; IQ, YE and TR
  clear.
- **Guards.** `check-registers.py`, `gen-comply-rules.py --check` and `check-generated.py`, with the EU rule file
  refreshed, plus `check-master-plan.py`, `check-repository.py` and `check-allman-braces.py`.

## Next

1. **Amendments the consolidated texts lack.** The index lists, per regime, the amending Council regulations "not yet
   included" in the consolidated text (corrigenda to Regulations (EU) 2026/506 and 2026/513 under the Russia and
   Belarus regimes on 2026-11-01). A place regime's entry is read in CELLAR's latest consolidated version, so such an
   amendment is owed a reading of its own until CELLAR consolidates it. The judgement should report each one dated
   after the version read.
2. **COMPLY.1.**
   - Read the UK's regime regulations through legislation.gov.uk's `uksi/` reader (`gb.sanctions` is SAMLA 2018) and
     the UN Security Council resolutions, each for its own embargo generation.
   - Then Turkey's asset-freeze decisions and their importer, the Wassenaar Category 5 Part 2 and Turkey's dual-use
     communiqué.
3. **COMPLY.0.** Read the other eCFR texts of batch 19's fetch (COPPA, Section 508, the CVAA), then the CELLAR and UK
   acts.
