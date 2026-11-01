# COMPLY.1: the DPRK and Iran regimes read and the UN embargo written

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C, batch 30; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening). The register's fetch and its comparison are the
[report](2026-11-01-legal-register-un-dprk-and-iran.md). The previous batch read the Security Council's other regimes
([UN resolutions](2026-11-01-comply-1-un-resolutions.md)) and left the generation `open` until the DPRK and Iran regimes
were read.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The resolutions, the UN Digital Library records and the Council's documents
read here fall under that decision's approval of the first read of the official legal texts. Their bodies stay outside
the repository (`~/.cerid/downloads/legal-2026-11-01-l/` and `~/.cerid/downloads/un-probe-2/`), pinned in the report
and below by size and SHA-256. The five counsel questions this reading raises are open questions of the review
`counsel.2026-11-01`, never a clause of the row.

## CI first

No failure comes from a track C change, and no hosted run includes batch 29 (e1a449fa..408515f1) yet: the queued
runs 41295628482 (push), 41295647725 (dispatch) and 41295924613 (schedule) are on 43963cb2, an ancestor.

- **Complete run 41274380605 (de5e10b0)** has five new failures since batch 29. win-debug, win-shipping and
  win-public-checks fail only `filesystem: the last write time converts exactly and orders files within one second`
  (`tests/foundation/platform/test_filesystem.cpp:145`, track B's, fixed on main by cc5ee9e9). win-release fails that
  case and one more: `diagnostic host: the application's wild write leaves one attributable crash capture, and the
  repaired one runs clean` (`tests/foundation/perf/test_diag_host.cpp:1838` to 1859). Its capture says
  `code image: the module is not a Windows image`, so the code-image comparison of
  `engine/foundation/perf/src/fault_classification.cpp` (HW-RESILIENCE.2, track A: 3c360240, 1f053102, 76e61d51)
  returns no match, and the instruction check after it cannot be proven. Neither file has changed on main since
  de5e10b0, so it is still open, for track A. linux-gcc-debug and win-relwithdebinfo were still running.
- **Complete run 41269105413 (b0b1b245)** still reads "queued" for its Required result job; its jobs ended with the
  known failures.

<a id="reading"></a>
## What was read

The regimes were enumerated from the Council's own texts, not from a secondary page (main.un.org refuses the
fetcher). Resolution 2397 (2017)'s preamble recalls every DPRK resolution before it (825 (1993), 1695 (2006), 1718
(2006), 1874 (2009), 1887 (2009), 2087 (2013), 2094 (2013), 2270 (2016), 2321 (2016), 2356 (2017), 2371 (2017),
2375 (2017)), and the UN Digital Library's records of the Council's resolutions naming Korea list none after it but
the Panel of Experts' renewals: 2407 (2018), 2464 (2019), 2515 (2020), 2569 (2021), 2627 (2022) and 2680 (2023), each
of whose operative paragraphs only extends the Panel (2680 (2023) until 30 April 2024; none since). Iran's regime is
the five resolutions resolution 2231 (2015), paragraph 7(a), terminated and paragraph 12 applies again, with the
Council's record of paragraphs 11 and 12 found by its symbols: the notification, the two drafts put to the vote and
their meeting records.

The plan ([report](2026-11-01-legal-register-un-dprk-and-iran.md)) read 362 of 373 sources on 2026-11-01, the 12 new
library records and their English texts among them; `read` recorded the 12 resolutions below. Every text fetched is
byte for byte the one read by hand (the same SHA-256).

