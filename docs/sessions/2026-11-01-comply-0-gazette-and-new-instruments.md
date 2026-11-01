# COMPLY.0: the Official Gazette reader and the new entries' instruments

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.0](../ROADMAP.md#slice-comply.0) and [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C,
batch 19; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts). The fetch is this batch's
([report](2026-11-01-legal-register-new-instruments.md)).

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". This batch's fetch is part of the first read of the official legal texts
that decision approved; every body came from its official host and stays outside the repository
(`~/.cerid/downloads/legal-2026-11-01-c/`), pinned by URL, size and SHA-256 in the report.

## CI first

No failure comes from a track C change that main has not already fixed.

- **Run 41254049879 (31404ec4)**: no new failure. Its four failures are the ones batch 18 recorded (win-tidy:
  `test_arguments.cpp`, fixed by a40af1bf; linux-gcc-release and linux-gcc-shipping: track B's `test_deflate.cpp`,
  rebuilt by 95ec6965; win-relwithdebinfo: the CI orchestrator's own `crd-ci-orchestrator-e2e`, named for its owner).
  linux-gcc-debug, linux-clang-tsan, win-debug and win-asan (shard 1) were still running at 04:02 UTC.
- **Run 41269105413 (b0b1b245)**, the complete run batch 17 dispatched, was still queued at its preflight job; the push
  runs since are cancelled by newer pushes or pending (41271289639, f99dc6b9).

<a id="gazette-reader"></a>
## The Official Gazette reader

A Presidential decision is published in the Official Gazette alone, as a PDF, and never consolidated on
mevzuat.gov.tr. The pages fetched on 2026-11-01 (the days of 28 October 2017, 25 December 2025 and the fifth repeated
issue of 31 December 2025) show its shape:

- **The day page** `eskiler/YYYY/MM/YYYYMMDD.htm` (a repeated issue `YYYYMMDDM5.htm`) is windows-1254, as its meta tag
  declares, headed `25 Aralık 2025 Tarihli ve 33118 Sayılı Resmî Gazete` (`- 5. Mükerrer` on a repeated issue), and
  links each item once (`20251225-21.pdf`, `20251231M5-1.pdf`, `20171028-13.htm`) with its title as the link's text. A
  decision's title ends with its number, `(Karar Sayısı: 10767` (the closing parenthesis missing on this one) or
  `(Kararname Numarası: 191)`.
- **The resolver** turns a cited item (`https://www.resmigazete.gov.tr/eskiler/<year>/<month>/<day>[M<n>]-<item>.pdf`
  or `.htm`) into an index, the day page read for that item alone (`resmigazete-<day>-<item>-day`, so two items of one
  day never share one index), and the item itself (reader `gazette`).
- **The index** decodes the page in its declared charset (none or two refused), finds exactly one heading, whose day
  (Turkish month names) and repeated issue must be the item's, and exactly one link to the item; the version is
  `<day>/<issue>[-m<n>]`, with `/<number>` when the title names one decision number (two refused).
- **The document** is a PDF (`%PDF-`) or an HTML page as its name says, read as `2025-12-25/33118 No. 10767 as
  published`. The PDF's text is CID glyphs (`[<0006>]TJ`) with no stdlib reading, so the machine pins it by digest and
  the gazette's own day page vouches for what it is; the decision was read from the PDF by a person (below).

legislation.gov.uk serves retained EU law the same way as acts: `eur/2016/679/data.xml` answered 200 with
`dc:identifier` `http://www.legislation.gov.uk/eur/2016/679` and `dct:valid` 2026-09-30, holding the 140 provisions, so
the resolver accepts `eur/` (only; `eudr` and `eudn` stay unresolved until checked). `tr.kvkk.deletion`'s primary moved
from the gazette's day page to the regulation on mevzuat.gov.tr (`MevzuatNo=24038&MevzuatTur=7`, found through the
site's own search, published 28 October 2017, No. 30224, which the by-law reader confirms from the cited page).

<a id="reading"></a>
## What was read

`plan`, `fetch` and `compare --apply` ran on the legal register (125 sources, 112 read, the same 13 not read as
before); the run is recorded as a light run with this batch's report. Each instrument below was read in the fetched
text against its entry's obligation, and the obligation, article and counsel question now say what the text says. The
versions are the readers', the SHA-256 the report's.

- **The Communiqué on the obligation to inform** (`2018-03-10/30356 at 2019-04-28 (RG 30758)`, Articles 4-6): the
  minimum contents (Article 4); in every processing whatever its legal basis, again before a new purpose, not on
  request, the controller's burden of proof, separately from explicit consent, a specific purpose without vague
  wording, plain language, the legal basis among Articles 5-6, the transfer purpose and recipient groups, the
  collection method, nothing incomplete or misleading (Article 5); data not obtained from the subject at a reasonable
  time, the first contact or the first transfer (Article 6). "Never make consent a service condition" is not in the
  communiqué and left the obligation.
- **The Regulation on deletion, destruction or anonymisation** (`2017-10-28/30224 at 2019-04-28 (RG 30758)`, in force
  1 January 2018, Articles 5-12): the policy is owed by controllers registering under Article 16, with Article 6's
  contents; destruction at the first periodic destruction (at most every six months), three months without a policy,
  thirty days on request with third parties informed; records kept at least three years (Article 7(3)).
- **The BTK regulation on coded or encrypted communication** (`2010-10-23/27738 at 2018-01-12 (RG 30299)`, Articles
  5-8 and 11): the permit application carries the crypto algorithm and keys and, where needed, the means to decrypt
  (Article 5(2)(ç)); on acceptance the code or crypto is delivered to the BTK (Article 7(1)) and kept in its critical
  data centre (Article 8(2)); no change without approval (Article 7(3)); a local system outside an operator's
  infrastructure needs no permit (Article 7(2)); monthly reports of devices placed on the market (Article 11). The
  entry had said "without key escrow", which is Cerid's design, not the regulation's: the obligation now states the
  regulation's duties, and the counsel question asks whether an end-to-end encrypted product falls within it and may
  be offered in Turkey without delivering keys.
- **Law No. 7262** (`2024-01-18 (Anayasa Mahkemesi)`, in force 31 December 2020, Articles 1-3): no funds or business
  relationship for designated persons, those they control or those acting for them (Article 2(1)); freezes,
  prohibitions and liftings applied without delay from the President's decision in the Official Gazette (Article
  3(1)-(2)); final lists published by the public bodies (Article 3(3)); execution under Law No. 6415 (Article 3(4)).
  This is COMPLY.1's "Law No. 7262 read".
