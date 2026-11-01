# COMPLY.1: the Federal Register read and the US embargo completed

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.1](../ROADMAP.md#slice-comply.1) and [COMPLY.0](../ROADMAP.md#slice-comply.0) (track
C, batch 22; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening) and [official texts](../design/registers.md#official-texts).
The fetch is the register's own ([report](2026-11-01-legal-register-federal-register.md)); the documents read beside
it are pinned [below](#pins).

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The Federal Register, OFAC and State Department documents read here are
part of the first read of the official legal texts that decision approved. Their bodies stay outside the repository
(`~/.cerid/downloads/legal-2026-11-01-d/` for the register's fetch, `~/.cerid/downloads/probe-2026-11-01c/` for the
rest), pinned by URL, size and SHA-256.

## CI first

No failure comes from a track C change.

- **Run 41269105413 (b0b1b245), the complete run batch 17 dispatched.** Seven Windows lanes (win-debug,
  win-debug-scalar, win-asan shard 2, win-vs, win-clang-cl, win-clang-cl-shipping and, separately, win-arm64-debug)
  failed to build. Six failed on `engine/foundation/platform/src/filesystem.cpp:308-309`, where `windows.h`'s `min` and
  `max` macros broke `std::numeric_limits<i64>::max()` (MSVC C4003, clang-cl "too few arguments"). Those lines came from
  track B's aa91b382 (the bundle files moved onto crd-platform), and 9723ac75 on main already fixes them.
  win-arm64-debug failed on the `checksum.cpp` C4324 that track A's 921e19cc fixes. Both fixes are in de5e10b0, the
  commit of complete run 41274380605, which was still queued at 05:20 UTC.
- **Run 41262267004 (8a243a34).** linux-gcc-release failed at the LTO link of track B's `test_deflate.cpp`
  (`-Wnull-dereference`), which 95ec6965 fixes. The failure on Repository checks (windows-2025-vs2026) is
  `crd-ci-orchestrator-e2e`, the CI orchestrator's own test (CI-1), as batch 18 reported.
- **Run 41254049879 (31404ec4).** Nothing new: win-tidy (a40af1bf), the LTO links (95ec6965), win-relwithdebinfo (the
  orchestrator test) and win-asan shard 1 (track A's perf test) were all recorded before. One job is still queued.
- No push run since b9fb75d8 has started; 41276276561 (b9fb75d8) was pending.

<a id="reader"></a>
## The Federal Register reader

Executive orders and OFAC's determinations are published in the Federal Register and never consolidated: an
amendment is a document of its own. `scripts/register-review.py` now resolves `https://www.federalregister.gov/d/<document
number>` to two sources:

- **Index: the API record** (`api/v1/documents/<number>.json`). It must be the document's own record and name the
  document's raw text on its publication day. Its version is `<publication day> (<citation>) as published`, followed by
  each change the record names: an Executive order's "Amended by", "Revoked by", "Superseded by" or "Modified by"
  notes, and each correcting document's number. So when a later order amends a read order, or a correction is
  published, the next scan reads the result as a new version. EO 14068's record, for example, reads `2022-03-15
  (87 FR 14381) as published; Amended by: EO 14114, December 12, 2023`.
- **Primary: the raw text** (`documents/full_text/text/<year>/<month>/<day>/<number>.txt`). It must name the document
  (`[FR Doc No: <number>]`), and it is pinned by its digest.

An index reader may now return the part of the URL and the version separately, because the version can say more than
the path. New Executive orders and new determinations under an order already read are documents with no record yet.
The review's reading by hand therefore now includes the Federal Register.

`check-registers.py` accepts two new instrument types, `executive-order` and `determination`. Two Python cases cover
the resolver, both readers, every refusal, the fetch, `compare` and `read` end to end, and the amendment showing as a
new version.

<a id="reading"></a>
## What was read

The register's fetch of 2026-11-01 (`legal-2026-11-01-d`, 118 of 131 sources) was compared and applied
([report](2026-11-01-legal-register-federal-register.md)), and `read` recorded four entries:
`us.eo-14065.covered-regions`, `us.eo-14071.services`, `us.eo-14071.it-services` and `us.sanctions` (chapter V's text
of 2026-10-15, the same digest as batch 21's reading, re-read for Part 596).

- **E.O. 14065** (87 FR 10293, 21 February 2022; no amendment in its record). It expands E.O. 13660's emergency, so
  31 CFR 589.201(e) applies it. Section 1(a) prohibits:
  - new investment in "the so-called Donetsk People's Republic (DNR) or Luhansk People's Republic (LNR) regions of
    Ukraine or such other regions of Ukraine as may be determined by the Secretary of the Treasury" (the Covered
    Regions);
  - importing any goods, services or technology from them;
  - exporting, reexporting, selling or supplying any goods, services or technology to them, from the United States or
    by a United States person.

  The order names the regions after the so-called republics and draws no boundary. No determination of another region
  exists: a full-text search of the Federal Register for "Covered Regions", "so-called Donetsk", "Kherson" and
  "Zaporizhzhia", and OFAC's Ukraine-/Russia-related programme page, both on 2026-11-01, found only the order, its
  general licences, the Commerce rules and the continuations of the emergency. The latest continuation is the Notice
  of 18 February 2026 (91 FR 8355), so the order is in force.
- **E.O. 14071** (87 FR 20999, 6 April 2022; no amendment in its record). It takes steps under E.O. 14024's emergency,
  so 31 CFR 587.201(b) applies it. Section 1(a) prohibits:
  - new investment in the Russian Federation;
  - exporting "any category of services as may be determined by the Secretary of the Treasury" to any person located
    in the Russian Federation;
  - approving, financing or facilitating a foreign person's doing either.

  Every determination under Section 1(a)(ii) was read:
  - the Federal Register publications 87 FR 32307 (accounting, trust and corporate formation, and management
    consulting), 87 FR 62006 (quantum computing), 87 FR 76931 and 87 FR 79255 (trading, financing, shipping,
    insurance, flagging and customs brokering for the maritime transport of crude oil above the price cap, and that
    cap), 88 FR 36645 (architecture and engineering), 89 FR 48324 (Covered Metals Acquisition Services) and 89 FR 58286
    (IT and software services, below);
  - the determinations OFAC published only on its website: the petroleum products' covered services and price caps,
    and petroleum services (10 January 2025).
- **The IT and software services determination** (89 FR 58286; signed 12 June 2024, in force 12 September 2024). From
  the United States or as a United States person, no "(1) Information technology (IT) consultancy and design services"
  and no "(2) IT support services and cloud-based services" for enterprise management software or design and
  manufacturing software may be supplied to any person located in the Russian Federation. Three services are
  excluded:
  - a service to an entity there that a United States person owns or controls;
  - a service for winding down or divesting an entity there that no Russian person owns or controls;
  - a service for software whose export to the Russian Federation the Department of Commerce licenses or otherwise
    authorises (or would, were the software subject to the EAR).

  This is the one Russia measure that reaches Cerid's own services: it is now its own entry with a counsel question.
- **E.O. 14066, 14068 and 14114** (87 FR 13625, 87 FR 14381 and 88 FR 89271), read in full:
  - they bar importing Russian energy, fish, seafood, alcohol, diamonds and the products later determined;
  - they bar new investment in the energy sector and in determined sectors;
  - they bar exporting luxury goods and dollar banknotes to persons in the Russian Federation;
  - E.O. 14114 amends E.O. 14024 to reach foreign financial institutions serving Russia's military-industrial base.

  No Cerid flow imports, invests in or exports any of these, so they add no destination. They are cited in
  `us.eo-14071.services`' note and not registered as obligations.
- **Part 596 and the designations.** Part 596 is read in the pinned chapter V text:
  - §596.201 prohibits financial transactions with the government of a country designated as supporting terrorism;
  - §596.503 leaves a Terrorism List Government that another part governs to that part;
  - §596.504 authorises every other financial transaction with one, except a donation to a United States person and
    a transfer the person knows or has reason to believe risks furthering terrorist acts in the United States.

  Part 596 therefore embargoes no destination. Whatever the designations are, it leaves one duty: accept no donation
  from such a government, which `us.sanctions`' obligation now states. The designations were read from the State
  Department's notices in the Federal Register, because state.gov answers the fetcher 403:
  - Cuba: 86 FR 6731, 12 January 2021;
  - North Korea: 82 FR 56100, 17 November 2017;
  - the rescission of Syria's designation: 91 FR 55965, effective 24 August 2026;
  - Iran's designation of 1984 predates the Federal Register's online text and was not read.

  OFAC amended Part 596 for Syria's rescission on 25 September 2026 (91 FR 60825; only its Federal Register abstract
  was read). The 2026-10-15 text that was read reserves §§596.505-596.506.

<a id="embargo"></a>
## The US embargo, second reading

- **Each destination names its entry.** A destination now carries `entry`, the register entry its provision is read
  in. That is the screening entry, or an Executive order or determination that the entry's regulations incorporate,
  and it must itself be read from its primary source. The generation's digest covers every text it rests on, and its
  `published` day is the latest reading. One rule set still refuses a malformed generation in the importer and the
  guard.
- **The measure `partial`** is new: the embargo covers part of a region whose boundary no subdivision draws, so a
  match needs review. crd-comply's `Measure::Partial` decodes it, and the verdict rules are unchanged: only
  comprehensive blocks.
- **`us.ofac.embargo`** (generation 20261101000000 on an emptied directory; 10 destinations; nothing `open`):
  - CU, IR, KP, UA-43 and UA-40: comprehensive (chapter V);
  - UA-14 and UA-09 under E.O. 14065: partial. These are the Donetsk and Luhansk oblasts (ISO 3166-2:UA), which hold
    the Covered Regions;
  - RU twice, both sectoral: chapter V's Directives 1 to 4, and E.O. 14071's determined services;
  - VE: sectoral.
- **Rule generation.** The three new entries and `us.sanctions`' changed obligation are the US rule generation 6
  (reviewed 2026-11-01). The new entries' active check is the hidden `[.imported]` case that screens their
  destinations.

## Counsel

Two questions join the open review `counsel.2026-11-01`, which now holds 63:

- **E.O. 14065.** Must the store treat the whole of both oblasts as the Covered Regions? If not, how does it
  establish that a person or payment there lies outside them?
- **The IT and software services determination.** Are any of Cerid's hosted services "IT support services or
  cloud-based services" for design and manufacturing or enterprise management software, or "IT consultancy and
  design services"? These include the store, cloud builds, the collaboration and session servers, crash collection
  and the editor's online features. May Cerid rely on the third exclusion for mass-market encryption software
  (5D992.c) that the EAR authorises?

As the user decided, neither question holds a row.

## Verification

- **Python.** `SanctionsLists` and `RegisterReview` pass: 39 cases, two of them new and four extended.
  - The new cases cover the Federal Register resolver, its readers and the end-to-end read.
  - The extended cases cover a destination read in an incorporated order, every rule on a destination's `entry`, the
    `partial` measure, the published day and digest across texts, and the real register's ten destinations.
  - Ten source mutations (the destination entry rule, the published day, the digest's texts, the measure list, and
    six in the record reader, the raw-text reader and the fetch's split version) were each caught.
- **Guards.** These pass: `check-registers.py`, `gen-comply-rules.py --check` (US generation 6), `check-generated.py`
  (the US rule file refreshed), `check-master-plan.py`, `check-repository.py` and `check-allman-braces.py`.
- **C++.** The run was on the track's machine (linux-gcc-debug at a401c808, the work-in-progress commit of this tree;
  05:43-05:48 UTC).
  - `crd-comply-tests`, `crd-fuzz-comply-sanctions`, `crd-ceridc-tests` and `ceridc` built with no warning, and the
    158 scoped CTests passed.
  - `[sanctions]` ran 4 714 assertions in 18 cases, including the new partial-embargo case (14 assertions).
  - `crd-comply-sanctions-imported` passed with the new expectations: UA-14 and UA-09 review, UA four possible hits,
    RU two matches.
  - `ceridc screen --authority us.ofac.embargo` on the importer's output answered: CU and UA-43 blocked; UA-14 and
    UA-09 review (partial, E.O. 14065); UA review with four possible hits; RU review with two sectoral matches; SY and
    UA-30 clear.
  - The final commit reflows two test lines and one comment to clang-format's layout, with no change in meaning.

<a id="pins"></a>
## Documents read beside the register's fetch

These were retrieved on 2026-11-01 into `~/.cerid/downloads/probe-2026-11-01c/`. OFAC's programme pages were read
for their lists of determinations: Ukraine-/Russia-related, 69 852 bytes, `d6172b2bff2c2633de3e69e20e9102ed685114d8ee9e6a56ad182624c84c2b59`;
Russian harmful foreign activities, 137 204 bytes, `6c27ee43718163d50d30554497d3a023100753403c9f79b885ab309d539fb787`.

| Document | Citation | Source | Bytes | SHA-256 |
|---|---|---|---:|---|
| E.O. 14066 | 87 FR 13625 | [2022-05232.txt](https://www.federalregister.gov/documents/full_text/text/2022/03/10/2022-05232.txt) | 6876 | `38fc9ca40168ef7f404b4a4be8e6b514a6f5a0f4fb503c32924e7e32c8453122` |
| E.O. 14068 | 87 FR 14381 | [2022-05554.txt](https://www.federalregister.gov/documents/full_text/text/2022/03/15/2022-05554.txt) | 7944 | `835c2474e63015ec3f63eb19dbd6b79e4378b34efbaff865504d186a0bfb27d5` |
| E.O. 14114 | 88 FR 89271 | [2023-28662.txt](https://www.federalregister.gov/documents/full_text/text/2023/12/26/2023-28662.txt) | 15482 | `6c26bd955d8bae3a8416bee3c1710668c597344f73915f01be9b030f38675e15` |
| Determinations (accounting, consulting) | 87 FR 32307 | [2022-11606.txt](https://www.federalregister.gov/documents/full_text/text/2022/05/31/2022-11606.txt) | 9844 | `8eb0abd289b694082b2674d7382468581ccc59271dc6fbfc4495ae19c847b05f` |
| Determinations (quantum computing) | 87 FR 62006 | [2022-22162.txt](https://www.federalregister.gov/documents/full_text/text/2022/10/13/2022-22162.txt) | 7748 | `7d7d985c7b8dfd2566b6cbd2a2a3c636908179e512a49141bd920d550785a760` |
| Determination (crude oil services) | 87 FR 76931 | [2022-27235.txt](https://www.federalregister.gov/documents/full_text/text/2022/12/16/2022-27235.txt) | 10085 | `634c21ef438e9c342241260c081a6d04236ac8c3a6b37737347e8a669b408f74` |
| Determination (crude oil price cap) | 87 FR 79255 | [2022-28153.txt](https://www.federalregister.gov/documents/full_text/text/2022/12/27/2022-28153.txt) | 3207 | `31fd66ef3056a75bc0dbeb723baf1f634e844945e9336c2a66f47ffe5cc295b9` |
| Determination (E.O. 14068 products) | 87 FR 56590 | [2022-20030.txt](https://www.federalregister.gov/documents/full_text/text/2022/09/15/2022-20030.txt) | 4201 | `7320e64663e1bcbc01eb3aca368bf2ae5320aca0683be2229fe271bbdff618bc` |
| Determinations (architecture, engineering) | 88 FR 36645 | [2023-11977.txt](https://www.federalregister.gov/documents/full_text/text/2023/06/05/2023-11977.txt) | 7372 | `7b6cb06cf5293d74db3e7590f6a027807da4c6287e9f401d43f6dcf97e1c0f6f` |
| Determinations (diamonds) | 89 FR 16450 | [2024-04855.txt](https://www.federalregister.gov/documents/full_text/text/2024/03/07/2024-04855.txt) | 10601 | `7fe930bdfc5afc3397635eb084c688948714e44c71d2ff424b60eaf92cada751` |
| Determinations (metals) | 89 FR 48324 | [2024-12372.txt](https://www.federalregister.gov/documents/full_text/text/2024/06/06/2024-12372.txt) | 8736 | `91861e0ebc4119610aaef1eb418d59c005fea9ff3f17f17c4b6a861d55ba7b36` |
| Determinations (E.O. 14024 items, seafood) | 89 FR 78245 | [2024-21798.txt](https://www.federalregister.gov/documents/full_text/text/2024/09/25/2024-21798.txt) | 15254 | `112d6d0d74cb71c5044c10ce4fc74ac1ea44b3c28d3171c4f22cd42d968b41b0` |
| Cuba's designation | 86 FR 6731 | [2021-01416.txt](https://www.federalregister.gov/documents/full_text/text/2021/01/22/2021-01416.txt) | 1255 | `e9c0c529e88b9dce5c0238e4bef20605199a839a14d25f457a0e28a4843d7a69` |
| North Korea's designation | 82 FR 56100 | [2017-25547.txt](https://www.federalregister.gov/documents/full_text/text/2017/11/27/2017-25547.txt) | 1321 | `945f0387023acf10470d0532cb5282ee96cbb1fb74e5a938ceefc26ce79d2154` |
| Syria's rescission | 91 FR 55965 | [2026-17653.txt](https://www.federalregister.gov/documents/full_text/text/2026/08/31/2026-17653.txt) | 1661 | `9770113302795cfa216ec88cad12b9e4b74e8e8749e39e9e5f1097d9ec633500` |
| Ukraine emergency continued | 91 FR 8355 | [2026-03501.txt](https://www.federalregister.gov/documents/full_text/text/2026/02/20/2026-03501.txt) | 6001 | `e4ac7723231a97e78fe2b372c3c3b68145f92788f760704b3e4fd46add8bc0d4` |
| OFAC: accounting, consulting | | [922956](https://ofac.treasury.gov/media/922956/download?inline) | 156621 | `ab35cbb9c6f6907055235871bee2d19c4238c70e2426c42a7492131673ed5b14` |
| OFAC: quantum computing | | [926591](https://ofac.treasury.gov/media/926591/download?inline) | 160581 | `b949a655e5f24ea385aebdee2cf4c265cba3c4e9d80e5db5f94695c9515cbd34` |
| OFAC: crude oil services | | [929501](https://ofac.treasury.gov/media/929501/download?inline) | 1371265 | `671a78b39f83226dde47268c5acf9c6ba77ca3012a8a97dc43e7315a69369a33` |
| OFAC: crude oil price cap | | [929776](https://ofac.treasury.gov/media/929776/download?inline) | 618079 | `ab200c6e48694af120bda7b6d9b71b4146b6bd77d4045b8a673f8335901a93a0` |
| OFAC: petroleum products services | | [931021](https://ofac.treasury.gov/media/931021/download?inline) | 126655 | `639c962424e89d23475910040044c352657e16c84006c38ade3f836a18018ba2` |
| OFAC: petroleum products price caps | | [931026](https://ofac.treasury.gov/media/931026/download?inline) | 75137 | `e025156b6398de307954e2ec4d79476e383d87839d802349a24d86d49976158c` |
| OFAC: architecture, engineering | | [931776](https://ofac.treasury.gov/media/931776/download?inline) | 163832 | `bb32e8cd45330ed215694d68e6c32359e0e973005f48d55cec671427f5ecf2c9` |
| OFAC: metals | | [932801](https://ofac.treasury.gov/media/932801/download?inline) | 160158 | `d37cabbfa2c0d6254e247d3a04224d5ffcfc139d0faa2d75c136dcfb97ff9d85` |
| OFAC: IT and software services | | [932951](https://ofac.treasury.gov/media/932951/download?inline) | 251345 | `abc5d0a2a7f69614e4e73c568286478d8d2d97f05845bfa929de65722950aa5c` |
| OFAC: petroleum services | | [933901](https://ofac.treasury.gov/media/933901/download?inline) | 166995 | `be5fd82ada9e63ad9deff7576200d4287d5ddc3c1aa7ba963f5532c668ca329f` |

## Next

1. **COMPLY.1.** Read the EU's Article 215 regulations, the UK regime regulations and the UN resolutions, and author
   their embargo generations. Then read Turkey's asset-freeze decisions (from the Official Gazette and MASAK) and write
   their importer. Then read the Wassenaar Category 5 Part 2 and Turkey's dual-use communiqué for the classifications'
   bases.
2. **COMPLY.0.** Read the other eCFR texts of batch 19's fetch (COPPA, Section 508, the CVAA), then the CELLAR and UK
   acts.