| Entry | Resolution | What it restricts |
|---|---|---|
| `un.res-1718-2006.dprk` | 1718 (2006), paragraphs 8 and 12 | The conventional arms of 8(a)(i), the nuclear, missile and WMD items and technology of 8(a)(ii), luxury goods (8(a)(iii)) and the training and services related to them (8(c)); the Committee's asset freeze and travel ban |
| `un.res-1874-2009.dprk-arms` | 1874 (2009), paragraphs 9, 10, 18 to 20 and 28 | All arms and related materiel and the services related to them (paragraph 10); financial services that could contribute to the programmes (paragraph 18) |
| `un.res-2094-2013.dprk-finance` | 2094 (2013), paragraphs 11, 15, 20 and 23 | Financial services and assets, bulk cash included, that could contribute; the items of annex III; luxury goods (annex IV) |
| `un.res-2270-2016.dprk` | 2270 (2016), paragraphs 6, 17, 27, 31, 33, 36 and 39 | Small arms too; specialized teaching or training in disciplines including advanced computer simulation and related computer sciences; any item that could contribute (the catch-all); aviation fuel; financial support for trade |
| `un.res-2321-2016.dprk` | 2321 (2016), paragraphs 4, 10, 11 and 30 | The items of annex III, item 13 "modelling and design software" for the aerodynamic and thermodynamic analysis of rocket or UAV systems among them; scientific and technical cooperation suspended; helicopters and vessels |
| `un.res-2375-2017.dprk` | 2375 (2017), paragraphs 13 to 15 and 18 | Condensates, refined petroleum and crude oil; every joint venture or cooperative entity with DPRK entities or individuals |
| `un.res-2397-2017.dprk` | 2397 (2017), paragraphs 4, 5 and 7 | Crude oil and refined petroleum beyond their caps; industrial machinery (HS 84 and 85), transportation vehicles (HS 86 to 89) and metals (HS 72 to 83) |
| `un.res-1737-2006.iran` | 1737 (2006), paragraphs 3, 4, 6, 12, 17 and 18 | Nuclear and missile items, materials, equipment, goods and technology and the assistance, training, investment and services related to them; the asset freeze; the Committee |
| `un.res-1747-2007.iran-arms-exports` | 1747 (2007), paragraphs 4 and 5 | Arms procured from Iran; designations |
| `un.res-1803-2008.iran` | 1803 (2008), paragraphs 7 and 8 | Every item of INFCIRC/254/Rev.7/Part 2 and 19.A.3 of Category II, save light water reactor supplies |
| `un.res-1929-2010.iran-arms` | 1929 (2010), paragraphs 8, 9 and 13 | The conventional arms of the UN Register and the training, services and assistance related to them; ballistic missile technology; the item lists |
| `un.res-2231-2015.iran` | 2231 (2015), paragraphs 7 to 9 and 11 to 13 | The previous resolutions applied again in the same manner from 28 September 2025 |

**Iran's measures apply again.** France, Germany and the United Kingdom notified the Council under paragraph 11 on
28 August 2025 (S/2025/538). Paragraph 11 required a vote on a draft continuing the terminations: S/2025/561
("the provisions of resolutions 1696 (2006), 1737 (2006), 1747 (2007), 1803 (2008), 1835 (2008), 1929 (2010) and
2224 (2015) remain terminated") received 4 votes in favour, 9 against and 2 abstentions on 19 September 2025
(S/PV.10001), and the Russian Federation and China's draft extending the resolution by six months (S/2025/539) the same
4, 9 and 2 on 26 September 2025 (S/PV.10006). No resolution under paragraph 11 was adopted, so paragraph 12 applied
those resolutions' provisions "effective midnight Greenwich Mean Time after the thirtieth day after the notification":
from 28 September 2025, and paragraph 9 kept paragraph 8's termination (ten years after the JCPOA's Adoption Day) from occurring. The four
measure resolutions are in force from that day (their `effective`), and resolution 2231 (2015) has no `sunset`. On
17 September 2026 the United States' draft renewing the Iran Panel of Experts (S/2026/740) was vetoed (11 in favour,
2 against, S/PV.10224); the measures have no period and stand. In that meeting China held that resolution 2231 (2015)
"expired on 18 October 2025"; that dispute is counsel's question, not the register's.

The Council's documents read for the chain, pinned here rather than as entries (each read from the UN Digital
Library on 2026-11-01):

