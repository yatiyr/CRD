# COMPLY.0: the acts an EU consolidated text lacks, found in CELLAR and read as published

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.0](../ROADMAP.md#slice-comply.0) and [COMPLY.1](../ROADMAP.md#slice-comply.1) (track
C, batch 26; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts). The register's fetch and its comparison are the
[report](2026-11-01-legal-register-acts-the-consolidated-texts-lack.md). The previous batch is the
[EU regime index](2026-11-01-comply-1-eu-regime-index.md), whose first next item this batch closes.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The CELLAR metadata and the nine acts read here belong to the first read of
the official legal texts that decision approved. Their bodies stay outside the repository
(`~/.cerid/downloads/legal-2026-11-01-i/`), pinned below and in the report by size and SHA-256.

## CI first

No failure comes from a track C change.

- **Complete run 41274380605 (de5e10b0)** was still queued as a run at 07:55 UTC. Its failed jobs are the three batch 25
  recorded: win-clang-cl and win-clang-cl-shipping (track A's `__udivti3` link in
  `tests/foundation/perf/test_instruction_access.cpp`), and linux-gcc-release (GCC `-Wmaybe-uninitialized` in
  `tests/numerics/hesap-stats/test_multivariate.cpp:125`, which 76330344 fixes). Its win-arm64-debug and Windows
  Repository checks jobs passed.
- **Push runs.** 41284892084 (c43cede7) and every push run after it up to 41286060489 (e519b77d) were cancelled by a
  newer push; 41286401310 (d4f461ae) was pending.

## The index

Batch 25 named the Sanctions Map's notes of amendments "not yet included" in a consolidated text as owing readings. Two
CELLAR probes showed the notes are the wrong source:

- **The notes lag CELLAR.** The two Council-regulation notes the map carried on 2026-11-01, the corrigenda 2026/506
  R(03) (Russia) and 2026/513 R(01) (Belarus), are both consolidated: CELLAR lists them under
  `act_consolidated_consolidates_resource_legal` of the versions this register read (02014R0833-20260724 and
  02006R0765-20260724, the latter carried into 02006R0765-20261015). The map's notes stay out of the judgement, as its
  structure digest already left them.
- **The gap is general.** Every EU act the register reads in a consolidated version can lack an amendment published
  after the version, not only a sanctions regulation: the Cyber Resilience Act's consolidated text lacks Article 104 of
  Regulation (EU) 2025/327, which applies from 26 March 2027.

The version index of every `eur-lex.europa.eu/eli/...` citation now asks CELLAR, in the same query, for the acts that
change the act's English text and the consolidated versions consolidating each:

- the relations: an act that `amends` the act, one that `corrects` it, and one that corrects an act amending it (a
  corrigendum of an amending regulation, two hops), each with an English expression (a corrigendum of one language
  leaves the English text read unchanged: 32014R0833R(08) is Danish only);
- a consolidated version lists only the acts it adds, so an act is in the version read when any version up to it
  consolidates it; one consolidated only by a later version (not yet applicable) is lacked;
- an act CELLAR consolidates in no version is lacked when it is dated after the version read. Older ones are a gap of
  CELLAR's metadata, not of the text: each sanctions regulation has 3 to 7 corrigenda of its amending acts, dated
  2011-2025, that no version records as consolidated while the later versions carry the corrections.

The query answers in 0.35 to 0.63 s for each act probed (833/2014, 765/2006, 36/2012, 2024/2847, 2016/679).

**The version names what the text lacks.** The index reader returns the consolidated version as the URL part and, as
the version, the version followed by each act it lacks (`02024R2847-20241120 + 32025R0327`), so a newly published
amendment is a new version, which the review already reports as an amendment since the entry was read. The SHA-256
stays that of the consolidated text: an act as published never changes, and a correction is a new CELEX number. The
`consolidated` record keeps its three fields.

**The fetch reads each lacked act as published.** The acts are found only when the index is read, so they are items
beyond the plan's sources: the fetch reads each once (`celex-<act>-published`, from
`https://publications.europa.eu/resource/celex/<act>` with the CELEX number percent-encoded, in English XHTML) right
after the index that names it, and records an error like any other source. `plan` says so in its listing, the report's
header counts them, and each owed reading names them. `read` refuses an entry until every act its version names was
read and its XHTML names that act.

