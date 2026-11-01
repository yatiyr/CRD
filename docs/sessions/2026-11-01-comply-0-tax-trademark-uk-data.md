# COMPLY.0: the e-archive communiqué, the Tax Procedure Law, the EU trade mark and the UK data protection texts

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.0](../ROADMAP.md#slice-comply.0) (track C, batch 20; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts). The texts are the fetch batch 19 pinned
([report](2026-11-01-legal-register-new-instruments.md)); nothing new was downloaded for them.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The five texts below are part of the first read of the official legal texts
that decision approved; their bodies stay outside the repository (`~/.cerid/downloads/legal-2026-11-01-c/`), pinned by
URL, size and SHA-256 in the report.

## CI first

No failure comes from a track C change.

- **Run 41254049879 (31404ec4)**: a fifth failure since batch 19's note, win-asan shard 1: `crd-perf-tests`, `sample
  ring: a copy never straddles a clear` (`tests/foundation/perf/test_diag_sample_ring_readers.cpp:202`,
  `samples_seen == 0`). The test's own comment calls a zero a test-power failure: on a loaded ASan runner the four
  copier threads saw no live batch before the producer's 3000 rounds ended. The file is DIAG's (unchanged since
  2026-10-08), so it is named for its owner, track A. The others are the ones recorded before (win-tidy fixed by
  a40af1bf, the LTO links by 95ec6965, win-relwithdebinfo's `crd-ci-orchestrator-e2e` for the orchestrator's owner);
  linux-clang-tsan was still running at 04:30 UTC.
- **Run 41269105413 (b0b1b245)**, the complete run batch 17 dispatched, started at last: win-arm64-debug failed to
  build `engine/foundation/containers/src/checksum.cpp:308`, MSVC C4324 (warning as error) on `CheckStream`, whose
  `alignas(64)` 1026-byte array is padded to 1088. That is HW-RESILIENCE.3's known-answer check (8332bf9f, track A),
  still unchanged on main, so it is named for its owner.
- b0b1b245 predates batch 19, and every push run since (41273181833 for 092a2726 among them) was cancelled by a newer
  push, so the gazette reader, the `eur/` resolver and the `project_sync` fix still have no hosted run. This batch
  dispatched complete-tier run 41274380605 at 04:46 UTC on de5e10b0, the mirror's head: the first hosted run of those
  and of this batch's three rule generations (the communiqué's quoted statement a new string shape for the decoder).
  Its win-arm64-debug lane will fail on the `checksum.cpp` C4324 above until track A repairs it.

## The gate of NET.1

NET.1, the first row of this scope, waits only on the table order: on 2026-11-01 SIMD-PERF, HW-RESILIENCE, DIAG and REPO
are Open, MCP-ADAPT.0 Partial and HESAP-JOBS-RETUNE Open, so `check-master-plan.py` keeps NET.1, NET-STD, COMPLY.0 and
COMPLY.1 Partial whatever their own evidence.

## The readers on these hosts

`read` had recorded mevzuat.gov.tr laws of the fifth series and CELLAR acts before, but no legislation.gov.uk document
and no law of the fourth series. Each reader was run on its body before the reading: Law No. 213 (`MevzuatTertip=4`)
gives `2026-06-24 (7587)`, its latest change; Communiqué No. 509 gives `2019-10-19/30923 at 2025-12-31 (RG 33124-m5)`;
Regulation (EU) 2017/1001 gives CELLAR `02017R1001-20251201`; and the UK GDPR (`eur/2016/679`) and the Data Protection
Act 2018 (`ukpga/2018/12`) both give their point-in-time day `2026-09-30` (`dct:valid`). The UK texts were read from
the Crown Legislation Markup provision by provision (by element `id`, commentary left out), so what was read is the
text as amended to that day.

<a id="reading"></a>
## What was read

- **General Communiqué No. 509** (`2019-10-19/30923 at 2025-12-31 (RG 33124-m5)`, sections IV.1.4, IV.2, V.5.2,
  V.6-V.8 and VI): an e-Archive registrant issues e-Invoices to e-Invoice users and e-Archive Invoices to everyone else
  (IV.2.1); a seller over the internet with gross sales of 500 thousand TL in a period from 2022 must join e-Invoice
  (IV.1.4(a)(4)) and e-Archive by the start of the seventh month after that period (IV.2.4.2); since 1 January 2026 a
  taxpayer outside e-Archive issues every invoice as an e-Archive Invoice through the portal or a licensed integrator
  whatever its amount (IV.2.4.3, 1 January 2027 for simple-procedure and business-account taxpayers, from the
  31 December 2025 amendment); an internet sale's invoice carries the web address, payment method and day, dispatch
  day, carrier and "Bu satış internet üzerinden yapılmıştır.", delivered electronically (IV.2.4.5), with the issue time
  in hours and minutes (V.5.2); signed, time-stamped reports to the Administration (V.8); paper only in the V.7
  failures, the Article 353 fine otherwise (V.6); the e-documents kept electronically with their seal or signature
  **inside Turkey**, a secondary archive abroad allowed and a third-party archive only a licensed one (VI). The
  entry's one-line obligation said none of this; the in-Turkey archive binds the store's invoice storage and server
  regions (STORE.3, NET-SERVER).
- **Tax Procedure Law No. 213** (`2026-06-24 (7587)`, in force 1 January 1961 under Article 416, Articles 227,
  229-232, Repeated Article 242, 253 and 256): a document missing a compulsory particular counts as never issued
  (227); the invoice's particulars (230), sequence, signature and the seven-day limit, or never issued (231); who must
  invoice and when a consumer is owed one, above the Article 232 amount (revalued yearly under Repeated Article 414) or
  on request (232); the rules on books and documents bind electronic ones (Repeated 242); five years' retention from
  the next calendar year (253); production on request with every information and password needed to read electronic
  records (256). The entry's `effective` is now the day Article 416 names, `1961-01-01`. Article 256 is a duty an
  encrypted invoice archive meets by keeping its keys producible to the tax authority, which the store's design records
  (STORE.3).
- **Regulation (EU) 2017/1001** (`02017R1001-20251201`, applied from 1 October 2017 under Article 212, Articles 6, 18,
  30-32, 46, 52-53 and 58): a mark exists only by registration (6); the application's contents, the fee within one month
  and the filing date (30-32); opposition within three months of publication by earlier marks' owners (46); genuine use
  within five years or revocation (18, 58(1)(a)); ten-year terms from filing, renewed in the six months before expiry or
  the six after with the late fee (52-53). The entry stays a user action of COMPLY.4 and STORE.8.
- **The UK GDPR** (`2026-09-30`, Articles 3, 5-6, 8, 12-12A, 21, 22A, 27, 32-34, 44A, 45A-46 and 49): a controller
  outside the UK offering goods or services to people in the UK is within it (3(2)) and designates a **UK
  representative** in writing unless its processing is occasional and low-risk (27), which binds the user's Turkish
  company; the Annex 1 recognised legitimate interests (6(1)(ea)); the child's age for consent to an information
  society service is **13** (8(1)); requests within one month of the relevant time, extendable by two months by a
  reasoned notice inside the first month (12-12A); the right to object presented separately at the first
  communication (21(4)); the regulator is now the Information Commission (33); transfers rest on the Secretary of
  State's approval regulations and the "not materially lower" data protection test (44A, 45A-45B, 46(1A) and (6)), not
  on EU-style adequacy. Article 22A (solely automated significant decisions) was read and binds nothing in Cerid's
  flows today, so the entry does not cite it.
- **The Data Protection Act 2018** (`2026-09-30`, sections 1, 4, 9A, 123, 137, 164A and 171): Part 2 supplements the
  UK GDPR (1, 4); a controller takes complaints through an electronic form, acknowledges them within **30 days** and
  reports progress and outcome (164A); the charge payable to the Commission is set by regulations under 137;
  re-identifying de-identified data without the controller's consent is an offence (171), which the crash pool's
  pseudonymisation (CRASH-FIX.2, COMPLY.2) relies on; the age-appropriate design code comes from 123 (`gb.aadc`).
  Section 9A (bases in international law) binds nothing in Cerid's flows, so the entry does not cite it.

`read` recorded these five entries from batch 19's fetch, all or nothing, with this section as their verified source.

## Addresses still missing

- **Ofcom's codes** (`gb.ofcom-codes`): ofcom.org.uk answers the fetcher 403. Ofcom's regulatory documents page lists
  the illegal content codes for user-to-user services issued 24 February 2025 (in force 17 March 2025) as superseded on
  29 September 2026 by a version incorporating the amendments issued 9 September 2026 (in force 30 September 2026),
  and the protection of children codes separately. The entry names two instruments behind one primary, which the
  register's one-instrument-per-entry rule refuses once it is read: it splits into the illegal-harms code and the
  children's code, each citing its own PDF, read through a reader that pins the PDF by digest as the gazette reader
  does, once the host serves the machine.
- **The Chinese Cryptography Law** (`cn.cryptography-law`): www.npc.gov.cn refuses every TLS handshake from this host
  (Python's OpenSSL at every security level and Windows Schannel alike) and answers only plain HTTP, which the register
  refuses for a primary. The National Database of Laws and Regulations (`flk.npc.gov.cn`) answers TLS 1.3, but it is
  a script-rendered application whose documents come from its `/law-search/search/flfgDetails` JSON and whose download
  route carries a captcha, which the fetcher never answers. Its document address is resolved through that JSON route.

## Rule generations

The TR, GB and EU obligations changed, so their generations were reviewed (`gen-comply-rules.py --review TR`, `GB` and
`EU`) and the generated-source manifest refreshed; every other jurisdiction's generation is unchanged.

## Verification

- **Guards.** `check-registers.py`, `gen-comply-rules.py --check`, `check-generated.py`, `check-master-plan.py`,
  `check-repository.py` and `check-allman-braces.py` pass.
- **C++.** On the track's machine (linux-gcc-debug, 3a597b51 before the rebase onto main; 04:41-04:45 UTC)
  `crd-comply-tests`, `crd-ceridc-tests` and `ceridc` built with no warning and the 159 scoped CTests passed, among
  them the committed generations decoding (the communiqué's quoted Turkish statement included), `crd-comply-rules-drift`,
  `crd-check-registers` and `crd-generated-sources`.

## Next

1. COMPLY.0: read the other fetched instruments host by host (the CELLAR acts not yet read, the UK acts, COPPA, Section
   508, the CVAA); split `gb.ofcom-codes` into its two codes on their own PDFs; resolve the Cryptography Law's address
   through the national database's JSON route.
2. COMPLY.1: both freeze regimes' decisions from the Official Gazette and MASAK, then the Wassenaar Category 5 Part 2
   and Turkey's dual-use communiqué.