| Document | Record | Read for | Bytes | SHA-256 |
|---|---|---|---:|---|
| S/RES/2087 (2013) | 742733 | designations; paragraph 9: no transfer whose originator, recipient or facilitator is designated | 43636 | `25df6934e8c31905dfe287443a40494aea9ded504479daf9d377baf1ee32e01d` |
| S/RES/2356 (2017) | 1288033 | designations only | 254020 | `238a2dd9b765080d9d801d727fbd209bb9870cdbfd6d2e9c42e858b54dc7371e` |
| S/RES/2371 (2017) | 1298283 | exports from the DPRK; paragraph 12's new joint ventures, replaced by 2375 (2017), paragraph 18 | 327006 | `398c4d8885d380b9d678c7cacf5b2a7b75ca9df4585d7de038a9d10f428ac895` |
| S/RES/2407 (2018) | 1478250 | the Panel renewed until 24 April 2019 | 335470 | `7e3c35d268a160e5356cd4db8def754cd8128dbee5c29078cf06bf0c7dcfd064` |
| S/RES/2464 (2019) | 3799758 | the Panel renewed until 24 April 2020 | 191838 | `97c9cd8de2fe072121f2ab5ae073a373330a77c2f07de422d3b5600e6bab6464` |
| S/RES/2515 (2020) | 3856395 | the Panel renewed until 30 April 2021 | 188627 | `b6de48b096ab446ea8eb8e8618790b66eeb8458c1cded89bc1c9909ad09e97c1` |
| S/RES/2569 (2021) | 3906135 | the Panel renewed until 30 April 2022 | 193834 | `96a6975f49626e0d824715caf8b020bb66d38fd9fd27fc380755fe00a224fec2` |
| S/RES/2627 (2022) | 3966837 | the Panel renewed until 30 April 2023 | 194338 | `4061e372307805d1f14162ba312c94957f3dddf3188d5274c52e037d6d96c6f2` |
| S/RES/2680 (2023) | 4007306 | the Panel renewed until 30 April 2024 | 196241 | `aa0f92392cb85f7dce09d92de4b80a57a6f10949873abf3ffc19078c5e31ec85` |
| S/2025/538 | 4087687 | the E3's notification under paragraph 11 of 2231 (2015), 28 August 2025 | 195581 | `12da0390df7ab84506f650ca5e0674239c5e291de1595f624f2602a4597f6fe2` |
| S/2025/561 | 4088987 | the draft continuing the terminations | 149346 | `0a6b874b38bd5acc4603ef254d842b9ff5022fc0ce8e401ebb786708ea2ad8de` |
| S/PV.10001 | 4089136 | its vote, 19 September 2025: not adopted (4-9-2) | 449810 | `f2c1ca86cc10dc477d947c18d7e714944635ac6e2e7dc61dbacf762324154c5e` |
| S/2025/539 | 4089594 | the draft extending 2231 (2015) by six months | 147296 | `2935e4375227ecfaf484c8887bd93fda00281f1b4fadb0d702482dbc1677bba4` |
| S/PV.10006 | 4089909 | its vote, 26 September 2025: not adopted (4-9-2) | 477154 | `414d30937ca5095a3299f16fc94caf58e563024675810048ed05a1de341e7d8f` |
| S/2026/740 | 4128531 | the draft renewing the Iran Panel of Experts | 175365 | `ad963db2d21d0e22ad091fc825d694107495549254045a7b91acd3a7d1cc358c` |
| S/PV.10224 | 4128549 | its vote, 17 September 2026: vetoed (11-2-2); China's position on 2231 (2015) | 273417 | `032a7b67174f34110ec0aebb46d81595e0720eacb77bb3b97df2735037cf9e45` |
| S/RES/2817 (2026) | 4105560 | the one 2026 resolution on Iran: attacks on neighbouring States, no supply measure | 168123 | `c16f33242dee731c1ffd86a29581adccfd224b0a4fc55931168e5c8ae836ce93` |

<a id="embargo"></a>
## The UN embargo written

`un.sc.embargo` adds two destinations, both sectoral, and nothing is open, so the importer writes it (generation
20261101000000, 15 destinations):

- **KP**, programme S/RES/1718 (2006), read in `un.res-1718-2006.dprk`: paragraph 8(a) and (c), extended to all arms
  (1874 (2009), paragraph 10; 2270 (2016), paragraph 6), to the annexed items (2094 (2013) and 2321 (2016), annex
  III), to any item that could contribute (2270 (2016), paragraph 27), to aviation fuel, helicopters and vessels,
  petroleum, industrial machinery, vehicles and metals. The DPRK regime is the broadest of the Council's, yet it does
  not reach every supply (food, medicine and the goods outside its headings remain), so it is sectoral: a match
  needs review. The US embargo (comprehensive) still blocks the DPRK when its authority is named.
- **IR**, programme S/RES/1737 (2006), read in `un.res-2231-2015.iran`, the reapplication in force: 1737 (2006),
  paragraphs 3, 4 and 6, 1803 (2008), paragraph 8, and 1929 (2010), paragraphs 8, 9 and 13.

