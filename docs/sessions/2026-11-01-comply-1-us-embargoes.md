# COMPLY.1: 31 CFR chapter V read and the US embargoed destinations authored

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.1](../ROADMAP.md#slice-comply.1) and [COMPLY.0](../ROADMAP.md#slice-comply.0) (track
C, batch 21; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening). The text is the eCFR document batch 19 pinned
([report](2026-11-01-legal-register-new-instruments.md)); nothing new was downloaded.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The chapter read here is part of the first read of the official legal texts
that decision approved; its body stays outside the repository (`~/.cerid/downloads/legal-2026-11-01-c/`), pinned by
URL, size and SHA-256 in the report.

## CI first

No failure comes from a track C change.

- **Run 41262267004 (8a243a34)**, the only push run since batch 20 that started: Repository checks
  (windows-2025-vs2026) failed on `crd-ci-orchestrator-e2e` alone (`test_1_webhook_build_runs_end_to_end`, its plan job
  still `scheduled`), the CI orchestrator's own test that batch 18 named for its owner (CI-1). Its win-arm64-debug pass
  proves nothing about the `checksum.cpp` C4324 batch 20 expected to fail the ARM64 lane: 8a243a34 predates 8332bf9f,
  which brought it. Track A's 921e19cc ("size the checksum check stream to whole lines") is the repair, and de5e10b0,
  the commit of the complete run batch 20 dispatched, holds it; that run's ARM64 build is the first to prove it.
- **Run 41274380605 (de5e10b0)**, that complete run, and 41269105413 (b0b1b245) were still queued at 04:49 UTC, with
  every push run since 41262267004 cancelled by a newer push.

<a id="reading"></a>
## What was read

**31 CFR chapter V** (Office of Foreign Assets Control), the eCFR versioner's text of 2026-10-15 (the chapter's latest
amendment day). The versioner returns the whole of Title 31 for a chapter, so the reader pins the chapter's own subtree
(`ecfr_record`); the 9 720 889-byte file's other chapters are not what was read. The question asked of every programme
part was whether it prohibits dealings with a country or region as such (comprehensive), with defined sectors or a
government that the lists do not fully name (sectoral), or only with the parties it designates (no destination).

- **Comprehensive.** Cuba: §515.201 prohibits every transaction in property in which Cuba or a Cuban national (§515.302:
  a citizen, a domiciliary, an organisation of or controlled from Cuba) has any interest. Iran: §560.204 prohibits the
  exportation, reexportation, sale or supply of any goods, technology or services to Iran or its Government from the
  United States or by a United States person, wherever located, with §560.201 and §560.206; §560.304 and §560.211
  block the Government of Iran whether or not it is listed. North Korea: §510.206 prohibits exporting any goods,
  services or technology to North Korea from the United States or by a U.S. person, with §510.205 and §510.209. The
  Crimea region of Ukraine: §589.207 prohibits exporting goods, services or technology to it and §589.206 new
  investment in it; §589.306 makes the region its land territory and the maritime areas claimed from it, which is
  ISO 3166-2 UA-43 (the Autonomous Republic of Crimea) and UA-40 (Sevastopol).
- **Sectoral.** The Russian Federation: Directives 1 to 4 (§589.202 to §589.205) restrict new debt and equity and the
  goods, services and technology for certain oil projects in the financial services, energy and defense sectors of the
  Russian Federation economy. Venezuela: §591.201 adopts E.O. 13884, under which the Government of Venezuela is blocked,
  and §591.406(b) says that Government includes blocked persons the SDN List does not name; a Venezuelan counterparty
  therefore needs review even when no listed party matches.
- **Incorporated, not in the chapter's text.** §589.201(e) prohibits every transaction that an Executive order issued
  after 19 December 2014 under E.O. 13660's emergency prohibits, and §587.201(b) every transaction that a further
  Executive order under E.O. 14024's emergency prohibits; the chapter names neither the regions nor the services those
  orders reach. §596.201 prohibits financial transactions with the government of a country the State Department
  designates as supporting terrorism, and leaves the designations to the State Department. These are the generation's
  `open` reading: the Executive orders and determinations from the Federal Register and the designations from the
  State Department.
- **Party programmes only.** Burma (525), Belarus (548: its Directive 1 names two bodies; the sector definitions are
  grounds for designation), Lebanon, the Western Balkans, Nicaragua, Hong Kong, Syria (569, adopting E.O. 13894, whose
  persons are listed) and the thematic programmes block the parties they designate, which the SDN and consolidated
  lists carry. Chapter V has no Part 542: no comprehensive Syria programme is in force in the text read.
- **General licences.** §515.578 (Cuba), §560.540 (Iran) and §589.517 (the Crimea region) authorise exporting services
  incident to communications over the internet, e-gaming, collaboration and video-conferencing platforms among them,
  and the software for them, on conditions. A comprehensive destination still screens Blocked: whether a product may
  rely on a licence, and on which conditions, is the store's decision, asked of counsel (below).