- **The ETBIS communiqué** (`2017-08-11/30151 at 2019-02-15 (RG 30687)`, Articles 5-8): registration before trading is
  the store's own (an intermediary, or a provider on its own platform); "and its sellers" was wrong (a seller on the
  store registers itself only as Article 5(1) says). The thirty-day notifications, including the country and address
  of the databases holding personal and customer data (Article 6(1)(ı)), the March cross-border volume and the
  intermediary's monthly statistics are now in the obligation.
- **The By-Law on transfers abroad** (`2024-07-10/32598 as published`, in force on publication): Article 13 is binding
  corporate rules; the standard contract is Article 14 alone, so the entry cites Article 14: unmodified (Turkish text
  prevails), signed by authorised signatories, notified within five business days of the last signature by the
  exporter unless the contract names the party, with proof of authority and notarised translations; changes and the
  end notified the same way.
- **The Regulation on Distance Contracts** (`2014-11-27/29188 at 2026-05-06 (Danıştay)`, in force 27 February 2015,
  Articles 5-9 and 15): Article 5(1)'s items, 12-point plain text on a durable medium (Article 6(1)), the four items
  before payment (Article 6(2)), confirmation or no contract (Article 7), the payment warning or not bound (Article
  8(1)), the platform jointly liable (Articles 5(7), 6(6)). Digital content delivered at once is not a waiver the
  consumer signs: Article 15(1)(ğ) excludes the withdrawal right for intangible goods delivered at once, and Article
  5(1)(h) requires saying so; the counsel question now asks whether a download is such a good.
