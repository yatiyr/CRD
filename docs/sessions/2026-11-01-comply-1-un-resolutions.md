# COMPLY.1: the Security Council's resolutions read and the UN embargo authored

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.1](../ROADMAP.md#slice-comply.1) and [COMPLY.0](../ROADMAP.md#slice-comply.0) (track
C, batch 29; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts) and [sanctions and embargo screening](../design/registers.md#screening).
The register's fetch and its comparison are the [report](2026-11-01-legal-register-un-resolutions.md). The previous
batch read the UK's regimes ([UK regimes](2026-11-01-comply-1-uk-regimes.md)) and named the UN Security Council's
resolutions first.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The resolutions, their UN Digital Library records and the Consolidated
List read here fall under that decision's approval of the first read of the official legal texts. Their bodies stay
outside the repository (`~/.cerid/downloads/legal-2026-11-01-k/` and `~/.cerid/downloads/un-2026-11-01/`), pinned in
the report and below by size and SHA-256.

## CI first

No failure comes from a track C change.

- **Complete run 41274380605 (de5e10b0)** has one new failure since batch 28, win-shipping-profile. Its log was read:
  one test fails, `filesystem: the last write time converts exactly and orders files within one second`
  (`tests/foundation/platform/test_filesystem.cpp:145`), the same case as win-asan shard 2 and win-debug-scalar. That
  file is track B's and cc5ee9e9 on main fixes it. Seven of its jobs were still running at 09:30 UTC.
- **Complete run 41269105413 (b0b1b245)** reads "queued" for its Required result job; its other jobs ended with the
  known failures (fixed on main or track A's).
- **Push runs.** 41291759660 (cacd6547) and 41292341675 (a883e502) were cancelled before they started; 41292729183
  (30a34cc3, batch 28's last commit) was pending. Batch 28's commits still have no hosted run of their own.

<a id="the-readers"></a>
## The readers

- **The old primary is gone.** `int.un-sanctions` cited `www.un.org/securitycouncil/content/un-sc-consolidated-list`,
  which now redirects to main.un.org; main.un.org answers the fetcher's requests with `202` and an empty body (a bot
  challenge), so no page there can be a primary. The entry now cites the list itself,
  `https://scsanctions.un.org/resources/xml/en/consolidated.xml`, the instrument its name already gave.
- **The Consolidated List indexes the regimes.** Each party carries its regime's list type (`UN_LIST_TYPE`): on
  2026-11-01 Al-Qaida, CAR, DPRK, DRC, GB, Haiti, Iran, Iraq, Libya, Somalia, SouthSudan, Sudan, Taliban and Yemen
  (14). `register-review.py` reads them (`un-sc-list`), and the text read is the sorted list types alone, so the daily
  designations and the generation instant change nothing (`14 regimes`). `UN_REGIMES` names the resolution
  establishing each regime's Committee; each scan reports a list type the reader does not know, a known regime whose
  resolution is no entry and a regime the list no longer names whose entry has no sunset. Lebanon's arms embargo has no
  Committee and lists no one, so it is no list type and is read on its own.
- **Delivery hosts.** The official host answers with a signed link to the UN's file store. The plan now gives a source
  whose URL is a `sanctions_lists` file the delivery hosts that list names (the importer's own rule), and the fetch
  follows a redirect to them for that source only.
- **A resolution by its library record.** A primary `https://digitallibrary.un.org/record/<number>` resolves to the
  record's MARC XML (`?of=xm`), read first: it must be the one record of that number, name one Security Council symbol
  (`191 $a`, `S/RES/1718(2006)` or, since 2014, `S/RES/2821 (2026)`) and one day (`269 $a`), and link exactly one
  English file of that record (`856`, subfields in any order) with its size (`$s`). The English PDF is the text read,
  checked as a PDF of exactly that size and pinned by digest; the version is `S/RES/1718 (2006) of 2006-10-14, English
  text of 38780 bytes`. A UN file name carries its symbol's parentheses (`S_RES_2821_(2026)-EN.pdf`), so a version
  path may now hold `(` and `)`; a percent escape, query or `..` is still refused. The file downloads under the
  review's own User-Agent through a redirect on the same host.
- **The `resolution` type.** `check-registers.py` admits it for a Security Council resolution named `Security Council
  resolution <number> (<year>)`, and once read its version must name that symbol, so an entry citing another record is
  refused.
- **A renewed measure ends with its renewal.** The Council renews most arms embargoes for a period. A destination rests
  on the renewal in force, whose `sunset` is the period's end, and the import and the guard now refuse a destination
  whose entry has ended on the day: the Darfur renewal (resolution 2830 (2026)) ends on 11 December 2026, so on that
  day the guard and `crd-comply-sanctions-import` fail until its successor is read.

<a id="reading"></a>
## What was read

The plan ([report](2026-11-01-legal-register-un-resolutions.md)) read 337 of 349 sources on 2026-11-01, the list,
28 library records and their 28 English texts among them; `read` recorded `int.un-sanctions` and the 28 resolutions.
Every text fetched is byte for byte the one read by hand (the same SHA-256). By regime:

| Regime (list type) | Read as entries | What it restricts |
|---|---|---|
| ISIL and Al-Qaida | 1267 (1999) | Persons only: resolution 2734 (2024), paragraph 1, reaffirms the asset freeze, travel ban and arms embargo of 2368 (2017) on the persons listed |
| Taliban | 1988 (2011) | Persons only (paragraph 1) |
| Guinea-Bissau (GB) | 2048 (2012) | A travel ban on the persons listed (paragraphs 4 to 6) |
| Yemen | 2140 (2014) | Persons only; the arms embargo of 2216 (2015), paragraph 14, reaches the persons listed and those acting for them |
| Iraq | 1518 (2003), 687 (1991), 1483 (2003), 1546 (2004), 1957 (2010) | Arms to Iraq (687, paragraph 24(a), (c), (d)), kept by 1483, paragraph 10, except for the Government and the multinational force (1546, paragraph 21); 1957 ended the WMD items of paragraph 24(b) |
| DRC | 1533 (2004), 1807 (2008), 2825 (2026) | Arms and military assistance to non-governmental entities and individuals operating in the DRC, renewed until 1 July 2027 |
| Sudan | 1591 (2005), 1556 (2004), 2830 (2026) | Arms to non-governmental entities and every belligerent in Darfur, renewed until 11 December 2026 |
| Libya | 1970 (2011) | Arms and military assistance to Libya, no period |
| none (Lebanon) | 1701 (2006) | Arms to any entity or individual in Lebanon unless the Government or UNIFIL authorises, no period |
| CAR | 2127 (2013), 2745 (2024), 2827 (2026) | Arms to armed groups and associated individuals operating in the CAR (the embargo on the country was lifted), extended until 31 July 2027 |
| Haiti | 2653 (2022), 2794 (2025), 2831 (2026) | Arms of every type and military assistance to Haiti, except the UN, UN-authorized missions and units under the Government's command, for a year from 14 October 2026 |
| Somalia | 2713 (2023), 2806 (2025) | Weapons, ammunition and military equipment to Somalia, save the Federal Government and its forces (deliveries to them notified), and the IED components of Annex B, Part I |
| South Sudan | 2206 (2015), 2428 (2018), 2821 (2026) | Arms and military assistance to the territory of South Sudan, renewed until 31 May 2027 |
| DPRK, Iran | not yet | Read next (below) |

The Darfur measure names "the states of North Darfur, South Darfur and West Darfur" of 2004; Central and East Darfur
were formed from them in 2012, so the measure reaches all five of today's states, each an ISO 3166-2 subdivision
(SD-DN, SD-DS, SD-DW, SD-DC, SD-DE), never Sudan as a whole.

The chain to each text in force was read in the Council's own texts, and the resolutions that only carry it are pinned
here rather than as entries (each read from the UN Digital Library on 2026-11-01):

| Resolution | Record | Read for | Bytes | SHA-256 |
|---|---|---|---:|---|
| 2734 (2024) | 4051294 | paragraph 1: the ISIL and Al-Qaida measures | 501963 | `2c47648172efd5854adf627a5d2711a2bcb063a8b5c54181ee912e00ea53dd4c` |
| 2216 (2015) | 791250 | paragraph 14: Yemen's targeted arms embargo | 181557 | `678803f0aa8cb990d97bf0bdb9e2c7dae2965778dca7555b00a8c58c89c60951` |
| 2783 (2025) | 4084763 | paragraphs 1 to 3: the DRC measures 2825 (2026) renews | 189319 | `5760401fb756ce8cef2cb21a0bb3a1305016793c1395ddf3cd345f2aced307ef` |
| 2750 (2024) | 4061079 | paragraph 1: Sudan renewed until 12 September 2025 | 166893 | `28a062961e3848d66ce45fd70f5b62c24648fb746577b68de707f4118ad8e243` |
| 2791 (2025) | 4088200 | paragraph 1: Sudan renewed until 12 September 2026 | 190952 | `a0c6c82a2715818c73b6579edaeda789593de31c3aa6782f41aba128a710cb90` |
| 2828 (2026) | 4127877 | paragraph 1: Sudan renewed until 9 October 2026 | 159297 | `f4ab8b91345dd8f511e971857d9a9e6a2026e1f1f50645166f2aae598a1820f8` |
| 2035 (2012) | 721136 | paragraphs 3 and 4: Sudan's entity listings and the ended CPA exemption | 41332 | `22b81008f169a3b8297bfb804e446049b97edb9f8cec19926846b070dff8026a` |
| 2699 (2023) | 4022890 | paragraph 14: Haiti's small arms embargo replacing 2653, paragraph 11 | 217001 | `34dcec6b2cd0d164b1c0cdf352dd984e2413e03a8a808e2e444362ede4775097` |
| 2752 (2024) | 4064280 | paragraphs 1 to 3: Haiti's embargo on arms of every type | 192861 | `6f54a9fc7fe3e964e97e9bb377d71f4002c30c521c9aaf0d3709f83aa9864167` |
| 2714 (2023) | 4029177 | paragraphs 1 and 2: the 733 (1992) embargo lifted | 158501 | `87667b8f1a9e3e125c39bf38eb8b7d08dac2e1bb72984f0cd8c8492814f8afd5` |
| 2776 (2025) | 4077956 | paragraph 4: Somalia's weapons measure, which 2806 (2025) reaffirms | 282712 | `94c9c8b9254fc68d2da33138e897614248cf9f10770e85cbd84925fa6bef859d` |
| 2009 (2011) | 710980 | paragraph 13: Libya's exemptions for the Libyan authorities | 44774 | `2323de07e8aa1e4977172b609a26eb80399908968aa2660e06965acdb4cd7993` |
| 2095 (2013) | 745873 | paragraph 9: Libya's non-lethal equipment freed from approval | 47765 | `344b2e81618bce40689629c18ac98a5bbcade43fb5ebea8e99355c08bb705f64` |

The UN Digital Library's titles of every resolution from 2700 (2023) to 2832 (2026) were read to find the renewals in
force (the Libya measures of resolution 2146 (2014) on illicit petroleum exports, extended by 2819 (2026), reach
designated vessels, never a supply to Libya; 2801 (2025) renews the Yemen measures until 14 November 2026).

<a id="embargo"></a>
## The UN embargo, and an open reading that failed open

`un.sc.embargo` holds 13 destinations, all sectoral, each read in the resolution in force: CD (1807 (2008), on 2825
(2026)), CF (2745 (2024), on 2827 (2026)), HT (2794 (2025), on 2831 (2026)), IQ (687 (1991)), LB (1701 (2006)), LY
(1970 (2011)), the five Darfur states (1556 (2004), on 2830 (2026)), SO (2713 (2023), on 2806 (2025)) and SS (2428
(2018), on 2821 (2026)). The 15 entries read without a destination are the regimes' founding resolutions and the
texts the destinations rest on beside the one in force.

The DPRK and Iran regimes are not read yet, so the generation is `open`. Writing it would have been a regression: the
decoder knows nothing of `open`, so the DPRK and Iran would screen Clear under `un.sc.embargo`, where before this batch
the missing generation answered Unavailable. The importer now refuses to write an open reading (`OpenReading`:
"13 embargoed destinations authored, not written while the reading is open: ..."), so a screening naming
`un.sc.embargo` still finds the list missing and fails closed. The generation, its CTest import and its hidden
`[.imported]` case land together once the reading closes; the US, EU and UK generations (none open) are unchanged.

No counsel question is new: every UN destination restricts arms, military assistance or IED components, which no
Cerid flow supplies.

## Verification

- `python scripts/test-repository-tools.py SanctionsLists RegisterReview`: 57 cases pass, 7 new (the record and
  English text end to end with the guard's name and symbol checks, the list's regimes and their judgement, the
  delivery hosts, the readers' refusals, the ended measure through the guard and the importer, the open reading, and
  the real register's UN embargo authored and not written) and 12 importer calls given their day.
- 21 source mutations, each caught: the PDF size check, the record's own files, one English text, the regimes-only
  digest, the ended-regime and unknown-regime and missing-entry judgements, the parenthesised version path, the
  delivery hosts in the plan and in the fetch, the list type's whitespace, the typographic symbol, the entities' list
  types, the day check, the guard's name and symbol checks and its day, the sunset rule (and its boundary) and both
  sides of the open reading.
- `gen-comply-rules.py --review INT`: generation 5 (34 rules); `check-generated.py --refresh` for the file.
- `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py`, `check-generated.py` and
  `check-registers.py` pass.
- No C++ changed: the comply suite decodes the new INT generation on CI (`test_rules.cpp`), whose sunsets the decoder
  and its tests already cover, so the cloud machine was not started.

## Next

1. **COMPLY.1.** Read the DPRK regime (1718 (2006) and every resolution after it that adds a measure) and the Iran regime
   (1737 (2006), 1747 (2007), 1803 (2008), 1929 (2010), 2231 (2015) and the Council's record of its paragraphs 11 and 12
   since August 2025); author their destinations, close `open`, add `un.sc.embargo` to the CTest import and its hidden
   Catch2 case. Read the Darfur renewal before 11 December 2026.
2. Then Turkey's asset-freeze decisions and their importer, and the Wassenaar Category 5 Part 2 and Turkey's dual-use
   communiqué.
