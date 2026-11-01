# COMPLY.1: the EU Article 215 regulations read and the EU embargo authored

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.1](../ROADMAP.md#slice-comply.1) and [COMPLY.0](../ROADMAP.md#slice-comply.0) (track
C, batch 23; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening). The register's fetch and its comparison are the
[report](2026-11-01-legal-register-eu-restrictive-measures.md).

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The seven EU regulations read here are part of the first read of the
official legal texts that decision approved. Their bodies stay outside the repository
(`~/.cerid/downloads/legal-2026-11-01-e/`), pinned below by version, size and SHA-256.

## CI first

No failure comes from a track C change.

- **Run 41269105413 (b0b1b245).** Its nine failed Windows jobs all failed to build. Seven of them failed on
  `engine/foundation/platform/src/filesystem.cpp:308-309`, where `windows.h`'s `min` and `max` macros break
  `std::numeric_limits<i64>::max()`. The MSVC lanes report C4003 and C2589, and clang-cl reports "too few
  arguments". win-arm64-debug failed on `checksum.cpp` (C4324). The fixes are 9723ac75 (track B's file units) and
  921e19cc (track A). Both are ancestors of de5e10b0.
- **Run 41274380605 (de5e10b0)**, the first complete run with both fixes, had started only win-vs (passed),
  linux-gcc-arm64-asan and one linux-gcc-asan shard at 06:00 UTC. The other 26 jobs were queued.
- **Push run 41278765658 (bd58e477)** holds the first hosted run of batch 22. It was still pending.

## One rule for an umbrella entry

`eu.restrictive-measures`, the EU's screening entry, names a class of acts: the Council regulations adopted under
Article 215 TFEU. No single text consolidates them, and its primary is the sanctions map, which is not a legal text.
The embargo rules required the generation's own entry to be read, so the EU had no honest way to pass them. Recording
the map page as read would have passed the rule without reading any law.

The rule now matches what the generation rests on.

- **The generation's entry.** It must be a register entry whose instrument names a jurisdiction. Its primary is the
  generation's `source`. The guard still requires a screening entry.
- **Each destination's entry.** It must be read from its primary source, as before. It must also be an instrument of
  the generation's jurisdiction. A destination read in another jurisdiction's act is refused by name.
- **The reading.** The reading is the destinations' texts alone. So `published` is the day of their latest reading,
  and `source_sha256` is the digest of those texts and the destinations.

For the US generation nothing changes. Its destinations already name `us.sanctions`, so its texts, digest and
written file are byte-identical to HEAD's (compared on the importer's output). `jurisdiction_of` now uses the same
lookup, so the module has no second one.

<a id="reading"></a>
## What was read

The register's fetch of 2026-11-01 (`legal-2026-11-01-e`, 133 of 145 sources) was compared and applied. Each
regulation's consolidated text was the same in a probe fetch made earlier that day. Each is CELLAR's latest English
consolidated version, a documentation text. The next scan reads amendments published after it.

| Entry | Regulation | Version | Bytes | SHA-256 |
|---|---|---|---:|---|
| `eu.reg-833-2014.russia` | (EU) No 833/2014 | 02014R0833-20260724 | 10349814 | `2682ecf3442d2f16a3b36ef4d9bbb6fde43fd4d6743bcd991bd4a8e14857770b` |
| `eu.reg-765-2006.belarus` | (EC) No 765/2006 | 02006R0765-20261015 | 9769815 | `4387a58c81ebe06d6d712e5d75ffe97577ec05bddc4fbd8a18843f1e1b19ef11` |
| `eu.reg-692-2014.crimea` | (EU) No 692/2014 | 02014R0692-20250225 | 188834 | `50062e4ab0a315810df7aa1c223723cd21acb824c8a142ad02254dec4637c511` |
| `eu.reg-2022-263.occupied-oblasts` | (EU) 2022/263 | 02022R0263-20250225 | 194569 | `f119b89f8fd53b686dc6d9be714fe07d3150ddbfeb892851588f5e3eeb621770` |
| `eu.reg-267-2012.iran` | (EU) No 267/2012 | 02012R0267-20260801 | 1914784 | `4f2d477c43b286cc569d92e399f8ac7bcd637002ec4529cb68d7556690f3b107` |
| `eu.reg-2017-1509.dprk` | (EU) 2017/1509 | 02017R1509-20251217 | 1738808 | `dbb874c9d7faa1c31bf8f9848dd48fdb56ebc25542d8ebfaebfb3ef53a2b3ad5` |
| `eu.reg-36-2012.syria` | (EU) No 36/2012 | 02012R0036-20260520 | 1699553 | `ee52d53521ef91e6ec66f3fbbcd4f6eecc2c8cd009fe5b849bed4203b14f0815` |

What each regulation says that bears on Cerid:

