# COMPLY.0: the legal register's first read of the official texts

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.0](../ROADMAP.md#slice-comply.0) and [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C,
batch 15; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts). Report of the run:
[2026-11-01-legal-register-first-read.md](2026-11-01-legal-register-first-read.md).

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE".

This batch is the first read of the official legal texts and the gazette reading that decision approved. Every fetch
came from the official source; the bodies stay outside the repository (`~/.cerid/downloads/legal-2026-11-01/` and
`~/.cerid/downloads/gazette-2026-11-01/`), and the report pins each one by URL, size and SHA-256.

## CI first

No failure comes from a track C change that main has not already fixed.

- **Run 41247045494 (67977fda), the operator's complete run.** Its nine failed Windows jobs all show the MSVC C2797 in
  COMPLY.1's screen test, which d9be0cfd fixed. linux-gcc-debug and linux-clang-tsan, the first hosted run of the
  concurrent comply tests, passed. linux-gcc-asan is still running.
- **Run 41254049879 (31404ec4), the complete run batch 12 dispatched.** No job has failed. win-vs, win-clang-cl,
  win-clang-cl-shipping, win-arm64-debug, linux-gcc-arm64-debug, linux-clang-arm64-debug and both Repository checks
  jobs passed; the rest are running or queued.
- Every push run since is cancelled by the next push before its jobs start.

## What changed

