# COMPLY.0: one instrument per entry

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.0](../ROADMAP.md#slice-comply.0) and [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C,
batch 18; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts). The fetch read here is the first read's
([report](2026-11-01-legal-register-first-read.md)).

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The official texts this batch located on mevzuat.gov.tr and the Official
Gazette are part of the first read of the legal texts that decision approved.

## CI first

No failure comes from a track C change that main has not already fixed.

- **Run 41254049879 (31404ec4), the complete run batch 12 dispatched.** win-relwithdebinfo has now failed too, on one
  test of 9 283: `crd-ci-orchestrator-e2e` (`scripts/test-ci-orchestrator-e2e.py`, `test_1_webhook_build_runs_end_to_end`)
  saw its plan job still `scheduled` when it expected the whole pipeline passed, after 10 s. That is the CI
  orchestrator's own end-to-end test, not a track C file; it is named here for its owner. The run's other failures are
  the ones batch 17 recorded (win-tidy: the `test_arguments.cpp` findings a40af1bf fixed; linux-gcc-release and
  linux-gcc-shipping: track B's `test_deflate.cpp`, rebuilt by 95ec6965). linux-gcc-debug, linux-clang-tsan,
  linux-gcc-public-checks, win-debug and win-asan (shard 1) were still running.
- **Run 41269105413 (b0b1b245), the complete run batch 17 dispatched,** was still queued at its preflight job, and the
  push run of aa4e9b3d had not started.

<a id="one-instrument"></a>
## One instrument per entry

The register's `about` says it holds one entry per instrument and article, and `read` records an entry only when the
instrument it names was read. Nine Turkish entries named a second instrument behind the law they cite, so `read` could
never record them, though their laws were read in the last two batches. Every such entry is now two: the law keeps its
id (and so its rule, its rows and its place in the fetch the first read planned), and the second instrument is a new
entry with its own official primary, its own effective day and the obligation that instrument carries. The same rule
split five entries of other jurisdictions.

| Entry (the law, read) | New entry (the second instrument) | Its primary |
|---|---|---|
| `tr.kvkk.transfer-abroad`: Law No. 6698 Article 9 | `tr.kvkk.transfer-abroad-bylaw`: the By-Law on transfers abroad (Articles 13-14) | mevzuat.gov.tr, by-law 40863 (Official Gazette of 10 July 2024, No. 32598) |
| `tr.kvkk.notice-and-consent`: Law No. 6698 Articles 3, 5 and 10 (already recorded) | `tr.kvkk.notice-communique`: the Communiqué on the obligation to inform | mevzuat.gov.tr, communiqué 24454 (10 March 2018, No. 30356) |
| `tr.btk.encryption`: Law No. 5809 Articles 39 and 63(6) | `tr.btk.encryption-regulation`: the regulation on coded or encrypted communication | mevzuat.gov.tr, regulation 14387 (23 October 2010, No. 27738) |
| `tr.asset-freeze`: Law No. 6415 | `tr.7262.proliferation-freeze`: Law No. 7262 | mevzuat.gov.tr, law 7262 (31 December 2020, No. 31351) |
| `tr.5809.authorisation`: Law No. 5809 Articles 9 and 63(1) | `tr.btk.authorisation-regulation`: the BTK authorisation regulation | mevzuat.gov.tr, regulation 13078 (28 May 2009, No. 27241) |
| `tr.6112.internet-broadcasting`: Law No. 6112 Article 29/A | `tr.rtuk.internet-broadcasting-regulation`: the regulation on broadcasts on the internet | mevzuat.gov.tr, regulation 32695 (1 August 2019, No. 30849) |
| `tr.6563.etbis`: Law No. 6563 Article 11(4) | `tr.etbis.registration`: the ETBIS communiqué, which sets the registration | mevzuat.gov.tr, communiqué 23842 (11 August 2017, No. 30151) |
| `tr.6502.distance-contracts`: Law No. 6502 Article 48 | `tr.6502.distance-contracts-regulation`: the Regulation on Distance Contracts | mevzuat.gov.tr, regulation 20237 (27 November 2014, No. 29188; in force three months later) |
| `tr.turkpatent.trademarks`: Law No. 6769 Articles 7(1), 11 and 23 | `eu.euipo.trademarks`: Regulation (EU) 2017/1001 | EUR-Lex |
| `tr.7194.digital-service-tax`: Law No. 7194 Articles 1-7 | `tr.7194.rate-decision`: Presidential Decision No. 10767 | the Official Gazette of 25 December 2025, No. 33118 (`20251225-21.pdf`) |
| `tr.213.e-archive`: Tax Procedure Law No. 213 (primary moved from gib.gov.tr's home page to the law) | `tr.vuk-509.e-archive`: General Communiqué No. 509 | mevzuat.gov.tr, communiqué 33905 (19 October 2019, No. 30923) |
| `gb.uk-gdpr`: the UK GDPR (primary moved to `eur/2016/679`) | `gb.dpa-2018`: the Data Protection Act 2018 | legislation.gov.uk |
| `gb.osa`: the Online Safety Act 2023 | `gb.ofcom-codes`: Ofcom's codes of practice | ofcom.org.uk |
| `gb.psti`: the PSTI Act 2022, Part 1 | `gb.psti-regulations`: SI 2023/1007, Schedule 1 | legislation.gov.uk |
| `cn.commercial-cryptography`: the Commercial Cryptography Administration Regulations (primary moved to oscca.gov.cn) | `cn.cryptography-law`: the Cryptography Law, Article 28 | npc.gov.cn |

Each primary on mevzuat.gov.tr was found by a search restricted to that host and checked against the document the
site serves: its title, and its Official Gazette of publication (the day and issue above) from the document's closing
table or, for a text never amended, the cited page. The decision's address is the gazette's own index of
25 December 2025. A new entry is not read: its obligation is the one its composite carried,
from the composite's source, `verified` false, until the document is read with `read`. The platform entries
(Steam, Epic) stay single: a store's distribution agreement is a private contract signed in its partner portal, and
the public instrument they cite is the store's documentation.

**Counsel.** Each question of the open review `counsel.2026-11-01` now asks about the instrument that carries it: the
regulation's questions moved to the new entries (the Regulation on Distance Contracts' withdrawal waiver, the By-Law's
standard contract and notification, the BTK regulations' permit and authorisation type, the RTÜK licence, Decision
No. 10767's rate, Communiqué No. 509's e-archive duties, the Communiqué on the obligation to inform, Ofcom's codes, the
Chinese regulations), and the laws keep the question each law asks (whether a feature is an electronic communication
service, whether the store is an intermediary under Article 48(6), the Cryptography Law's Article 28). 60 open
questions in one review; none blocks a row.

**Screening.** `tr.7262.proliferation-freeze` is screened like `tr.asset-freeze` (the screening tests are its active
check), so `sanctions_lists.pending` names both, each with its reason: MASAK publishes both regimes' decisions through
a web application with no list file at a stable address.

<a id="reading"></a>
## What was read

The nine law halves were read in the first read's fetch (the versions are the reader's, the SHA-256 the fetch's): Law
No. 6698 at `2024-03-02 (7499)`, No. 5809 at `2026-07-24 (7590)` (two entries), No. 6415 at `2020-12-27 (7262)`, No.
6112 at `2023-12-27 (7491)`, No. 6563 at `2024-10-24 (7529)`, No. 6502 at `2026-06-24 (7587)`, No. 6769 at
`2020-07-22 (7251)` and No. 7194 at `2023-05-18 (Anayasa Mahkemesi)`. The batch 17 [reading](2026-11-01-comply-0-mevzuat-reading.md#reading)
covers most of their articles; what this batch read again, and what the obligations now say:

- **Law No. 6698 Article 9** (as amended by Law No. 7499): a processing condition of Articles 5-6 and an adequacy
  decision (9(1)); without one, an appropriate safeguard while the data subject can exercise their rights and seek
  remedies in the destination (9(4): an agreement between public bodies, binding corporate rules approved by the Board,
  a standard contract the Board announced, or a written undertaking the Board permits); a standard contract is notified
  to the Authority within five business days of its signature (9(5)); else only an incidental transfer in a case 9(6)
  lists. "Used unmodified" is the By-Law's, so it moved there.
- **Law No. 6502 Article 48**: the pre-contract information the regulation sets and the payment-duty warning, the
  seller's burden of proof (48(2)); performance within the promised time, at most thirty days for goods (48(3), Law
  No. 7392); the fourteen-day withdrawal right, up to a year when its information was missing (48(4)); the
  intermediary's request system (48(5)) and shared liability for the pre-contract information and records (48(6)). The
  pre-contract list and the digital-content waiver are the regulation's and moved there.
- **Law No. 6563 Article 11(4)** (Law No. 7061): anyone reports the information the Ministry requests to the system it
  established. Registration in ETBIS is the communiqué's, so the law half is an obligation and the registration a user
  action of the new entry.
- **Law No. 5809 Article 9**: authorisation by notification, or a usage right where numbers, frequencies or satellite
  positions are needed (9(2)-(3)), before operating; Article 39: outside the radio systems reserved to the listed
  public bodies, the BTK sets the procedure for coded or encrypted communication within an electronic communication
  service.
- **Laws No. 6415, 6112, 6769 and 7194**: their articles are batch 17's; the obligations lost only the clauses of the
  second instrument (Law No. 7262, the joint regulation, the EUIPO filing, Decision No. 10767's rates).

`read` recorded the nine entries from the fetch, all or nothing, with this section as their verified source; the guard
then held each one's source to this note.

<a id="by-law-reader"></a>
## A reader for by-laws, regulations and communiqués

The reader of mevzuat.gov.tr knew a law by the number its text names. A by-law, regulation or communiqué names no
number, and the site's `MevzuatNo` for it is an opaque record number; what identifies it is the Official Gazette that
published it. The pages this batch fetched (ten by-laws, regulations and communiqués, four of their cited pages) show
where that is written: the cited page always names it (`Resmî Gazete Tarihi: 27.11.2014 Resmî Gazete Sayısı: 29188`),
and an amended text closes with a table of its own gazette and the amending ones; a text never amended (the By-Law on
transfers abroad, the BTK personal data regulation) has neither notes nor table.

- **Resolver.** A `MevzuatTur=7` (by-law or regulation) or `9` (communiqué) primary resolves to an index, the cited
  page itself (reader `mevzuat-gazette`), and the framed text with its `form` (`YÖNETMELİ`, `TEBLİĞ`). Another kind is
  read by digest alone until its shape is known; a law resolves as before.
- **Index.** `read_mevzuat_gazette` reads exactly one gazette from the cited page: `<day>/<issue>`, `-m<n>` for a
  repeated issue (`33124 (5.Mükerrer)`), an impossible day refused.
- **Document.** The title must name the form; an amended text's table must repeat the cited page's gazette (a
  different one is refused: the site served another instrument); the version is the gazette and the latest change of
  the amending gazettes' notes (`(Değişik ibare:RG-23/8/2022-31932)`), the table's rows (empty trailing rows skipped)
  and court annulments, as `2014-11-27/29188 at 2026-05-06 (Danıştay)`, or `2014-11-27/29188 as published`.
- **Courts, laws included.** A Council of State annulment (`(Danıştay Onuncu Dairesinin 6/5/2026 tarihli ... iptal
  bent: <the text annulled>)`) was not a note the reader knew, and an annulment's quoted text may hold other days, so a
  court's note counts on its decision's day only, for laws too; "Tarihli" is capitalised in some (Law No. 5378).

On the fetched pages the readers give: the Regulation on Distance Contracts `2014-11-27/29188 at 2026-05-06
(Danıştay)` (the Council of State annulled three exceptions to the withdrawal right in Article 15(1) on 6 May 2026), Communiqué No. 509
`2019-10-19/30923 at 2025-12-31 (RG 33124-m5)`, the RTÜK regulation `2019-08-01/30849 at 2026-05-07 (RG 33246)`, the
By-Law on transfers abroad `2024-07-10/32598 as published`. Every law the first read fetched (16, and Laws No. 7262
and 213) reads the same version as before the change.

## What changed

- `cmake/legal-register.json`: 121 entries (15 new), the nine law halves recorded, the counsel questions following
  their instruments, the proliferation freezes pending.
- `scripts/register-review.py`: the resolver, the cited-gazette index reader and the by-law, regulation and communiqué
  reading of `read_mevzuat`; court notes counted on their decision's day. `docs/design/registers.md`'s official-texts
  table has the row.
- `assets/comply/tr.rules.toml` (generation 6), `eu.rules.toml` and `gb.rules.toml` (generation 5): regenerated and
  reviewed, their `rule_generations` records and the generated-source manifest refreshed.

## Verification

- **The register and generations.** `check-registers.py` passes (121 entries, 107 pinned, 60 open counsel questions);
  `gen-comply-rules.py --check` finds all 11 generations current after the TR, EU and GB reviews; `check-generated.py`
  passes with the refreshed manifest.
- **Python.** `Registers`, `LegalRegister`, `ComplyRules`, `CryptoInventory`, `SanctionsLists`, `RegisterReview` and
  `RegisterIds` pass: 66 cases, 2 of them new (the readers and resolver with every refusal; plan, fetch, compare and
  `read` of a regulation and a communiqué end to end, the communiqué whose table contradicts its cited page refused
  and nothing written). Twelve mutations of the new reader were each caught (the table's gazette unchecked, court notes
  counted whole, courts ignored, the table's rows ignored, the form unchecked, the repeated issue dropped, no cited
  gazette accepted, the index dropped, two cited gazettes accepted, the `RG` label dropped, an unknown kind read,
  "Tarihli" missed); the first run let "court notes counted whole" survive, so a case now quotes a later day in an
  annulment note.
- **C++.** On the track's machine (linux-gcc-debug, 4f5b6995) `crd-comply-tests`, `crd-ceridc-tests` and `ceridc`
  built with no warning and the 158 scoped CTests passed, among them the committed generations decoding and binding
  the crash pool, `crd-comply-rules-drift`, `crd-check-registers`, `crd-generated-sources` and `crd-repository-tools`.

## Next

1. COMPLY.0: `plan`, `fetch`, `compare --apply` and the reading of the 15 new entries' instruments: the twelve on
   mevzuat.gov.tr through the new reader, Regulation (EU) 2017/1001 through CELLAR, SI 2023/1007 and the UK GDPR on
   legislation.gov.uk (the resolver gains `eur/` once its `data.xml` is checked), and an Official Gazette reader for
   Decision No. 10767 (`eskiler/YYYY/MM/YYYYMMDD-N`, its PDF and decision number); `tr.kvkk.deletion`'s primary moved
   from the gazette's day index to the regulation.
2. COMPLY.0, host by host: the CELLAR acts not yet read, the UK acts, COPPA, Section 508, the CVAA, OFAC's chapter V
   and the Wassenaar Category 5 Part 2; the full re-research of Law No. 5651 (due 2026-11-26).
3. COMPLY.1: Law No. 7262 read, then both freeze regimes' decisions from the Official Gazette and MASAK.