**Proof.** `RegisterReview` has 26 cases and `SanctionsLists` 19; both classes pass.

- The reader case covers:
  - the act consolidated by an earlier version;
  - the act consolidated by a version not yet in force (lacked);
  - an unconsolidated act dated after the version (lacked), on it and before it (not lacked);
  - the act read as adopted, lacking all of them;
  - eleven refused shapes.
- The query case pins the three relations, both English restrictions and the optional consolidation.
- A new end-to-end case:
  - plans three EU acts;
  - fetches a regulation all three lack only once;
  - records an unreachable corrigendum as not read;
  - reports each reading with the files to read;
  - refuses `read` for the entry whose act was not read and for the one whose act names another regulation, writing
    nothing;
  - records the third entry with the act in its version, which the register's guard accepts.
- Nine mutations of `register-review.py` were each caught:
  - consolidation by any version counted;
  - every unconsolidated act counted;
  - the English restriction on the acts dropped;
  - the two-hop relation dropped;
  - `read` skipping the acts;
  - `read` skipping their reader;
  - the fetch reading an act twice;
  - the report omitting the acts;
  - corrigenda refused by the CELEX pattern.

## Reading

<a id="reading"></a>
The applied run (`legal-2026-11-01-i`, 185 planned sources and 9 lacked acts, 181 read) found 11 entries whose
consolidated text lacks an act. Each act was read in full:

| Act | Bytes | SHA-256 | What it changes |
|---|---:|---|---|
| [32025R0327](https://publications.europa.eu/resource/celex/32025R0327) | 884 421 | `bf331ac48264118fb0461f793123a2726da88195a11d4bccb2075fd6326909da` | Regulation (EU) 2025/327 (European Health Data Space), Article 104, applying from 26 March 2027 (Article 105): the Cyber Resilience Act's Article 13(4) lets the risk assessment of a product under Article 12 or the new Article 32(5a) be part of another Union act's risk assessment; Article 31(3) asks one set of technical documentation for such a product; Article 32(5a) has a product classified as an EHR system demonstrate conformity through the EHDS procedure. |
| [32026R2374](https://publications.europa.eu/resource/celex/32026R2374) | 64 332 | `dbcc182e5cc895b802c7127621f4216593cafbdb8b93af503412b93e98eb3353` | Council Regulation (EU) 2026/2374 (in force 28 October 2026) amends Regulation (EC) No 1183/2005 (DRC) after UNSC Resolutions 2667 (2022) and 2825 (2026): a new Article 1a(-1) prohibits selling, supplying, transferring or exporting Common Military List items, firearms, their parts and ammunition to any non-governmental entity or person operating in the DRC; Article 1a(1) now also covers services related to military activities and financing; the advance notification of Article 1a(2) is deleted; Article 1b's derogations are rewritten; new designation criterion 2a(1)(l), reporting duties, an anti-circumvention clause (Article 7b) and the territorial scope (Article 11). |
| [32026R1880](https://publications.europa.eu/resource/celex/32026R1880) | 12 120 | `03f6bd12322f33d3f26400737df8a4fe872996a12f5352941ac9ea23185dae7c` | Council Implementing Regulation (EU) 2026/1880: two persons and one entity added to Annex I of No 1183/2005. |
| [32026R2191](https://publications.europa.eu/resource/celex/32026R2191) | 24 166 | `550316a6c301b028e9622e21d22bb6d502bbb238ebd8f9df94a62ddb6e644bf6` | Council Implementing Regulation (EU) 2026/2191: four persons and one entity moved from Annex Ia to Annex I of No 1183/2005. |
| [32026R2278](https://publications.europa.eu/resource/celex/32026R2278) | 46 729 | `b5f3b06c1a725f70a79f3bf33bb17096eeaa3b682b7794223e2bce0a6c535860` | Council Implementing Regulation (EU) 2026/2278: Annexes XV and XVI of Regulation (EU) 2017/1509 (DPRK), the persons and entities listed. |
| [32026R2388](https://publications.europa.eu/resource/celex/32026R2388) | 7 970 | `92e095362a71986c4522b431c43f2adbb5d852c904fe7553981f0661bace3dd3` | Commission Implementing Regulation (EU) 2026/2388: one entity deleted from Annex IV of Regulation (EC) No 1210/2003 (Iraq). |
| [32026R2138](https://publications.europa.eu/resource/celex/32026R2138) | 125 170 | `c2d08a448f9d0d165b47ded0385dc0e6b4af96213f96a851c4e382208504feb5` | Council Implementing Regulation (EU) 2026/2138: the entries of 24 persons and one entity in Annex I of Regulation (EU) No 753/2011 (Afghanistan) replaced. |
| [32026R2138R(01)](https://publications.europa.eu/resource/celex/32026R2138R%2801%29) | 3 630 | `76b5082de6874634e5346c15e9f8d3eb91eea76aaf7d1f9fc6dbeac756da547b` | Its corrigendum: the Annex's introductory sentence amends Annex I to No 753/2011, not the annex of Decision 2011/486/CFSP. |
| [32023R2465R(02)](https://publications.europa.eu/resource/celex/32023R2465R%2802%29) | 4 738 | `774663b935112210d4c14c360deb6508b22a687e5fd3d65e17e91457782a2368` | A corrigendum to Commission Delegated Regulation (EU) 2023/2465 on marketing standards for eggs (a Hungarian label in Annex I). CELLAR links it as correcting Council Regulation (EU) 2024/2465, which amends 47 sanctions regulations, so it reached Lebanon, both Somalia regulations, Yemen and Afghanistan; it changes none of them. |

What each changes in the register:

- **eu.reg-1183-2005.drc.** The obligation is restated from the amended Article 1a: no Common Military List goods,
  firearms or ammunition, and no related technical assistance, brokering, services or financing, to a
  non-governmental entity or person operating in the DRC. The notification duty is gone. The EU embargo's DRC
  destination stays sectoral (review), so the generation is unchanged.
- **The three CRA entries** keep their obligations. Article 104 of 2025/327 touches no clause they pin: the entries pin
  the essential requirements, the reporting duty and the contact. The new Article 13(4) and 31(3) wording governs only
  products also under another Union act (high-risk AI systems, EHR systems), which their notes now name.
- **The other seven** change annexes of listed persons, which the EU financial sanctions file screens, or nothing; their
  obligations hold and their notes name the acts read.

Each of the 11 entries is recorded with `read` at its new version, its source this section. The DRC obligation and
article change the EU rule file, reviewed as EU generation 10 (`gen-comply-rules.py --review EU`).

## Verification

- **Python.** `RegisterReview` (26 cases) and `SanctionsLists` (19) pass, and the nine mutations above were each
  caught.
- **Guards.** `check-registers.py`, `gen-comply-rules.py --check`, `check-generated.py` (the EU rule file refreshed),
  `check-master-plan.py`, `check-repository.py` and `check-allman-braces.py` pass.

## Next

1. **COMPLY.0: the UK texts have the same gap.** Crown Legislation Markup lists the changes legislation.gov.uk has not
   applied (`ukm:UnappliedEffects`; `RequiresApplied="true"` marks those owed). On 2026-11-01 the Data Protection Act
   2018 has 10, the Equality Act 2010 18, the Online Safety Act 2023 25, SAMLA 2018 2 and the UK GDPR 6. The reader
   should name the owed, in-force effects in the version as CELLAR's acts are named, and the fetch should read each
   affecting act.
2. **COMPLY.1.** Read the UK's regime regulations through legislation.gov.uk's `uksi/` reader and the UN Security
   Council resolutions, each for its own embargo generation. Then read Turkey's asset-freeze decisions and write their
   importer, and read the Wassenaar Category 5 Part 2 and Turkey's dual-use communiqué.
3. **COMPLY.0.** Read the other eCFR texts of batch 19's fetch (COPPA, Section 508, the CVAA), then the CELLAR and UK
   acts still unread.