The remaining founding and measure resolutions are entries read without a destination of their own. The generation
joins the CTest import (`crd-comply-sanctions-import`), a new hidden case screens it (`the authored UN embargo
decodes, adopts and screens destinations`: the 15 destinations and a place inside the DPRK need review, Sudan as a
whole names the country of the five Darfur states, Khartoum, Afghanistan, Guinea-Bissau, Yemen, Cuba, Syria and Turkey
are clear, and the reading past its maximum age is a gap), and the case screening the authorities together now holds
four (Iran 8 hits and the DPRK 5, both blocked by the US; Libya 4, Iraq 2 and a Darfur state 4 for review).

## Counsel questions

Five questions join `counsel.2026-11-01` (82 open), each on its entry, none holding the row:

- `un.res-2231-2015.iran`: whether the register is right to screen Iran under the reapplied measures in every
  jurisdiction the store serves, given that China, the Russian Federation and Iran dispute the notification.
- `un.res-2270-2016.dprk`: whether Cerid's documentation, tutorials, samples and support given to a DPRK national are
  the specialized teaching or training (advanced computer simulation and related computer sciences) of paragraph 17.
- `un.res-2321-2016.dprk`: whether Cerid's simulation is annex III's item 13 (modelling and design software for
  aerodynamic and thermodynamic analysis of rocket or UAV systems), and whether a collaboration session or shared
  project with a person representing the DPRK is the scientific or technical cooperation paragraph 11 suspends.
- `un.res-2375-2017.dprk`: whether a co-development project, a revenue share or a product made with a DPRK person is a
  joint venture or cooperative entity (paragraph 18).
- `un.res-2397-2017.dprk`: whether paragraph 7's industrial machinery (HS 84 and 85) reaches software supplied by
  download, or only goods such as a computer or recorded media.

## Verification

- `python scripts/test-repository-tools.py SanctionsLists RegisterReview`: 57 cases pass; the UN case now imports the
  real register and checks the written generation (15 destinations, the DPRK and Iran sectoral, nothing open, the
  reapplied resolutions in force from 28 September 2025 and resolution 2231 (2015) with no end). Five register
  mutations, each caught: the reading reopened (the Python case), the DPRK made comprehensive (the Python case),
  resolution 2231 (2015) given a sunset of 2025-10-18 (the Python case and the guard twice), resolution 1929 (2010)'s
  `effective` put back to its adoption (the Python case) and the DPRK destination resting on an entry that does not
  exist (the Python case and the guard).
- `gen-comply-rules.py --review INT`: generation 6; `check-generated.py --refresh` for the file.
- The cloud machine (linux-gcc-debug, b87b36f0, 10:27 to 10:33 UTC): the comply and ceridc targets build with no
  warning; `ctest -R "comply|ceridc|^crd-no-|^crd-check"` passes 61 of 61, the import writing the eight generations;
  the hidden `[.imported]` cases pass (1267 assertions in 7 cases). Two C++ mutations through the importer, each
  caught by `crd-comply-sanctions-imported`: the DPRK made comprehensive, and Iran's destination dropped; the
  restored register passes again. `ceridc screen --authority un.sc.embargo` on the real binary answers review for KP
  (S/RES/1718 (2006)), IR (S/RES/1737 (2006)), LY and SD-DN (a match), review for SD (the five Darfur states,
  possible) and clear for TR; with the four embargo authorities, KP is blocked by 31 CFR 510 with the EU, UK (twice)
  and UN programmes reported.
- `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py`, `check-generated.py` and
  `check-registers.py` pass.

## Next

1. **CI.** Complete-tier run 41298463221 (13453acb, dispatched on yatiyr/CRD) is the first hosted run of the UN
   generation, its two hidden cases and the rewritten Python case on Windows, clang-cl and ARM64: audit it when it
   ends.
2. **COMPLY.1.** Read the Darfur renewal before 11 December 2026 (from that day the guard and the import fail until it
   is read). Then Turkey's asset-freeze decisions and their importer (Laws No. 6415 and 7262, from the Official
   Gazette and MASAK), and the Wassenaar Category 5 Part 2 and Turkey's dual-use communiqué for the classifications'
   bases.
