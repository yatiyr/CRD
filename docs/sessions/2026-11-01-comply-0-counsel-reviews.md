# COMPLY.0: counsel reviews, so counsel never blocks a row

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.0](../ROADMAP.md#slice-comply.0) and [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C,
batch 14; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[counsel reviews](../design/registers.md#counsel-reviews).

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE".

Counsel review never holds a row. Every item that needs counsel is recorded as a dated, named review entry in the legal
register, with the exact question for counsel. A row whose engineering clauses are all proven closes Done, and its
counsel review stays open in the register. No part of the register claims legal sign-off.

## CI first

No failure in this batch comes from a track C change that main has not already fixed.

- **Run 41247045494 (67977fda), the operator's complete run.** It is still in progress. win-shipping, win-shipping-profile
  and win-tidy now fail as well, which makes nine failed Windows jobs. Each log shows the same MSVC C2797 in
  `tests/tools/ceridc/test_ceridc_screen.cpp`, lines 104 to 107: COMPLY.1's screen test with its defaulted
  `std::initializer_list` members. d9be0cfd on main fixes it.
- **Still running in that run:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan. These are the first hosted run of
  the concurrent comply tests.
- **Run 41254049879 (31404ec4).** This is the complete run batch 12 dispatched, after d9be0cfd. About half of its jobs
  are running and the rest are queued. win-vs, linux-gcc-arm64-debug and both Repository checks jobs have passed, and
  no job has failed.

## What changed

The register's counsel rule was "counsel precedes implementation": the guard refused an implemented entry whose counsel
review was outstanding. That rule held work on the user's lawyer, which the decision rules out. It is replaced by a
record of what counsel is asked.

- **`counsel_reviews`** (in `cmake/legal-register.json`) lists dated, named reviews. Each has an id, a title, the day it
  was opened, the day counsel answered (null while it is open) and its questions. A question names one entry or one
  export's head classification record and asks the exact question, ending with "?".
- **The first review.** `counsel.2026-11-01` is open. It asks 50 questions:
  - one for each of the 42 entries whose counsel is needed: every Turkish instrument that needs interpretation, the
    GDPR, NIS2, the PLD, the DSA, the EECC, the AVMSD, UK GDPR, the OSA, the EAR, CCPA, the ADA, ECA Digital and the two
    Chinese entries;
  - one for each of the eight export classifications. Each asks whether the classification is right under Law
    No. 7545, the BTK rules, Annex I of Regulation (EU) 2021/821, the Wassenaar Dual-Use List and the EAR.
    cerid-package's question names its authentication-only cryptography, and the sandbox's names its isolation of
    untrusted code.

  Each question is specific to the publisher's facts: a Turkish company, its products' data flows, the store and the
  cloud regions.
- **The guard** (`scripts/check-registers.py`, the `crd-check-registers` CTest and repository step) holds the reviews to
  the items' `counsel` fields. It fails:
  - an outstanding item (counsel needed and not reviewed) that no open review asks about;
  - an item asked about in two open reviews;
  - an open question about an item whose counsel is not outstanding;
  - an item's `counsel.reviewed` day that is not the `answered` day of a review that asked about it;
  - dates out of order or after today, duplicate ids, a question asked twice in one review, an unknown item and a
    malformed review.

  The refusal of an implemented entry whose counsel is outstanding is gone. The summary line now counts the open
  questions ("open counsel questions: 50 in 1 review").
- **Kept:** `gen-crypto-inventory.py --release` and the guard still refuse to freeze a classification for a release
  before counsel answers. A release is the publisher's act, not a clause of any row, so it blocks no work. The user can
  overrule this too.
- **The contract** ([registers.md](../design/registers.md#counsel-reviews)) has a new section on counsel reviews. The
  legal domain's rule now reads "Counsel never blocks, and is never guessed", and the `counsel` field and `--review`
  point to the reviews.
- **The rows.** COMPLY.0 records the reviews. COMPLY.0's and COMPLY.1's remaining items name counsel's answers as open
  register questions, not clauses. Both rows record the approved downloads (the official texts, the gazette reading and
  the sanctions lists) as approved.

The rule generations (`assets/comply/*.rules.toml`) do not change. Their `counsel` field (none, needed or reviewed)
already reports an outstanding review in every `ceridc comply` answer, so no rule claims sign-off.

## Verification

- **Python.** The suites pass: `Registers`, `LegalRegister`, `ComplyRules`, `CryptoInventory`, `RegisterReview` and
  `RegisterIds`, 47 tests.
  - The fixture writes consistent reviews for every tree that does not set its own: one answered review per answer day
    and one open review asking about every outstanding item.
  - `test_every_outstanding_counsel_item_is_asked_about_in_one_open_review` has 16 failing shapes, the answered and
    stale cases and the passing ones.
  - `test_reading_precedes_closing_and_counsel_never_blocks_implementing` proves an implemented entry passes while its
    question is open.
  - The classification case proves that a head record's outstanding counsel, and an answer without its review, both
    fail.
- **Mutations: twelve, each caught.** The outstanding-item rule, the stale-question rule, the answered-day rule, the
  "?" rule, the twice-asked rule, the date order, the duplicate id, the classification heads' inclusion, the
  two-open-reviews rule, the unknown-item rule, the outstanding predicate and the id pattern.
- **The real register** passes: 106 entries, 93 pinned, open counsel questions: 50 in 1 review.
- **Validators.** `check-registers.py`, `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py` and
  `check-generated.py` pass.
- **Cloud machine.** Not started: only Python, JSON and docs changed, and no C++ test reads the register's new block.

<a id="host-checks"></a>
## Host checks for the first read

The download is approved. Before any reader is written, one instrument per major host was fetched twice into
`~/.cerid/downloads/probe-2026-11-01/` (outside the repository). Each body was checked for the instrument's text and
the two digests compared. The planned primary URLs are not all readable as they stand:

| Host | Planned URL | What serves the text | Version marker | Stable |
|---|---|---|---|---|
| mevzuat.gov.tr | `mevzuat?MevzuatNo=6698&MevzuatTur=1&MevzuatTertip=5` (71 619 bytes, a script shell that frames the text) | `anasayfa/MevzuatFihristDetayIframe?MevzuatTur=1&MevzuatNo=6698&MevzuatTertip=5` (207 245 bytes, the consolidated KVKK in HTML); `MevzuatMetin/1.5.6698.doc` (260 608 bytes) is the Word copy | The amendment notes in the text, such as "(Değişik:2/3/2024-7499/34 md.)", give the latest amending law and day | Yes: `83115349…5ad5c23ba6` twice |
| eur-lex.europa.eu | `eli/reg/2016/679/oj` | Nothing: HTTP 202 with an empty body (a bot challenge, never bypassed) | — | — |
| publications.europa.eu (CELLAR, the Publications Office) | — | `resource/celex/32016R0679` with `Accept: application/xhtml+xml` and `Accept-Language: eng` redirects to the http form of `resource/cellar/3e485e15-….0006.03/DOC_1` (806 864 bytes). Its https form serves the same bytes, so the fetcher must request https and refuse a downgrade | Consolidated versions are separate CELEX numbers (`02016R0679-<day>`) | Yes: `962539af…1b646b9d` over http and https |
| legislation.gov.uk | `ukpga/2018/12` (5 512 194 bytes of HTML) | `ukpga/2018/12/data.xml` (5 919 263 bytes, CLML) | `dct:valid` 2026-09-30, the point-in-time version | One fetch |
| ecfr.gov | `current/title-15/subtitle-B/chapter-VII/subchapter-C` (HTML) | `api/versioner/v1/versions/title-15.json?part=740` lists 281 section versions, the latest amended on 2026-08-14. `api/versioner/v1/full/2026-08-14/title-15.xml?part=740` holds the dated text, and refuses a request without `Accept-Encoding: gzip` (406) | The amendment day per part; `titles.json` gives each title's `latest_amended_on` | Yes: `9f92d95b…b862fe38` twice |

What follows for the reader:

- **A source URL beside the citation URL.** Each host gets a resolver from the primary URL a person cites to the
  document the machine reads.
- **The fetcher.** It sends `Accept-Encoding: gzip` and decompresses, and it refuses a redirect from https to http.
- **The digest.** Each host's reader digests the instrument's text and takes its version from the host's own marker.
  Fifteen planned URLs are home pages (btk, kvkk, gib, ticaret, tbmm, cac, npc, edpb, ico, esrb, pegi, sanctionsmap,
  capitol.texas, le.utah, commission.europa). These are replaced by the instruments' own documents before they are
  read.

## Next

1. COMPLY.0's first read of the official texts (approved), on the [host checks](#host-checks) above. Check the
   remaining hosts the same way: planalto.gov.br, legislation.gov.au, laws-lois.justice.gc.ca, leginfo, uscode,
   elaws.e-gov.go.jp, resmigazete.gov.tr and the platform policy pages.
2. Replace the 15 home-page URLs with the instruments' own documents.
3. Add a `register-review.py` command that records a reading (`consolidated` version, day and SHA-256 from the fetch
   manifest). Read first what NET.2, CRASH-FIX.1 and the classifications depend on: Laws 6698, 7545 and 5809, the GDPR,
   ePrivacy, the CRA, the EAR, Regulation 2021/821 and Wassenaar.
4. Do the gazette reading.
5. Then COMPLY.1's official sanctions lists and their importers.