`scripts/register-review.py` reads the official texts the way each host serves them (the
[contract](../design/registers.md#official-texts) has the table):

- **Resolvers.** The plan resolves a cited `primary` to the document the host serves as data: mevzuat.gov.tr's framed
  consolidated text, CELLAR's English XHTML for an EUR-Lex ELI (eur-lex.europa.eu answers a machine with a challenge),
  legislation.gov.uk's `data.xml`, the eCFR versioner's full XML and the Wassenaar list's PDF. Each source names its
  reader and the act it expects; a host without a resolver is still read as the cited page, by digest alone.
- **Version indexes.** Where a document's bytes cannot show a later amendment, an index is fetched first and the
  document's URL holds the version it names: CELLAR's SPARQL endpoint lists the act's consolidated versions that have
  an English XHTML text (bound to the act's CELEX number it answers in 0.3 s; the first form, matching CELEX prefixes,
  took 40 to 120 s and timed out), the eCFR versioner gives the hierarchy's latest amendment day, and the Wassenaar
  page links the current list.
- **Readers.** Each checks the document is the act cited and finds its version: the latest day of mevzuat.gov.tr's
  amendment notes with the amending act (every note shape the read found: spaced and en dashes, several notes in one
  parenthesis, "Aynen kabul", `KHK/` numbers, "Yeniden düzenleme", bare notes and Constitutional Court annulments),
  `dct:valid`, the consolidated CELEX number, the eCFR amendment day or the list's upload path.
- **The text read.** The eCFR versioner ignores subtitle, chapter and subchapter and serves the whole title above a
  part, so the digest of an eCFR instrument is its hierarchy's subtree: an amendment elsewhere in title 15 does not
  read as a change to the EAR.
- **Fetching.** gzip is requested and decoded (eCFR refuses a request without it), bounded at 16 MiB both ways; another
  encoding, a broken or truncated gzip body and a version that could leave the URL (`..`) are refused. A redirect is
  followed on the same host only, http upgraded to https (CELLAR's Location is http); the refused redirect closes its
  response. Error messages are folded onto one line.
- **Judging.** A scan reports a read instrument amended (a new version) separately from a changed text under the same
  version.
- **`read`.** It records the instruments a person read from the fetch the applied run compared: the reader's version,
  the fetch day and the SHA-256 of the text read, with the source verified and cited by `--ref`. An entry whose
  document was not read, is unreadable or has no reader is refused, and a refusal writes nothing.

<a id="first-read"></a>
## The first read

`plan`, `fetch` and `compare --apply` ran on the legal register (99 sources, 86 read). The run is recorded as a light
scan of 2026-11-01 (report above).

- **Stable digests.** The plan was fetched twice. Every resolved document (21 mevzuat.gov.tr laws, 14 CELLAR acts, 5
  UK acts, 5 eCFR hierarchies and the Wassenaar list) had the same SHA-256 both times. The pages that differed are
  indexes (a SPARQL result's order, the Wassenaar page) and platform HTML pages read by digest alone (Android, Apple,
  Google Play, Steamworks, ESRB, the Australian register), which need readers of their own before they can be pinned.
- **Not read (13).** cac.gov.cn (a certificate chain the host does not complete), npc.gov.cn (TLS handshake failure),
  planalto.gov.br twice and ticaret.gov.tr (connection reset), le.utah.gov and ico.org.uk (timeouts), cisa.gov,
  leginfo.legislature.ca.gov and store.epicgames.com (403), uscode.house.gov (404: the cited granule URL is wrong), and
  two moved documents refused as cross-host redirects: e-Gov Japan (now laws.e-gov.go.jp) and the UN list (now
  main.un.org). Their primaries are corrected, and their hosts given readers, before they are read.

<a id="reading"></a>
## What was read

Each entry below had its cited articles read in the fetched text against its obligation. Corrections were applied to
the register before the run; `read` then recorded the version and digest.

| Instrument (version read) | Entries recorded | What the text says, and what changed |
|---|---|---|
| Law No. 6698, KVKK (`2024-03-02 (7499)`) | `tr.kvkk.processing`, `tr.kvkk.notice-and-consent`, `tr.kvkk.rights`, `tr.kvkk.security-and-breach`, `tr.kvkk.verbis` | Article 4's principles, Article 5's explicit consent or listed grounds, Article 6 as amended by Law No. 7499, Article 7's deletion, destruction or anonymisation, Article 10's notice items, Article 11's rights, Article 12(5)'s breach notice "en kısa sürede" to the data subject and the Board, Article 16(2)'s registry before processing starts. Article 13(2): within 30 days at the latest and free of charge, but a fee on the Board's tariff where answering has its own cost; `tr.kvkk.rights` now says so. The notice-and-consent rule that the notice is separate from the consent request comes from the Communiqué on the obligation to inform, which this run did not read. |
| Law No. 7545, Cybersecurity (`2026-07-24 (7590)`) | `tr.cybersecurity.reporting`, `tr.cybersecurity.products` | Article 7(1)(b): report detected vulnerabilities and cyber incidents to the Presidency without delay; 7(1)(a): hand over requested data; Article 8: audits. Article 7(1)(ç): approval before a certification-subject company operates; Article 18: export approval for permit-subject products under the Presidency's procedures, and a merger, division or share transfer of a company producing them is notified (approval where control changes). Articles corrected to "Articles 7 and 8" and "Articles 7(1)(ç) and 18"; the products obligation gained the merger and transfer duty. |
| Regulation (EU) 2016/679, GDPR (`02016R0679-20160504`) | all six `eu.gdpr.*` entries | Articles 5, 6, 9, 25 and 30; Article 8's age 16 with member-state ages down to 13; Article 12(3)'s one month (extendable by two); Article 33's 72 hours; Article 35; Articles 44-49. |
| Directive 2002/58/EC, ePrivacy (`02002L0058-20091219`) | `eu.eprivacy.device-storage` | Article 5(3): storage or access only with consent after clear information, except for transmission or what is strictly necessary for a service the user asked for. |
| Regulation (EU) 2024/2847, CRA (`02024R2847-20241120`) | `eu.cra.reporting`, `eu.cra.essential-requirements`, `eu.cra.reporting-contact` | Article 14(2) and (4): early warning within 24 hours, notification within 72 hours, final report 14 days after a corrective measure for a vulnerability and one month after the notification for an incident, simultaneously to the coordinating CSIRT and ENISA through the single reporting platform; Article 13 and Annex I. Article 71(2): the regulation applies from 11 December 2027, Article 14 from 11 September 2026. The single point of contact is Article 13(17), which applies from 11 December 2027, not with Article 14; `eu.cra.reporting-contact` now says so, and `eu.cra.reporting` names both final-report periods. |
| Regulation (EU) 2021/821, dual use (`02021R0821-20260921`) | `eu.dual-use` | Annex I Category 5 Part 2 (5A002, 5D002) and the Cryptography Note in the consolidated text in force since 21 September 2026. |
| EAR, 15 CFR chapter VII subchapter C (`2026-10-26`) | `us.ear.encryption`, `us.ear.self-classification-report` | § 740.17 (License Exception ENC), 5D002 and 5D992 in the CCL. § 740.17(e)(3): the self-classification report (Supplement No. 8 to Part 742) is due by 1 February for the previous calendar year's exports, and it covers items exported under § 740.17(b)(1) that stay in 5A002, 5B002 or 5D002, and mass-market components and executable software in 5A992 or 5D992; mass-market end items are not reported. `us.ear.self-classification-report` said "mass-market encryption items" and is corrected. |

Not recorded, with the reason:

- `tr.kvkk.transfer-abroad`, `tr.btk.encryption` and `tr.5809.authorisation` name secondary instruments (the By-Law on
  transfers abroad, the BTK rules on encryption and on authorisation) beside the law. Article 9 of Law No. 6698 as
  amended was read (adequacy, the appropriate safeguards, the standard contract notified within five business days,
  the incidental transfers); the entries are recorded once their by-laws are read from mevzuat.gov.tr.
- `int.wassenaar`: the 2025 list (with its July 2026 corrigendum) was fetched and pinned, but its Category 5 Part 2 was
  not read in this batch.
- Every other instrument the run fetched (the report lists 92 with their versions and digests) is read in the next
  batches, host by host.

<a id="gazette-reading"></a>
## The gazette reading

The register's first calendar event was "Law No. 5651 game-platform duties take effect" on 2026-11-01, unverified. The
consolidated text of Law No. 5651 (`2026-07-24 (7590)`) lists in its table of amendments that Law No. 7578's
Additional Articles 4 and 5 take effect on 1/11/2026 and says that, until then, the text is in the Official Gazette of
1/5/2026, No. 33240. That gazette's law (`resmigazete.gov.tr/eskiler/2026/05/20260501-1.htm`, 89 448 bytes,
SHA-256 `a7bc07f7f32b424e…`, fetched on 2026-11-01) is Law No. 7578 of 30/4/2026:

- **Article 21** adds the definitions of game, game distributor, game developer and game platform to Article 2.
- **Article 22** amends Additional Article 4 for social network providers: no service to children under 15, with age
  verification, and a separate service for those of 15 and over (7); parental controls over account settings, paid
  transactions and usage time (20); deceptive advertising blocked (21); and duties above ten million daily accesses
  (22, 23).
- **Article 23** adds Additional Article 5 for game platforms: no unrated game (an unrated game only at the highest age
  criterion) and unrated content removed (1); a representative in Turkey for a foreign-based platform above 100 000
  daily accesses (2); parental controls over account settings and paid transactions (3); the Authority's information
  requests answered at once, within at most 15 days (4); a by-law on the duties and the age rating (5); and fines and
  bandwidth throttling (6, 7).
- **Article 27(1)(a)**: Articles 22 and 23 take effect six months after publication, on 1 November 2026. The calendar
  event is verified.

What it changes in the register:

- `tr.5651.game-platforms`, `tr.5651.game-platform-ratings` and `tr.5651.under-15` now cite the additional articles
  and say what they say. The representative duty is for a foreign-based platform only, which the entry had not said;
  `tr.5651.under-15` takes effect on 2026-11-01.
- **Law No. 7590** (24/7/2026, in force on 31/7/2026) moved Law No. 5651's other provisions from the ICTA ("Kurum") to
  the Cybersecurity Presidency ("Başkanlık") and repealed the definition of "Kurum", while Additional Article 5, not
  yet in force then, still says "Kurum". Which authority the game-platform duties and their by-law answer to is
  therefore a question for counsel, added to `tr.5651.game-platforms`'s question in `counsel.2026-11-01`, and the watch
  entry `tr.5651.icta-rules` is no longer sure to be the ICTA's.
- **Triggers.** The gazette reading changed the pinned `tr.5651.game-platforms` and `tr.5651.under-15`, so it is
  recorded as a run with both as triggers, which owes a full re-research of Law No. 5651 as amended by Laws No. 7578 and
  7590 (the guard holds its due day).

## Verification

- **Python.** `RegisterReview` has 17 cases, 5 of them new: the legal domain end to end (resolved plan, the index read
  first, CELLAR's headers, never read, amended and changed text judged apart), `read` (snapshot rule, every refusal
  writing nothing, recorded entries passing the guard, a tampered body refused, the standards register refused),
  `read` of an unreadable or unfetched document, every host's resolver, fetching (index first, gzip decoded and bounded,
  truncated gzip, unrequested encoding, a gzip bomb, a version leaving the URL, unreadable indexes), redirects, and the
  readers with every note shape and refusal (eCFR subtree and the digest of a reading). `Registers`, `LegalRegister`,
  `ComplyRules`, `CryptoInventory` and `RegisterIds` pass: 53 cases.
- **Mutations: 28, each caught.** Index ordering, the gzip request, the gzip end and size checks, the unrequested
  encoding, the version pattern, the redirect host and upgrade, the closed redirect response, the law-number check,
  the spaced-dash note, the court label, the in-force day, the act fallback, the eCFR hierarchy echo and subtree (three),
  the legislation identifier, the amended judgment, the snapshot rule, the reader rule, the all-or-nothing write, the
  verified flag, the ELI sector map, the one-link rule, the PDF check and the English header.
- **The register.** The guard passes. Every jurisdiction's rule generation was reviewed as a new one on 2026-11-01:
  the register's snapshot day is in each file, and the TR, EU and US rules changed (their `read` flags, articles and
  obligations). The generated-source manifest was refreshed for the eleven files.