- **Russia, Regulation (EU) No 833/2014.**
  - **Article 2a(1)** prohibits selling, supplying, transferring or exporting the goods and technology of Annex VII
    to any natural or legal person in Russia, or for use there. The ban reaches related technical assistance,
    financing and intellectual property (2a(2)).
  - **Annex VII Category III Part 2** lists mass-market encryption goods and software under the Cryptography Note:
    X.A.III.201.c and X.D.III.201.c. Its note says the category "does not control goods for the personal use of the
    natural persons".
  - **Article 2a(4)** lets competent authorities authorise software updates and consumer communication devices, among
    other purposes.

  So the ban reaches mass-market encryption itself. That matters once NET.2 gives an export encryption.

  - **Article 5n(1)** prohibits providing certain services to the Government of Russia and to legal persons
    established there. These include IT consultancy (e), AI services: access to models or to platforms for training,
    fine-tuning and inference (g), high-performance computing including GPU-accelerated computing (h), and managed
    security services (i). Points (g) and (h) apply from 25 November 2025 and (i) from 25 May 2026.
  - **Article 5n(3)** prohibits supplying them the software of Annex XXXIX, and 5n(3a) its intellectual property.
    Annex XXXIX names "Design and Manufacturing Software used in the areas of architecture, engineering,
    construction, manufacturing, media, education and entertainment". This is the EU twin of the US IT and software
    services determination.
  - **Article 3h (Annex XVIII)** bans video game consoles as luxury goods. Cerid sells no hardware.
- **Belarus, Regulation (EC) No 765/2006.** It mirrors Russia, with one difference in reach.
  - **Article 1f with Annex Va** carries the same X.D.III.201.c entry and the same personal-use note. It reaches
    every person in Belarus.
  - **Article 1jc** carries the services, including GPU computing and managed security, and the Annex XXVI software,
    which also names media, education and entertainment. It reaches only the Republic of Belarus, its Government,
    public bodies, corporations and agencies, and those acting on their behalf or at their direction.
- **Crimea and Sevastopol, Regulation (EU) No 692/2014.** This is no comprehensive embargo. It bans:
  - importing goods originating there, and investment (Articles 2 and 2a);
  - the Annex II transport, telecommunications, energy and resource goods (2b);
  - IT consultancy and the Annex IV software for legal persons there, Annex IV also naming media, education and
    entertainment (2c);
  - tourism services (2d).

  Under the register's own definition, both regions are therefore sectoral, not comprehensive.
- **The non-government-controlled areas, Regulation (EU) 2022/263.** Article 1(d) names "the non-government
  controlled areas of Ukraine in the oblasts of Donetsk, Kherson, Luhansk and Zaporizhzhia". That is four oblasts, not
  the two E.O. 14065 names, and the regulation draws no boundary. Articles 2 to 6 repeat the Crimea measures for
  them, Annex III software included.
- **Iran, Regulation (EU) No 267/2012.** The version read is consolidated after Council Regulation (EU) 2025/1975 of
  29 September 2025 re-imposed the measures. It bans:
  - Annex I: the items Regulation (EU) 2021/821 lists, those of its Part A needing an authorisation under it;
  - Annexes II, III, VI, VIA and VIB, with Annex IIA needing an authorisation (Articles 2, 3, 4a, 8 and 10a);
  - the Annex VIIA software for integrating industrial processes (10d);
  - any transfer of funds of EUR 10 000 or more to or from an Iranian person without the competent authority's prior
    authorisation, and transfers with credit and financial institutions domiciled in Iran except as authorised
    (Articles 30 and 30a).

  Annex I reaches Cerid only if its cryptography is a dual-use item, which the classifications under `eu.dual-use`
  decide.
- **The DPRK, Regulation (EU) 2017/1509.** It bans:
  - the Annex II items, every item the dual-use list names among them (Article 3);
  - any item destined for the armed forces (5);
  - related technical assistance (7);
  - the Annex VIII luxury goods, video game consoles and electronic items for domestic use worth more than EUR 50
    among them (10).

  Article 21 prohibits every transfer of funds to or from the DPRK, except the transfers of up to EUR 15 000 that
  21(4) allows (a trade contract the regulation does not prohibit among them) and personal remittances of up to
  EUR 5 000 (21(5)). So it is no comprehensive embargo either, but a store payment from the DPRK is restricted.
- **Syria, Regulation (EU) No 36/2012.** This is the version consolidated after the economic measures were lifted in
  2025. What remains:
  - internal-repression equipment (Article 2a, Annex IA; 2b, Annex IX under authorisation);
  - military goods (3);
  - Annex V monitoring and interception equipment and software, under authorisation, and the related services to the
    Syrian State (4);
  - cultural property (11c).

`register-review.py read` recorded the seven entries (EU rule generation 7, reviewed 2026-11-01). Each entry's active
check is the hidden `[.imported]` case that screens its destinations.

<a id="embargo"></a>
## The EU embargo