- **Duties the screening creates** (Part 501): a full record of each transaction kept for **10 years** (§501.601, since
  the 2024 amendment); blocked property reported within 10 business days of blocking and yearly by 30 September for
  what is held on 30 June (§501.603); a rejected transaction, a sale of goods or services included, reported within
  10 business days (§501.604); civil penalties under Part 505. They bind the publisher as far as its dealings are
  subject to U.S. jurisdiction (a U.S. person, the United States' financial system, items exported from it), which is
  how each prohibition above is drawn.

`read` recorded `us.sanctions` from batch 19's fetch, with this section as its verified source; its article, obligation
and note now say the above, and the US rule generation was reviewed.

<a id="embargoes"></a>
## The US embargoed destinations

The design said each authority's embargoes are authored generations of their own. They now are:

- **In the register.** `sanctions_lists.embargoes.generations` holds, per authority, the screening entry it serves,
  a title, the maximum age, the destinations (region, programme, `comprehensive` or `sectoral`, and the provision it
  rests on) and `open`, what the reading still owes. `reasons` says why a screening entry has none yet. The entry must
  have been read from its primary source.
- **Written by the importer.** `import-sanctions.py import` writes `<out>/<authority>.sanctions.toml` beside the
  imported lists, with no fetch: `published` is the day the text was read (the maximum age runs from the reading),
  `source` the entry's primary and `source_sha256` the digest of the reading, the pinned text's digest together with
  the destinations, so a corrected reading of the same text is the next generation. One rule set
  (`embargo_errors`) refuses a malformed generation in the importer and in `check-registers.py`.
- **`us.ofac.embargo`** (generation 20261101000000): CU, IR and KP (31 CFR 515, 560 and 510) and UA-43 and UA-40
  (31 CFR 589, the Crimea region) comprehensive; RU (31 CFR 589, Directives 1 to 4) and VE (31 CFR 591) sectoral. A
  maximum age of 92 days ends on 2027-02-01, after the entry's review on 2027-01-25: chapter V is amended every few
  weeks and comprehensive programmes change by Executive order, so a reading a quarter old is stale and fails closed
  to Unavailable.
- **The other authorities.** `eu.restrictive-measures`, `gb.sanctions` and `int.un-sanctions` have no embargo
  generation until their regimes' regulations and resolutions are read (the EU entry still cites a map, not a
  regulation); Laws No. 6415 and 7262 freeze listed parties' assets and embargo no destination.

## Counsel

`us.sanctions` now needs counsel, asked in the open review `counsel.2026-11-01`: which general licences may Cerid's
e-gaming and collaboration products rely on for Cuba, Iran and the Crimea region, and on which conditions. As the user
decided, the question never holds a row.

## Verification

- **Guards.** `check-registers.py`, `gen-comply-rules.py --check` (the US generation reviewed as generation 5),
  `check-generated.py`, `check-master-plan.py`, `check-repository.py` and `check-allman-braces.py` pass.
- **Python.** `SanctionsLists` has five new cases: an authored embargo on a read screening entry passes; every rule
  refuses (an unread or unverified entry, a non-screening entry, unknown keys, the title, the age bound, an empty
  `open`, no destinations, six malformed or unknown regions, the measure, a missing provision, an over-long value, a
  region twice under one programme while two programmes may share it, an authority clash with an official file);
  every screening entry has a generation or a reason, never both; the importer dates the generation by its reading,
  leaves the same reading as it is, takes the next generation for a corrected reading or a new text the same day and
  refuses an older reading; the real register writes `us.ofac.embargo` with its seven measures. Ten source mutations
  of the rules, the reading digest, the publication day and the guard were each caught (one was missed first, an
  unverified source, and its case was added).
- **C++.** On the track's machine (linux-gcc-debug at 3a21b5cd, the work-in-progress commit of the same tree;
  05:09-05:14 UTC) `crd-comply-tests`, `crd-fuzz-comply-sanctions`, `crd-ceridc-tests` and `ceridc` built with no
  warning and the 158 scoped CTests passed;
  `crd-comply-sanctions-imported` ran both hidden cases (206 assertions): the embargo decodes and adopts as generation
  20261101000000 with seven destinations, CU, IR, KP, UA-43, UA-40 and a place inside Cuba block, UA needs review with
  both Crimean regions possible, RU and VE need review as sectoral, SY, UA-30 and TR are clear, and on 2027-02-02 the
  reading is stale, still blocks Cuba and keeps Turkey from clear. `ceridc screen --authority us.ofac.embargo` on the
  importer's output answered CU and UA-43 blocked, UA and RU review and SY clear. The 37 `SanctionsLists` and
  `RegisterReview` Python cases pass on Linux too.

## Next

1. COMPLY.1: read the Executive orders and determinations §589.201(e) and §587.201(b) incorporate (E.O. 14065's regions
   of Ukraine, E.O. 14066, 14068 and 14071 and the services determinations under them) from the Federal Register, and
   the State Department's designations under §596.201, then the next `us.ofac.embargo` generation; the EU, UK and UN
   regimes' regulations and resolutions for their own generations.
2. COMPLY.0: the other eCFR texts of batch 19's fetch (COPPA, Section 508, the CVAA), then the CELLAR and UK acts.
