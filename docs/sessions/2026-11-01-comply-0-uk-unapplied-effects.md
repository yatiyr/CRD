# COMPLY.0: the changes a revised UK text has not applied, named in its version and read as published

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [COMPLY.0](../ROADMAP.md#slice-comply.0) (track C, batch 27; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts). The previous batch is
[the acts the EU consolidated texts lack](2026-11-01-comply-0-acts-the-consolidated-texts-lack.md), whose first next
item this batch closes.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The two published acts read below as the reader's samples belong to the
first read of the official legal texts that decision approved. Their bodies stay outside the repository
(`~/.cerid/downloads/uk-effects-probe-2026-11-01/`), pinned below by size and SHA-256.

## CI first

No failure comes from a track C change.

- **Complete run 41274380605 (de5e10b0)**, still running: 10 of 30 jobs passed and 5 failed. The two new failures
  since batch 26 are already fixed on main. win-tidy failed on `readability-identifier-naming` for `kUnixNs` in
  `tests/foundation/platform/test_filesystem.cpp:134`, which 4b9bc837 renames. linux-gcc-shipping failed on the
  `-Wmaybe-uninitialized` LTO warning in `tests/numerics/hesap-stats/test_multivariate.cpp:125`, which 76330344 fixes.
  The other three were known: clang-cl's `__udivti3` link belongs to track A, and linux-gcc-release is the same
  test_multivariate warning.
- **Complete run 41269105413 (b0b1b245)**: 14 failures, all from code older than its fixes. Thirteen Windows lanes
  fail on `engine/foundation/platform/src/filesystem.cpp:308-309`: `windows.h`'s `min` and `max` macros break
  `std::numeric_limits<i64>::max()`. 9723ac75 fixes this. win-arm64-debug fails on the `checksum.cpp` C4324 error,
  which 921e19cc fixes.
- **Push runs.** 41288713312 (f809f975, which includes batch 26's 73ebcdbe) was pending with no job started.
  41286750175 (8311a54e) had 1 of 30 jobs done.

## The reader

legislation.gov.uk's revised text lists, in its Crown Legislation Markup, every change its editors have not yet applied
(`ukm:UnappliedEffects`). Each `ukm:UnappliedEffect` carries:

- the affecting act (`AffectingURI`, `http://www.legislation.gov.uk/id/<type>/<year>/<number>`);
- whether the editors owe it (`RequiresApplied`). The value is "false" for a conditional amendment that took no
  effect, or a commencement;
- its `ukm:InForce` days. Each is a `Date`, or `Prospective="true"` until an order commences the change. A commenced
  change carries both.

`scripts/register-review.py` now treats the revised text as its own version index, as CELLAR's index is for an EU act:

- **What the text lacks.** An effect the editors owe that has an in-force day on or before the fetch day is a change
  the text lacks. A prospective effect, or a conditional one, is not.
- **The version** names each affecting act once: `2026-09-30 + ukpga/2026/20`.
- **The fetch** reads each act as published, once:
  - an Act as enacted (`ukpga`, `asp`, `asc`, `anaw`, `nia`) at `<act>/enacted/data.xml`;
  - an instrument as made (`uksi`, `ssi`, `wsi`, `nisr`) at `<act>/made/data.xml`;
  - an affecting type with no known published form is refused, never guessed.
- **`read`** refuses an entry until every act was read and is a final text naming that form
  (`dc:identifier` ending `/ukpga/2026/20/enacted`, `ukm:DocumentStatus` "final"). The check now goes through each
  document's own reader, so CELLAR's documents name theirs (`cellar`).
- **The reader refuses** a `RequiresApplied` other than "true" or "false", an owed effect with no in-force day, a day
  that is not YYYY-MM-DD or does not exist, and a version whose day is not the text's `dct:valid`.

The two published forms were read from the real files:

| File | Bytes | SHA-256 | Identifier, status |
|---|---|---|---|
| https://www.legislation.gov.uk/ukpga/2026/20/enacted/data.xml | 3 836 918 | `5edf6df8bb0b6ce2bd7a2bdaff723c30e7700aadc14fa277b254eb6cf5ad752a` | `.../ukpga/2026/20/enacted`, final (Crime and Policing Act 2026, enacted 2026-04-29) |
| https://www.legislation.gov.uk/uksi/2026/386/made/data.xml | 397 762 | `71ff3a384c7e393e3d9916907d6c65f9cf99d5ee1728a5597118c088c9aaf002` | `.../uksi/2026/386/made`, final (made 2026-03-26) |

## The seven texts on 2026-11-01

The reader ran on the seven UK texts of batch 26's fetch (`~/.cerid/downloads/legal-2026-11-01-i/fetched/`):

| Text | Unapplied | Owed | In force by 2026-11-01 | Version on 2026-11-01 | Version from 2026-11-06 |
|---|---|---|---|---|---|
| UK GDPR (`eur/2016/679`) | 5 | 0 | 0 | 2026-09-30 | 2026-09-30 |
| Equality Act 2010 (`ukpga/2010/15`) | 17 | 10 | 0 | 2026-10-30 | 2026-10-30 |
| Data Protection Act 2018 (`ukpga/2018/12`) | 9 | 7 | 0 | 2026-09-30 | 2026-09-30 |
| SAMLA 2018 (`ukpga/2018/13`) | 1 | 1 | 0 | 2026-10-15 | 2026-10-15 |
| PSTI Act 2022 (`ukpga/2022/46`) | 0 | 0 | 0 | 2026-04-07 | 2026-04-07 |
| Online Safety Act 2023 (`ukpga/2023/50`) | 24 | 10 | 0 | 2026-09-30 | 2026-09-30 + ukpga/2026/20 |
| PSTI regulations (`uksi/2023/1007`) | 0 | 0 | 0 | 2025-12-04 | 2025-12-04 |

Each of batch 26's five counts is one above this reader's; counting the `ukm:UnappliedEffects` container is the likely
cause.

None of the 56 effects is owed and in force on 2026-11-01. Every owed effect is prospective, and the Equality Act's
s. 104(7) repeal is due on 2030-12-31. So the versions recorded for `gb.uk-gdpr`, `gb.dpa-2018` and
`gb.psti-regulations` stand, and the register is unchanged.

The first owed change arrives on 2026-11-06. S.I. 2026/1090 commences sections 104(5), 106(7) and 107(5) of the Crime
and Policing Act 2026, which amend paragraph 29 of the Online Safety Act's Schedule 7 (the priority offences). Run
with that day, the reader names the Online Safety Act `2026-09-30 + ukpga/2026/20` and plans
`https://www.legislation.gov.uk/ukpga/2026/20/enacted/data.xml`. `gb.osa` was never read: its first reading, owed with
Ofcom's codes, will read the Act at that version with the Crime and Policing Act's sections.

## Verification

- **Python.** `RegisterReview` (28 cases) and `SanctionsLists` (19) pass. Two cases are new:
  - a reader case: the fetch day before and on commencement, a conditional effect, a prospective effect, a text with
    no unapplied effects, six refusals, the version check, and the published reader's three refusals;
  - an end-to-end case: two texts lacking one act read it once. A failed act and a mismatched act each refuse
    `read`, and a refused read writes nothing.
- **Mutations.** Nine source mutations were each caught:
  - counting effects the editors do not owe;
  - `<` for `<=` on the in-force day;
  - checking every document with CELLAR's reader;
  - dropping the version's day check;
  - an Act read as made;
  - accepting a non-final text;
  - accepting an owed effect with no in-force day;
  - not recording the version on the fetched item;
  - not treating the text as its own index.
- **C++.** None changed, so the track's machine was not started.
- **Guards.** `check-registers.py`, `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py` and
  `check-generated.py` pass.

## Next

1. **COMPLY.1.**
   - Read the UK's regime regulations through legislation.gov.uk's `uksi/` reader, and the UN Security Council
     resolutions, each for its own embargo generation.
   - Then read Turkey's asset-freeze decisions and write their importer.
   - Read the Wassenaar Category 5 Part 2 and Turkey's dual-use communiqué.
2. **COMPLY.0.** Read the other eCFR texts of batch 19's fetch (COPPA, Section 508, the CVAA), then the CELLAR and UK
   acts still unread. `gb.osa` is read at its version after 2026-11-06, with the Crime and Policing Act's sections.
