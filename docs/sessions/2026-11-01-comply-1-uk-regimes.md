# COMPLY.1: the UK regime collection read, its regulations read and the UK embargo authored

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.1](../ROADMAP.md#slice-comply.1) and [COMPLY.0](../ROADMAP.md#slice-comply.0) (track
C, batch 28; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts) and [sanctions and embargo screening](../design/registers.md#screening).
The register's fetch and its comparison are the [report](2026-11-01-legal-register-uk-regimes.md). The previous
batches read the EU's regimes ([EU embargo](2026-11-01-comply-1-eu-restrictive-measures.md),
[EU regimes](2026-11-01-comply-1-eu-arms-and-repression-regimes.md), [EU regime index](2026-11-01-comply-1-eu-regime-index.md));
the UK's generation was the first item batch 27 named.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The FCDO's regime collection, its 71 documents and the 28 instruments read
here are part of the first read of the official legal texts that decision approved. Their bodies stay outside the
repository (`~/.cerid/downloads/legal-2026-11-01-j/`), pinned below and in the report by size and SHA-256.

## CI first

No failure comes from a track C change.

- **Complete run 41274380605 (de5e10b0)** had two new failures since batch 27, win-asan shard 2 and
  win-debug-scalar. Both logs were read: each fails one test, `filesystem: the last write time converts exactly and
  orders files within one second` (`tests/foundation/platform/test_filesystem.cpp:145`, `REQUIRE( stamp(older, at) )`).
  That file is track B's, and cc5ee9e9 on main ("narrow the file-time stamp to the file clock's tick before
  clock_cast") is its fix. The run's earlier failures are the known ones: the clang-cl `__udivti3` link (track A),
  the win-tidy finding fixed by 4b9bc837 and the linux-gcc `test_multivariate` warnings fixed by 76330344.
- **Push runs.** 41288713312 (f809f975), 41289183496 (8b62bf72) and 41290152491 (cb0c04ab) were cancelled before they
  started; 41291759660 (cacd6547) was pending at 09:15 UTC. So batches 26 and 27 still have no hosted run of their own.

<a id="the-index"></a>
## The UK's regime index