`eu.council.embargo` (generation 20261101000000, written into an emptied directory; 92 days' maximum age). It holds
11 destinations, and none is comprehensive, because no regulation read embargoes a place outright:

- **RU, sectoral.** 833/2014: Articles 2a and 2b, 3h and 5n.
- **BY, sectoral.** 765/2006: Articles 1f and 1jc.
- **UA-43 and UA-40, sectoral.** 692/2014: Articles 2a to 2d.
- **UA-14, UA-65, UA-09 and UA-23, partial.** 2022/263: the specified territories, whose boundary no subdivision
  draws.
- **IR, sectoral.** 267/2012: Articles 2 to 10d, 30 and 30a.
- **KP, sectoral.** 2017/1509: Articles 3, 5, 7, 10 and 21.
- **SY, sectoral.** 36/2012: Articles 2a, 2b, 3, 4 and 11c.

Every measure that attaches to a place is a destination, whatever its sector. Whether a sale falls within the sector
is for the review to decide, not the screening. The generation's `open` names the regulations still to read: the
arms, internal-repression and monitoring regimes, which are Iran's Regulation (EU) No 359/2011 and Regulation (EU)
2023/1529, Myanmar, Venezuela, Libya, Iraq, Lebanon, Somalia, South Sudan, Sudan, the Central African Republic, the
Democratic Republic of the Congo, Zimbabwe, Haiti, Yemen and Afghanistan. Screened together with the US embargo, Iran
and Crimea still block (the US measure is comprehensive), Donetsk is partial under both, and the Russian Federation
has three hits.

## Counsel

Four questions join the open review `counsel.2026-11-01`, which now holds 67. As the user decided, none holds a row.

- **Regulation (EU) No 833/2014.** Where an export or a game built with Cerid carries encryption classified as
  mass-market (the transport NET.2 adds), is it X.D.III.201.c software that Article 2a(1) bars from every person in
  Russia? Does the personal-use note cover a natural person's download? Is Cerid or its editor Annex XXXIX design and
  manufacturing software used in media, education and entertainment? Are its cloud builds or hosted services
  GPU-accelerated computing, AI services or IT consultancy under Article 5n(1)?
- **Regulation (EC) No 765/2006.** The same encryption question under Annex Va. And how does the store establish
  that a Belarusian customer is not a State body, or acting for one, before supplying software Annex XXVI may list?
- **Regulation (EU) No 692/2014.** Is Cerid Annex IV software, so that it may not be supplied to a legal person in
  Crimea or Sevastopol? May the store supply it to a natural person there?
- **Regulation (EU) 2022/263.** How does the store establish that a person or payment in the four oblasts lies outside
  the non-government-controlled areas? And the Annex III software question.

## Verification

- **Python.**
  - `SanctionsLists` and `RegisterReview` pass 41 cases. Two are new: the umbrella embargo read in its destinations'
    acts, and the real EU generation. One is extended: an umbrella entry passing, an unread destination named, an
    entry missing from the register, and a destination of another jurisdiction.
  - Six importer mutations were each caught: the generation entry required to be read; no jurisdiction check; the
    generation's entry among the texts; the source taken from a read instrument; an empty jurisdiction accepted; no
    check of the generation's entry.
- **Guards.** `check-registers.py` passes: 131 legal entries, 117 pinned, 67 open counsel questions. So do
  `gen-comply-rules.py --check` (EU generation 7), `check-generated.py` (the EU rule file refreshed),
  `check-master-plan.py`, `check-repository.py` and `check-allman-braces.py`.
- **C++.** These ran on the track's machine (linux-gcc-debug at 69d527bd, the work-in-progress commit of this tree;
  06:20-06:28 UTC).
  - `crd-comply-tests`, `crd-fuzz-comply-sanctions`, `ceridc` and `crd-ceridc-tests` built with no warning.
  - Of the 62 scoped CTests, 61 passed. `crd-check-isa-isolation` failed only on two objects the partial build had not
    compiled (`crd-perf-tests` and `crd-platform-tests` had new sources from other tracks). After those two targets
    were built, it passed.
  - The four `[.imported]` cases (464 assertions) passed: the lists, the US embargo, the new EU embargo and both
    together. `[sanctions]` ran 4 500 assertions in 18 cases.
  - `ceridc screen --authority us.ofac.embargo,eu.council.embargo` answered on the written lists:
    - blocked: IR, UA-43, KP and CU;
    - review: RU (three hits), BY, UA-65, UA-14 (two partial hits), SY and VE;
    - clear: TR.

## Next

1. **COMPLY.1.**
   - Read the EU's remaining territorial regulations (the generation's `open`).
   - Read the UK's regime regulations through legislation.gov.uk's `uksi/` reader: `gb.sanctions` is SAMLA 2018,
     one text, as chapter V is.
   - Read the UN resolutions.
   - Then Turkey's asset-freeze decisions and their importer, the Wassenaar Category 5 Part 2 and Turkey's dual-use
     communiqué.
2. **COMPLY.0.** Read the other eCFR texts of batch 19's fetch (COPPA, Section 508, the CVAA), then the CELLAR and UK
   acts.