- **C++.** On the track's machine (linux-gcc-debug, 0920905e), `crd-comply-tests`, `crd-ceridc-tests` and `ceridc`
  built with no warning, and 155 scoped CTests passed, among them the committed generations decoding and binding the
  crash pool, the `ceridc comply` verb with the real binary, `crd-comply-rules-drift`, `crd-check-registers`,
  `crd-generated-sources` and `crd-repository-tools`. The machine ran 0.11 h and is stopped.
- **Validators.** `check-registers.py`, `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py` and
  `check-generated.py` pass. The whole `test-repository-tools.py` passes except `test_generated_target_folders`, which
  needs Ninja and fails the same way without this change on this PC, where nothing is built.

## Next

1. Read the remaining fetched instruments host by host: the other mevzuat.gov.tr laws (5237, 5651 after its full
   re-research, 6415, 6112, 6563, 6502, 5070, 5846, 6769, 5378, 3065, 7194, 6493, 5809 with the BTK rules), the other
   CELLAR acts, the UK acts, COPPA, Section 508, the CVAA, OFAC's chapter V and the Wassenaar Category 5 Part 2.
2. The full re-research the gazette run owes: Law No. 5651 as amended by Laws No. 7578 and 7590.
3. Readers for the platform pages and the hosts not read; correct the moved and wrong primaries (e-Gov Japan, the UN
   list, uscode) and the 15 home pages; the secondary instruments (the KVKK By-Law on transfers abroad and the
   Communiqué on the obligation to inform, the BTK rules).
4. COMPLY.1's official sanctions lists and their importers.