`gb.sanctions` cited the Sanctions and Anti-Money Laundering Act 2018, which enables the regimes and imposes nothing
itself. It now cites the FCDO's collection of the current UK sanctions regimes
(`https://www.gov.uk/government/collections/uk-sanctions-regimes-under-the-sanctions-act`), the UK's equivalent of
the EU Sanctions Map's regime index, which `register-review.py` reads through gov.uk's content API
(`/api/content/...`, the page's data):

- **The collection.** Its record lists content ids in three groups: "Regimes listed by country" (54 ids),
  "Regimes listed by theme" (18) and "More information" (1). `links.documents` holds a record for each document gov.uk
  publishes; two listed ids have none (one in the country group, the other under More information), and gov.uk answers
  404 for both. The rendered page shows exactly the 71 published documents, so an id without a record is
  unpublished. The text read is the regime groups' structure: the published documents by group and the count of
  unpublished ids. Its version is `71 documents: 53 by country, 18 by theme, 1 listed unpublished`, and a group other
  than the three refuses the read.
- **Its documents.** The collection is its own index: the fetch reads every published document of the two regime
  groups through the same API. Each regime's statutory guidance opens with "The <name> Regulations ..., and certain
  other regulations, are in force", whose link is the first legislation.gov.uk link of its body. A later link is an
  amending instrument (Syria's guidance links S.I. 2025/507, the Syria (Sanctions) (EU Exit) (Amendment) Regulations
  2025, which legislation.gov.uk serves only as made), and a revised text applies such an instrument or names it among
  its unapplied effects. A list of designations links none or the same regulation. The China guidance links the Export
  Control Order 2008.
- **The judge.** `judge_uk_regimes` reports two things. The first is a regulation of a regime listed by country that
  is no entry of the register. The second is a GB entry of COMPLY.1 on legislation.gov.uk that no regime names any
  more, whose revocation is then read. A regime listed by theme reaches the persons it designates only (every thematic
  guidance says "no sanctions that apply in respect of goods and services"), so it owes no entry: the UK Sanctions List
  screens its persons. While a regime document is unread, no entry is judged ended.
- **Wording.** A regime index's documents are not acts a consolidated text lacks. So the report now counts "documents
  their indexes name", `read` names "the document it names", and the "read the acts its consolidated text lacks" hint
  is left off an entry whose reader indexes a class.

The rule "the first link names the regulation" was checked against all 40 regime documents with a link (37 guidance
pages and the lists of designations of Iran, Libya and Russia): each first link is the regime's principal instrument,
whose title (in the table below) is no amending instrument.

<a id="reading"></a>
## What was read

The register gained one entry per regulation of a regime listed by country: 27 regulations and the Export Control
Order 2008. It was refreshed (`legal-2026-11-01-j`, 280 of 293 sources read; the 13 not read are the hosts that
refused before), compared and applied. The comparison found no regime by country without its entry, and no entry left
over. Each text is legislation.gov.uk's revised version, and none names an unapplied effect in force by 2026-11-01. The
umbrella read is the collection (72 560 bytes, SHA-256 `5f540694186a876359378bf67155c215896d043c31183da4ed168cd3e1bd4544`;
the text read, its regime groups, is `66ecf0898474516fd1f979416789ad50619eb761f36ee99d250dcc0d82307a9b`).

| Entry | Instrument | Version | Bytes | SHA-256 |
|---|---|---|---:|---|
| `gb.si-2020-948.afghanistan` | S.I. 2020/948 | 2026-05-13 | 455563 | `835d5d87bfb16a0452211181684826646fb65936524210cc0cc09a851a7f1e63` |
| `gb.si-2019-600.belarus` | S.I. 2019/600 | 2026-05-13 | 3029575 | `743e2bc2a68ca17175f53cb5adbcbc852796e774895a89780ce8e371f7714c9d` |
| `gb.si-2020-608.bosnia-and-herzegovina` | S.I. 2020/608 | 2026-05-13 | 502730 | `b66d3148b82a62d975e828e77457a7290e10f2aa706af3fc95c88e7d9c01dfed` |
| `gb.si-2020-616.central-african-republic` | S.I. 2020/616 | 2026-05-13 | 791960 | `c796c24ce50ab1f6c2cbb0bdd92f7431831b6482f2cfbc599a51ca5dcf996a07` |
| `gb.si-2008-3231.export-control-order` | S.I. 2008/3231 | 2025-12-16 | 1602358 | `e612ee6bd3e412f9f8cb9a4fb9be12930d51c6ce70aa71c382cf924eb3a8c6e0` |
| `gb.si-2019-433.drc` | S.I. 2019/433 | 2026-05-13 | 794337 | `2dc659d2564b23cda09aebde08795a37120c840ef6e40603e80f4aabea1e27c8` |
| `gb.si-2019-411.dprk` | S.I. 2019/411 | 2026-05-13 | 1542939 | `884775e9ad7ed2ab3c7d844d4fe95ea3c14c9ba2e1ff7ed9ece19354dde92661` |
| `gb.si-2019-1145.guinea` | S.I. 2019/1145 | 2026-05-13 | 493605 | `52cf782f73d29c094e22259404bfb4cb1863a7da403d392ef33f3df1ea08fd1f` |
| `gb.si-2019-554.guinea-bissau` | S.I. 2019/554 | 2026-05-13 | 516735 | `fc7c5c0f37baa38d4958d9952df451fcc48eec02134d7f50a624949b9f396b37` |
| `gb.si-2022-1281.haiti` | S.I. 2022/1281 | 2026-05-13 | 540874 | `591f3eedcc613b2fcaf0f34faf8c804cfaa9b736997c622d260b90192aef689c` |
| `gb.si-2023-1314.iran` | S.I. 2023/1314 | 2026-09-29 | 995264 | `6cfbd97471c2395f5df7ed0ed1f8c852dbd6e6bd04b9119454fced8579c1e655` |
| `gb.si-2019-461.iran-nuclear` | S.I. 2019/461 | 2026-09-29 | 2070868 | `30fb1ad1d6ac75bbc37fd8c5b94ddaa900a0d33add13884c7d4c13918c2b6190` |
| `gb.si-2020-707.iraq` | S.I. 2020/707 | 2026-05-13 | 581672 | `166b7a651e1b6a5a463810ca922907dde800f0678e184a7348aedfb6f4721522` |
| `gb.si-2020-612.lebanon` | S.I. 2020/612 | 2026-05-13 | 250943 | `c9d4c3e64716a389e936bd5784b4a7ea97618d1321788237ebcc2d14bc2dea21` |
| `gb.si-2020-617.lebanon-hariri` | S.I. 2020/617 | 2026-05-13 | 337403 | `4bedcb3642843b687a85508392f457e44a7f34fa7631ea17ff3f2ae23438d9b0` |
| `gb.si-2020-1665.libya` | S.I. 2020/1665 | 2026-05-13 | 835556 | `f86175b12830358eaf2ed1ddbd6b807420f47a7a2bd1932ea4d1dfcb0c37e840` |
| `gb.si-2020-705.mali` | S.I. 2020/705 | 2026-05-13 | 554348 | `03c4ab911f33da1de38de00c8b542ac576aa6d872aad1751a903a334c272e464` |
| `gb.si-2021-496.myanmar` | S.I. 2021/496 | 2026-05-13 | 752182 | `62f3c38ea0cecea1f6612c66cc767e13b57d74598b0692bdf8ec28f42ab482ee` |
| `gb.si-2020-610.nicaragua` | S.I. 2020/610 | 2026-05-13 | 491294 | `2326a702449cbe6af7a45c914f6ecba15328f526d70a1516b92ac2a446eec0bb` |
| `gb.si-2019-855.russia` | S.I. 2019/855 | 2026-05-20 | 7966930 | `146f749d478109c2e8c72d7620eaab8dbe1352d270d35828e95566935573e99f` |
| `gb.si-2020-642.somalia` | S.I. 2020/642 | 2026-05-13 | 823144 | `70e022797ffd8070688b0d62e3493e8acab7e3edf8386d25728826a6555700ce` |
| `gb.si-2019-438.south-sudan` | S.I. 2019/438 | 2026-05-13 | 822013 | `318b719705eae2e1f01f2c828c53e1b049a53cfde7fc622bf50d9075ac1f2e7e` |
| `gb.si-2020-753.sudan` | S.I. 2020/753 | 2026-05-13 | 705238 | `cd8e0624f99aaf802682c6c69bd442791976a65e0397f9af7ff85d10eba17660` |
| `gb.si-2019-792.syria` | S.I. 2019/792 | 2026-05-13 | 1098067 | `b5254e65e9e7abf9e30061b9d2012ddf04d57146d696813b22f6025f2038619f` |
| `gb.si-2020-1233.syria-cultural-property` | S.I. 2020/1233 | 2026-05-13 | 169583 | `f22da12b48ceab2e1d84a54913778ab08adeda08346f1a91a0006a43445027c9` |
| `gb.si-2019-135.venezuela` | S.I. 2019/135 | 2026-05-13 | 808397 | `8f4cead7f0b003a15a07801fddb9dee37d0745c88bbb6beb4635a83a0b45e6a1` |
| `gb.si-2020-1278.yemen` | S.I. 2020/1278 | 2026-05-13 | 769864 | `7a8fd234ae17115fa936308a67aebcb7834615dfcb79e6f824ff0b4de5d3a0c0` |
| `gb.si-2019-604.zimbabwe` | S.I. 2019/604 | 2026-05-13 | 757260 | `93f328913408b567a89607816005d17dc9113d849fe101b6717d24596385b386` |

Effective days: Haiti's (2022-12-28), Iran's 2023 regulations (2023-12-14) and the Export Control Order (2009-04-06)
name their day. The EU Exit regulations come into force "in accordance with regulations made under section 56", and
those commencement regulations were not read here: their day is recorded as IP completion day, 2020-12-31, and
Myanmar's as 2021-04-29, from what is known of them, not from the texts read.

What each says that bears on Cerid ("a person connected with" a country is one ordinarily resident or located there,
or incorporated, constituted or domiciled there):

- **Russia (S.I. 2019/855).**
  - **Restricted goods.** Regulations 22 to 29 bar exporting, supplying, making available or transferring restricted
    goods and technology to a person connected with Russia or for use there. These include critical-industry goods,
    and Schedule 2A Part 4 Chapter 2 names 5A992, 5D992 and 5E992: information security items classified under the
    Cryptography Note, that is mass-market encryption. Regulation 60B excepts consumer communication devices and
    software updates for civilian use; regulation 60A excepts a traveller's personal effects.
  - **Sectoral software.** Regulations 46Z34 to 46Z38 bar making available, transferring or supporting sectoral
    software and technology. Schedule 3IA names enterprise resource planning, customer relationship management,
    business intelligence, supply chain, data warehouse, maintenance, project management and product lifecycle
    management software, and building information modelling, computer aided design and manufacture and engineer to
    order software. Regulation 60DZA(3) excepts software of a non-commercial nature or for personal use. This is the UK
    twin of the EU's Annex XXXIX.
  - **Services.** Regulation 54C bars IT consultancy and design services (CPC 83131 and 83141), engineering,
    advertising and other professional services to a person connected with Russia. Regulation 46K bars interception and
    monitoring services to its Government, and Chapter 6A obliges internet services toward designated persons only.
  - **Non-government controlled Ukrainian territory.** This is Crimea (the Autonomous Republic and Sevastopol) and the
    non-government controlled areas of the Donetsk, Kherson, Luhansk and Zaporizhzhia oblasts. Regulations 30A to 30H
    bar military goods there, 47 to 53 imports and infrastructure-related goods, 53A the defence and security goods of
    Schedule 3C, and 54 infrastructure and tourism services. As in the EU, no subdivision draws the oblasts' boundary.
- **Belarus (S.I. 2019/600).** Regulations 21 to 27 bar the same critical-industry goods, Schedule 2A's 5D992 included,
  with dual-use, interception and monitoring, luxury and military goods, to a person connected with Belarus or for use
  there. Regulation 31E excepts consumer communication devices and software updates for civilian use, as Russia's
  regulation 60B does. Regulation 24A bars interception and monitoring services to the Government.
- **The DPRK (S.I. 2019/411).**
  - Regulations 35 to 47 bar military, dual-use and arms- and WMD-related goods, luxury goods and others.
  - Regulation 56(1)(e) bars computer and related services to or for the benefit of a person connected with the DPRK.
  - Regulation 57 bars financial services for any trade with the DPRK, and regulation 28 every transfer of funds to or
    from it.

  A paid sale there needs a licence, yet the measures do not reach every supply, so the destination is sectoral and a
  match needs review; the US embargo still blocks it.
- **Iran.**
  - **The 2023 regulations (S.I. 2023/1314).** Internal repression and interception and monitoring goods and services
    (regulations 28 to 35), and goods and technology of strategic concern (37 to 43).
  - **The nuclear regulations (S.I. 2019/461).** Military, missile, nuclear and other restricted goods (21 to 27);
    gold, precious metals, diamonds and energy goods and services (27A to 27O); and sectoral software (27P to 27V).
    Schedule 1I names business enterprise software (with data services and access control software added) and
    industrial design software. Software in the public domain or used for basic scientific research is outside the
    definition, and regulation 37D(3) excepts software of a non-commercial nature or for personal use.
- **Arms and repression regimes.** These are restricted supplies to a person connected with the place or for use
  there:
  - Iraq (17 to 21), Lebanon (7 to 13), South Sudan and Sudan (21 to 28) and Haiti (15 to 22): military goods.
  - Somalia (21 to 27 and 36): military and IED component goods.
  - Libya (23 to 31): military, internal repression and migrant smuggling goods.
  - Myanmar (22 to 28; 30 to 36 dual-use goods for military use; 37 interception services).
  - Venezuela (22 to 30), Zimbabwe (21 to 28) and Syria (28 to 35 and 48).

  The Central African Republic's measures reach armed groups operating there (21 to 28), and the Democratic Republic
  of the Congo's non-governmental persons operating there (21 to 28). Iraq differs from the EU: the UK embargoes
  military goods to Iraq itself, where the EU's Regulation (EC) No 1210/2003 does not.
- **The Export Control Order 2008 (S.I. 2008/3231).** Article 20 bars supplying or delivering goods subject to trade
  controls (the military goods of Schedule 2) from one third country to another that is an embargoed destination.
  Schedule 4 Part 1 lists the DPRK and Iran. Part 2 lists Belarus, Burma (Myanmar), the Central African Republic,
  China (other than its Special Administrative Regions), the Democratic Republic of the Congo, Hong Kong, Lebanon,
  Libya, Macao, Russia, South Sudan, Sudan, Venezuela and Zimbabwe. The FCDO lists the arms embargo on mainland China
  and Hong Kong among its regimes, and it rests on this order alone.
- **Persons only.** These regulations restrict only what reaches designated persons, whom the UK Sanctions List
  (`gb.fcdo`) screens:
  - asset freezes alone: Bosnia and Herzegovina (11 to 15), Guinea (11 to 15), Guinea-Bissau (11 to 15), Lebanon's
    Hariri regime (8 to 12), Mali (12 to 16) and Nicaragua (11 to 15);
  - asset freezes with military goods to designated persons: Afghanistan (8 to 12; 15 to 22) and Yemen (12 to 16;
    20 to 27).

  Syria's cultural property regulations (S.I. 2020/1233, 7 to 12) restrict trade in property removed from Syria and no
  supply to it.

`register-review.py read` recorded the 29 entries. The GB rule generation was reviewed (generation 8, 2026-11-01). Each
entry that names a destination is checked by the hidden `[.imported]` case that screens its place. Each that names
none is checked by the imported official lists' case.

<a id="embargo"></a>
## The UK embargo

`gb.fcdo.embargo` (generation 20261101000000, 92 days' maximum age, `open` null) holds 40 destinations. The reason
"Not yet read" for `gb.sanctions` is gone. No UK instrument embargoes a place outright, so every destination is
sectoral, apart from the four oblasts, which are partial:

- **From the regulations (24).**
  - S.I. 2019/855: RU, UA-43 and UA-40, and the partial oblasts UA-14, UA-65, UA-09 and UA-23.
  - BY (2019/600), KP (2019/411), IR twice (2023/1314 and 2019/461), IQ (2020/707) and LB (2020/612).
  - LY (2020/1665), MM (2021/496), SO (2020/642), SS (2019/438), SD (2020/753) and SY (2019/792).
  - VE (2019/135), ZW (2019/604), HT (2022/1281), CF (2020/616) and CD (2019/433).
- **From the Export Control Order 2008 (16).** The embargoed destinations of Schedule 4 Parts 1 and 2: KP, IR, BY, MM,
  CF, CN, CD, HK, LB, LY, MO, RU, SS, SD, VE and ZW.

`ceridc screen --authority us.ofac.embargo,eu.council.embargo,gb.fcdo.embargo` on the written lists answered:

- blocked: Iran (seven hits) and North Korea (four);
- review: the Russian Federation (six), China and Iraq (one each, both UK) and Kherson (two, EU and UK);
- clear: Turkey.

## Counsel

Four questions join the open review `counsel.2026-11-01`, which now holds 77. As the user decided, none holds a row.

- **S.I. 2019/855 (Russia).**
  - Is mass-market encryption in an export or game built with Cerid 5D992 critical-industry technology that
    regulations 25 and 26 bar from every person connected with Russia? Does regulation 60B reach beyond software
    updates?
  - Is Cerid or its editor Schedule 3IA sectoral software, and does regulation 60DZA(3) except a natural person's
    personal use?
  - Are its cloud builds, collaboration servers or support IT consultancy and design services under regulation 54C?
  - Since the regulations bind UK persons and conduct in the UK, which of them reach the publisher (the user's Turkish
    company) through a UK store, payment provider, subsidiary or employee?
- **S.I. 2019/600 (Belarus).** The same encryption question: is a download of a game built with Cerid barred there,
  and does regulation 31E reach anything beyond software updates?
- **S.I. 2019/411 (the DPRK).** Do regulations 56(1)(e) and 28 bar a free download, an online session or a store
  account for a person there, so that the store should treat the DPRK as blocked rather than for review?
- **S.I. 2019/461 (Iran, nuclear).** Is Cerid or its editor Schedule 1I sectoral software, and does regulation 37D(3)
  except a natural person's non-commercial or personal use, such as a game or a hobby project?

## Verification

- **Python.** `SanctionsLists`, `RegisterReview`, `LegalRegister` and `ComplyRules` pass 64 cases. Three are new:
  - the collection read with its documents and judged across six scans (a regime by country without an entry, an
    entry no regime names, an unread document holding endings back, a regime moved between groups, a designations
    page linking another regulation first, a replaced document under the same counts);
  - the readers' refusals (13 collection shapes, 4 document shapes) and the first-link rule on the real link forms;
  - the real UK generation: 40 destinations, its 19 texts and the 9 read without a destination.

  Two cases changed only for the generalised wording.
- **Mutations.** Twelve source mutations were each caught:
  - the last link taken for the first;
  - regimes by theme owing entries;
  - endings judged while a document is unread;
  - unpublished ids not counted;
  - every GB legislation entry judged ended;
  - a sunset entry judged ended;
  - regime documents read as acts a text lacks;
  - the version unchecked;
  - another group accepted;
  - a document in both groups accepted;
  - the base path unchecked;
  - a zero-led act number accepted.
- **Guards.** These pass:
  - `check-registers.py`: 179 legal entries, 165 pinned, 77 open counsel questions;
  - `gen-comply-rules.py --check` and `check-generated.py`;
  - `check-master-plan.py`, `check-repository.py` and `check-allman-braces.py`.
- **C++.** These ran on the track's machine (linux-gcc-debug at cd5ef260, this tree's work-in-progress commit;
  09:09-09:13 UTC):
  - `crd-comply-tests`, `crd-fuzz-comply-sanctions`, `ceridc` and `crd-ceridc-tests` built with no warning;
  - the 41 scoped CTests (comply and the `crd-no-` and `crd-check` guards, `crd-check-isa-isolation` left out as in
    batch 24) passed;
  - the six `[.imported]` cases passed (1 025 assertions), the new UK case and the case screening all three
    embargoes among them.
  - A later check of the readings against the texts found two exceptions the first draft had missed: Belarus's
    regulation 31E and Iran's regulation 37D(3). Correcting them changed obligations and counsel questions only, not
    a destination, so the embargo's reading digest is unchanged. The GB rule file was regenerated after it, and
    `gen-comply-rules.py --check` passes.

  Two register mutations failed the fixture chain:
  - Iraq's destination dropped failed `crd-comply-sanctions-imported`;
  - Hong Kong's destination written as Macao made the importer refuse the generation (Macao twice under one
    programme).

  With the register restored, the chain passed again.

## Next

1. **COMPLY.1.**
   - Read the UN Security Council resolutions that impose embargoes for `int.un-sanctions`' own generation.
     `un.org`'s list page now redirects to main.un.org, which the fetch refuses as another host: correct the primary
     first.
   - Then read Turkey's asset-freeze decisions and write their importer.
   - Read the Wassenaar Category 5 Part 2 and Turkey's dual-use communiqué.
2. **COMPLY.0.** Read the other eCFR texts of batch 19's fetch (COPPA, Section 508, the CVAA), then the CELLAR and UK
   acts still unread. `gb.osa` is read at its version after 2026-11-06, with the Crime and Policing Act's sections.