- **Presidential Decision No. 10767** (`2025-12-25/33118 No. 10767 as published`, read from the PDF): under Article
  5(5) of Law No. 7194, the Article 5(3) rate is 5 % from 1 January 2026 and 2.5 % from 1 January 2027 (Article 1), in
  force on publication for revenue from the next new year (Article 2). The obligation was right; the counsel question
  now asks whether the store is a digital service provider under the law's thresholds.
- **The PSTI Regulations 2023** (`2025-12-04`, in force 29 April 2024, Schedule 1): passwords unique per product
  (with what that excludes) or user-defined (paragraph 1); a published point of contact with acknowledgement and status
  update times (paragraph 2); the defined support period published plainly, beside the price on the manufacturer's own
  invitation to purchase, extended but never shortened (paragraph 3); all in English, free and without personal data.
- **The BTK authorisation regulation** (`2009-05-28/27241 at 2021-05-01 (RG 31471)`, Articles 5-8 and 22): a company
  under Turkish law founded for the activity, its conditions (Article 7), notification before starting and service once
  the BTK confirms (Article 8), a usage right where numbers, frequencies or satellite positions are needed (Article
  5(3)), an unauthorised service shut down (Article 22), private own-premises networks exempt (Article 6).
- **The RTÜK regulation on broadcasts on the internet** (`2019-08-01/30849 at 2026-05-07 (RG 33246)`, Articles 2,
  5-8, 16-17 and 21): individual communication, platforms not dedicated to transmitting broadcasts and hosting
  providers are outside it (Article 2(2)); an online-only media service provider needs an INTERNET-RD, -TV or -İBYH
  licence (a joint-stock company, ten years) and a platform transmitting such services a transmission authorisation;
  their duties (Articles 16-17) and parental controls (Article 21).

`read` recorded these eleven entries from this fetch, all or nothing, with this section as their verified source.

**Not read this batch** (fetched and pinned, their entries still unverified): General Communiqué No. 509 and Tax
Procedure Law No. 213 (the e-archive and invoice provisions), Regulation (EU) 2017/1001 (CELLAR `02017R1001-20251201`),
the UK GDPR (`eur/2016/679`) and the Data Protection Act 2018. Ofcom's codes and the Chinese Cryptography Law stay on
home pages their hosts refuse a machine (ofcom.org.uk 403, npc.gov.cn a TLS handshake failure); each needs its own
document's address.

## Rule generations

The TR and GB obligations changed, so their generations were reviewed (`gen-comply-rules.py --review TR` and `GB`) and
the generated-source manifest refreshed; every other jurisdiction's generation is unchanged.

## Verification

- **Python.** `RegisterReview`, `Registers` and `LegalRegister` pass (40 cases, 2 of them new: the gazette resolver,
  day-page reader and document reader with every refusal; plan, fetch, compare and `read` of a decision end to end,
  two items of one day each read for itself, and a refused read writing nothing). Twelve mutations of the new code were
  each caught: the day, the repeated issue, the link count, the decision-number count, the charset count, the heading
  count, the month, the PDF check, the missing version, the number dropped from the version, a shared index for one
  day, and `eur/` dropped from the resolver. The first draft of the resolver gave all items of one day one index,
  which read every item with the first item's version; the end-to-end case found it.
- **On the real pages.** The reader gives `2025-12-25/33118/10767` for Decision No. 10767, `2025-12-31/33124-m5/10795`
  for the first item of the repeated issue of 31 December 2025, and `2017-10-28/30224` for the KVKK regulation's item.
- **Guards.** `check-registers.py`, `gen-comply-rules.py --check` and `check-generated.py` pass after the reviews.
