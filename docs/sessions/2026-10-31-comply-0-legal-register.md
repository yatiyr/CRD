# COMPLY.0: the legal register on the shared register substrate

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-10-31. Row: [COMPLY.0](../ROADMAP.md#slice-comply.0) (track C, batch 6; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[legal domain](../design/registers.md#legal-domain).

## CI first

No push of this track has a finished hosted run of its own: the GitHub Actions push runs since 3bb87c7f, 9f903661's
(41223673133) included, are cancelled by the next push to main (any track's) before their jobs are scheduled. The
operator's checkpoint run 41215667488 on c2645814 (batches 1 and 2) had, when this batch started, both "Repository
checks" jobs, win-vs, win-arm64-debug, linux-gcc-shipping, linux-gcc-debug-scalar, linux-gcc-relwithdebinfo and the
two arm64 debug lanes green and the remaining lanes running, none failed. crd-net and crd-crypto (batches 3 and 4)
still have no hosted build. No failure is attributed to this track. NET-STD's open item is the user's approval of its
first refresh's download; NET.1 and NET-STD wait on OWN (track B).

## What landed

- **The legal domain of the one guard.** `scripts/check-registers.py` reads `cmake/legal-register.json` beside the
  standards register (same `cerid-register/1` format, review schedule, owners, rows, sources, evidence and governed
  rows). The domain adds per entry the topic, the instrument (name, article, type, jurisdiction, the official primary
  URL and the consolidated version read with its day and SHA-256), the obligation in plain words, the scopes it
  applies to, the check that proves it (active or planned), effective and sunset days and the counsel flag; at the
  register level the jurisdictions with their official hosts and the scopes (products, data flows, channels) with
  the rows that put each in front of people.
- **Its rules.** A binding obligation (pinned, effective, before its sunset, on a live scope) needs an active check;
  a pinned entry past its sunset fails until deprecated; a planned scope with a Done row fails (any row puts it live)
  and a live scope names its evidence; a primary URL is https on one of its jurisdiction's official hosts (a
  lookalike host fails); an entry whose rows are all Done, or which is implemented, has been read in its consolidated
  text, is verified and cites the instrument's own document rather than a home page; an implemented entry has an
  active check and, when counsel is needed, its review day; every topic but the platform policies holds a Turkish
  entry. Effective days may be a month or a year (counted from its first day, as draft posting periods are).
- **The register.** 105 entries (93 pinned, 12 watched; 99 obligations and 6 user actions; 41 flagged for counsel)
  across 15 topics and 14 jurisdictions, Turkey first-class with 36 entries: KVKK 6698 (processing, the aydınlatma
  notice kept apart from explicit consent, rights, security and breach, transfer abroad after Law No. 7499 and the
  2024 By-Law, VERBIS, deletion), the Turkish Penal Code's data and information-system offences, Law No. 7545
  (reporting and product permits), BTK encryption rules under Law No. 5809, the dual-use export rules (watch until
  the instrument is identified), the asset-freeze regime, Law No. 5651 (content duties, the game-platform duties from
  2026-11-01, their age ratings, the pending ICTA rules, the under-15 restriction), Law No. 5809 authorisation and the
  BTK sector rules, Law No. 6112 with RTÜK, Laws No. 6563 (IYS, ETBIS), 6502, 5070, 5846, 6769 (and the trademark
  filings), 5378, 3065, 7194, 213 (e-archive), 6493, the KVKK AI recommendations and the AI bill (watch). Beside them
  the GDPR articles, EDPB Guidelines 01/2025, ePrivacy 5(3), the CRA (reporting since 2026-09-11, essential
  requirements from 2027-12-11, the reporting contact), NIS2, the PLD (2026-12-09), the AI Act (Article 50, Articles
  4-5, GPAI as watch), the DSA, the DSM Directive, the EAA, the dual-use regulation, EU sanctions, the CPC virtual
  currency principles, the Consumer Rights Directive, the Digital Fairness Act (watch), the EECC and AVMSD; the UK
  GDPR, AADC, OSA, PSTI, Equality Act and sanctions; COPPA, the EAR (classification and the annual report), OFAC,
  CCPA/CPRA, Section 508 (watch), the ADA, the CVAA, the secure-software attestation (watch), the DMCA and the Texas
  and Utah app store laws (watch); LGPD and ECA Digital; PIPL and China's labelling measures (watch); APPI; PIPEDA;
  Australia's Online Safety Act; Wassenaar, UN sanctions, IARC, PEGI, ESRB and the open-source licences; and the
  Apple, Google Play, Android, CA/B Forum, Steam, Epic and Microsoft Store policies. 18 scopes, all planned (nothing
  leaves a machine yet, so nothing is binding), and 21 governed rows: NET.2, CRASH-FIX.1, CRASH-FIX.2, OPS.1,
  NET-SERVER, NET-LIVE, COLLAB.1, 4.2d, ANTICHEAT, EXT.5, SHIP.7, SHIP.9, STORE.1, STORE.3, STORE.6, STORE-SOCIAL,
  AGENT.1, AI.1, WEB.5, SUPPLY.1 and SEC.1.
- **Honest sources.** The research sources are law-firm and secondary summaries, so every entry is `verified: false`
  with a null consolidated version, and the primary URLs are from knowledge (a few are still a regulator's home page,
  which the guard refuses once the entry is read). Unknown effective days are never invented: a month or year where
  that is all that is known, the 2026-10-27 reading day with a note for platform terms, and `watch` where the
  instrument itself is still to be identified.
- **The review.** Quarterly scans, the yearly platform events and the register's dated effective days are the
  calendar (13 events to 2027-12-31); the 2026-10-27 publishing research is the first full run; with 25 days' lag the
  first scan is due on 2026-11-26, after Law No. 5651's game-platform duties take effect.
  `scripts/register-review.py --register cmake/legal-register.json` plans the 80 official texts (one source per URL)
  and the gazette reading by hand, fetches them with digests, and `compare` lists every instrument never read or
  changed since the digest recorded when it was read; `record --trigger` records a gazette change with the pinned
  entries it changed, which owe a full re-research.
- **The owner.** `compliance.obligations` (planned owner `comply`, created by COMPLY.0) is in the capability registry:
  the rules generated from the register as CHIR assets, their `ceridc` and MCP query, the checks and the shared
  generators the governed rows consume.
- The CI step is renamed "Standards and legal registers are current" in both CI definitions and the CI platform
  contract.

## Verification

- `test-repository-tools.py`: the new `LegalRegister` class (8 cases: a complete register and its summary, binding
  obligations with day, month and sunset edges, scope liveness, official primary sources and jurisdictions, reading
  and counsel before closing and implementing, Turkish coverage, the format of entries, checks and dates, a missing
  register) and a `RegisterReview` legal case (plan per URL, fetch, compare with a never-read and a changed text and an
  unchanged one, `--apply`, `record --trigger` and its refusal); the existing `Registers` and `RegisterReview` cases on
  the two-register fixture. 25 cases pass on Windows.
- Teeth: twelve mutations each fail the cases: the live-scope condition dropped from the binding rule, a suffix host
  match without the dot, scope liveness on all rows, no Turkish coverage, no counsel rule, no read-before-close rule,
  no home-page rule, no sunset rule, effective counted a day late, planned checks citing anything, an unchanged digest
  reported as changed and `record` dropping its triggers.
- The real registers pass `check-registers.py` today, on 2026-11-01 (Law No. 5651's day) and on 2026-11-26; from
  2026-11-27 the legal register fails until its first scan is recorded, as the schedule intends.
- `check-capability-owners.py`, `check-repository.py`, `check-master-plan.py`, `check-allman-braces.py` and
  `check-generated.py` pass. No C++ changed, so the track machine was not started.

## Remaining in COMPLY.0

- **User action:** approve the download of the legal register's first read: the 80 official texts
  `python scripts/register-review.py --register cmake/legal-register.json plan --out <file>` lists (mevzuat.gov.tr,
  resmigazete.gov.tr, EUR-Lex, legislation.gov.uk, the eCFR and the other official hosts, plus the platform policy
  pages) and the gazette reading. The read records each consolidated version, day and digest, replaces the home-page
  URLs with the instruments' own documents, confirms the unverified calendar days and is due as the light scan on
  2026-11-26.
- The `comply` owner: generate the rules from the register as authored CHIR assets swapped live per jurisdiction as
  reviewed generations, and query them over `ceridc` and MCP (the MCP surface follows MCP-ADAPT, track A).
- Done when [OWN](../ROADMAP.md#slice-own) (track B) and [NET-STD](../ROADMAP.md#slice-net-std) are Done.
