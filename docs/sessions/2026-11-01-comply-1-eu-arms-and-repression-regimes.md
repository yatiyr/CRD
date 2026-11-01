# COMPLY.1: the EU arms, internal-repression and monitoring regimes and the blocking statute read

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.1](../ROADMAP.md#slice-comply.1) and [COMPLY.0](../ROADMAP.md#slice-comply.0) (track
C, batch 24; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening). The register's fetch and its comparison are the
[report](2026-11-01-legal-register-eu-arms-and-repression-regimes.md). The previous batch is the
[EU embargo](2026-11-01-comply-1-eu-restrictive-measures.md), whose generation left these regimes `open`.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The 19 regulations read here and the regime index below are part of the
first read of the official legal texts that decision approved. Their bodies stay outside the repository
(`~/.cerid/downloads/legal-2026-11-01-g/`), pinned below by version, size and SHA-256.

## CI first

No failure comes from a track C change, and no hosted run holds this track's last batch yet.

- **Complete run 41274380605 (de5e10b0)**, dispatched at 04:46 UTC, was still running at 07:25 UTC. Of its 30 jobs, 6
  had passed, 16 were running and 6 were queued.
  - Passed: win-vs, the three linux-arm64 lanes and the Windows Repository checks job. That job is the first Windows
    run of batch 19's register-review cases.
  - Failed: win-clang-cl and win-clang-cl-shipping, both at the link of `crd-perf-tests`. lld-link reports
    `undefined symbol: __udivti3` and `__divti3`, referenced by `tests/foundation/perf/test_instruction_access.cpp`
    lines 515 and 522: a 128-bit division that clang-cl leaves to compiler-rt, which the link lacks.
  - Owner: that file belongs to HW-RESILIENCE.2 (track A; 1f053102, 76e61d51). It is unchanged on main at c591984a,
    so the fix belongs to track A.
- **Push runs.** Every push run from 41273181833 (092a2726) to 41282416057 (0b9567f5) was cancelled by a newer push
  before it started. 41284087995 (c591984a) was pending.
- **No new dispatch.** The complete run already queued would hold a new one behind it, and COMPLY.1 does not close
  in this batch.

## Which regimes attach to a place

The previous generation's `open` named fifteen regimes from memory. They were checked against the EU's own index
before anything was read: the EU Sanctions Map's regime index, `https://www.sanctionsmap.eu/api/v1/regime`, the data
behind the map that is `eu.restrictive-measures`' primary (retrieved 2026-11-01, 392 161 bytes, SHA-256
`9baf902c0abb2c1a0f0aac720ec1a5137126cf1456871b45895d9a4dcefe912f`, kept in
`~/.cerid/downloads/probe-2026-11-01d/regime.json`). It lists 55 regimes, each with its legal acts and its measures.

- **Place regimes.** The regimes with a measure other than an asset freeze, an admission ban or a claims ban are the
  fifteen named, with Somalia's second regulation (No 147/2003) and Iran's two regimes beside No 267/2012. The index
  showed one more that the `open` had missed: Regulation (EU) 2024/1485, the Russia human-rights regime, whose
  internal-repression and monitoring annexes attach to Russia. Each regulation's ELI was taken from the index. The
  plan's CELLAR resolver then checked that each fetched consolidated text is the act it names.
- **The blocking statute.** The index lists Regulation (EC) No 2271/96 under the United States. It attaches to no
  place, but it decides which authorities an operator in the Union may screen with, so it was read too.
- **Person regimes.** Bosnia and Herzegovina, Burundi, Guatemala, Guinea, Guinea-Bissau, Mali, Moldova (both),
  Nicaragua, Niger, Tunisia, Türkiye (the Eastern Mediterranean drilling regime), Ukraine's No 208/2014 and
  No 269/2014, Russia's hybrid-threats regime (2024/2642), Lebanon's 2005 and 2021 regimes, Sudan's
  Regulation (EU) 2023/2147 and the thematic regimes (chemical weapons, cyber-attacks, human rights, terrorism, Hamas
  and the Palestinian Islamic Jihad, ISIL and Al-Qaida) attach to listed persons only. The EU financial sanctions file
  lists those persons, and `eu.fsf` screens them.
- **No regulation.** China's arms embargo is the 1989 political declaration, and Iraq's is Common Position
  2003/495/CFSP. Each Member State applies these through its licensing of military goods; no Article 215 regulation
  carries them.

<a id="reading"></a>
## What was read

The register was refreshed with the 19 regulations as new entries (`legal-2026-11-01-g`, 170 of 183 sources read; the
13 not read are the hosts that refused before). The fetch was compared and applied. A first fetch (`-f`, made before
2024/1485 and the blocking statute were added) held the same bytes for the other 17. Each text is CELLAR's latest
English consolidated version, which is a documentation text.

| Entry | Regulation | Version | Bytes | SHA-256 |
|---|---|---|---:|---|
| `eu.reg-359-2011.iran-human-rights` | (EU) No 359/2011 | 02011R0359-20260724 | 878506 | `eaece86a939c132c4073f7f35c2dcb17f77e390c10513f2cee0cf7ec4dd1d07b` |
| `eu.reg-2023-1529.iran-military-support` | (EU) 2023/1529 | 02023R1529-20260801 | 478701 | `4535f01372b2245a0b47ff8af84c310626a9b3fb28f82af303b6ddfee1efda8f` |
| `eu.reg-401-2013.myanmar` | (EU) No 401/2013 | 02013R0401-20260429 | 478949 | `10cf455e2330eabc91c4750f47ab94d566e3aae07ca545b6b18d8485a405ea44` |
| `eu.reg-2017-2063.venezuela` | (EU) 2017/2063 | 02017R2063-20251217 | 268337 | `2959633c446c7cc3aaa1a787d14f0f1845ae9fed26a1bee833f5e6f24a994ddb` |
| `eu.reg-2016-44.libya` | (EU) 2016/44 | 02016R0044-20261013 | 307611 | `29cc25eac8f0e37a778c6454a7c4a85fa52ecad9616f7aeba4161864ef2907ba` |
| `eu.reg-1210-2003.iraq` | (EC) No 1210/2003 | 02003R1210-20251224 | 254456 | `e731c5a87f7d9253caf8cb3677d12ddc64c729f2b6e73cbb7e1f69f93b989e3b` |
| `eu.reg-1412-2006.lebanon` | (EC) No 1412/2006 | 02006R1412-20240913 | 38912 | `b08f9e34b55c7cade29665cb0c3d33b1601487452547d203911d24f5103ba051` |
| `eu.reg-356-2010.somalia` | (EU) No 356/2010 | 02010R0356-20240913 | 130575 | `96281efcd51a8abaa160a3ff27406bde356ae1821792e73a0850d5821f80b032` |
| `eu.reg-147-2003.somalia` | (EC) No 147/2003 | 02003R0147-20240913 | 82295 | `951c127dbea177770741747d2de933825adc4e0d5d0467e23ef2394c2e6dce39` |
| `eu.reg-2015-735.south-sudan` | (EU) 2015/735 | 02015R0735-20260624 | 144242 | `fe96864d488b6c69452ad4fe6f86bb385e86ec2c65e3f61b90e577f069d4a938` |
| `eu.reg-747-2014.sudan` | (EU) No 747/2014 | 02014R0747-20260622 | 115990 | `5fd48ebe4f94520232f9d4a1d52f1a554c2030b7ea8c963d05a86af0d8cffe61` |
| `eu.reg-224-2014.central-african-republic` | (EU) No 224/2014 | 02014R0224-20250326 | 194761 | `055b8371fbc78bfc866151565871444b71bd7bc9ed4e28d8f3e501fafa30a2ed` |
| `eu.reg-1183-2005.drc` | (EC) No 1183/2005 | 02005R1183-20251210 | 380129 | `cc7d03c8fab79f6fe73cf9da0d45354c316677482d42335fc09cd41c9321c910` |
| `eu.reg-314-2004.zimbabwe` | (EC) No 314/2004 | 02004R0314-20260219 | 102769 | `ddd63f19cefcfbbb4559be1309c25eeebb9a45fada97e7e53d946fe718647047` |
| `eu.reg-2022-2309.haiti` | (EU) 2022/2309 | 02022R2309-20251215 | 187700 | `125b706a84006bf7e407c3ee1bb0fb32807879bede3b7cef6b1d3f35d2a3cd26` |
| `eu.reg-1352-2014.yemen` | (EU) No 1352/2014 | 02014R1352-20240913 | 166318 | `6c9d5f663830e39c29d17fae5a04c6b11e3994141f20e45063dc1529c0880ea9` |
| `eu.reg-753-2011.afghanistan` | (EU) No 753/2011 | 02011R0753-20240913 | 361493 | `5207a74aa089b4873b6f749452e640f3e08414c8c637749548be3a45e86dc604` |
| `eu.reg-2024-1485.russia-human-rights` | (EU) 2024/1485 | 02024R1485-20260928 | 417894 | `ede1e1649057cb6159cc83fe625e4639eb5f19ae3ae7399e024d1b4c60f1aa49` |
| `eu.reg-2271-96.blocking-statute` | (EC) No 2271/96 | 01996R2271-20180807 | 48268 | `ba1543e80f7bd6bffa4ef3cdc9743531be8f0f334656e52ec9741608910f1c89` |

What each says that bears on Cerid:

- **Monitoring and interception: Iran (No 359/2011), Myanmar/Burma (No 401/2013), Venezuela (2017/2063) and Russia
  (2024/1485).** Each bans the Annex of equipment for internal repression and the related services. Each also needs
  prior authorisation to supply the Annex of internet and telephone monitoring and interception equipment, technology
  and software (359/2011 Articles 1b and 1c with Annex IV; 401/2013 Articles 3b and 3c with Annex III; 2017/2063
  Articles 6 and 7 with Annex II; 2024/1485 Articles 3 and 4 with Annex II). Iran's, Venezuela's and Russia's also
  ban monitoring or interception services to the State. Russia's General Note also excludes what Article 2a of
  Regulation (EU) No 833/2014 covers.
  - The three annexes are the same list: deep packet inspection, network interception, remote infection, pattern
    recognition and profiling, VoIP interception and the like. Its "D" item covers software for them. The Annex
    reaches only what falls within "internet, telephone and satellite communications interception and monitoring
    systems".
  - Its General Note excludes software sold from stock for installation without further substantial support, and
    software in the public domain.
  - Myanmar's Article 3a also bans any dual-use item of the EU list for military use, a military end-user or the
    Border Guard Police. Venezuela's Article 2 bans services and financing related to the Common Military List.
- **Iran's military support (2023/1529).**
  - Article 2 bans the Annex II goods and technology that might contribute to UAV or missile manufacture, with the
    related services, financing and intellectual property, and their transit via Iran.
  - The software Annex II names is specially designed for the controlled machine tools, printed circuit boards and
    sensors. Its computers are those rated above 70 °C ambient.
  - Article 2a bans transactions with the Annex IV ports and locks.
- **Libya (2016/44).** It bans:
  - the Annex I equipment for internal repression and the Common Military List, with the related services (Articles
    2 and 3), and arms procured from Libya (3a);
  - without authorisation, the Annex VII items usable for migrant smuggling (2a).
- **Arms-only regimes.** These ban technical assistance, brokering and financing related to military activities and
  the Common Military List, and some ban the goods themselves:
  - Lebanon (No 1412/2006, Article 2) and Sudan (No 747/2014, Article 2): the services, to any person in, or for use
    in, the country.
  - South Sudan (2015/735, Article 2): the same services, and armed mercenary personnel.
  - Somalia (No 147/2003): the services (Article 1); improvised explosive device components only with authorisation
    (3c); Somali charcoal (3b).
  - Zimbabwe (No 314/2004): the Common Military List with its services (Article 2) and the Annex I equipment for
    internal repression (3). Its asset freezes are repealed, so the regulation lists no persons.
  - Haiti (2022/2309): the Common Military List with its services, to any person in, or for use in, Haiti (Article 2).
    The sanctions map still describes it as covering listed persons only, but the text now reaches every person
    there.
  - The Central African Republic (No 224/2014, Article 2): only armed groups and associated individuals operating
    there.
  - The Democratic Republic of the Congo (No 1183/2005, Article 1a): only non-governmental entities and persons
    operating there. Military assistance to anyone else there is notified to the Sanctions Committee.
- **Persons only.** Somalia's No 356/2010 (Articles 2 and 8), Yemen's No 1352/2014 (Articles 1a and 2) and
  Afghanistan's No 753/2011 (Articles 2 and 3) bar military assistance to, and freeze the assets of, the persons in
  their Annex I.
- **Iraq (No 1210/2003).** It bans trade in cultural property illegally removed from Iraq (Article 3) and freezes the
  previous Government's and the listed persons' assets (Article 4). Article 2's oil proceeds rule concerns the
  Development Fund for Iraq.
- **The blocking statute (No 2271/96).**
  - Article 5 forbids a person it covers to comply with any requirement or prohibition based on the laws of its
    Annex, unless the Commission authorises it.
  - Article 11 says who is covered: an EU national resident in the Union, a legal person incorporated there, and
    anyone acting there in a professional capacity.
  - Article 2 requires informing the Commission within 30 days when those laws affect the person's economic or
    financial interests.
  - The Annex, as Delegated Regulation (EU) 2018/1100 replaced it, names 31 CFR 515 (the Cuban Assets Control
    Regulations), the Iranian Transactions and Sanctions Regulations, the Cuban Democracy and LIBERTAD Acts and the
    Iran sanctions acts of 1996 and 2012.
  - So an operator established in the Union may not refuse a Cuban or Iranian customer because `us.ofac.embargo`
    says so, where only those laws require it.
  - A screening already applies only the authorities its request names (`ScreenRequest`, `ceridc screen
    --authority`). The entry's check is planned in COMPLY.8, the store compliance kit, which selects the
    authorities from the operator's establishment.

`register-review.py read` recorded the 19 entries (EU rule generation 8, reviewed 2026-11-01). The check of each entry
that names a destination is the hidden `[.imported]` case that screens it. The check of each that names none is the
imported official lists' case, because the EU financial sanctions file lists its persons.

<a id="embargo"></a>
## The EU embargo

`eu.council.embargo` (generation 20261101000000; written into an emptied directory, as the CTest fixture writes it)
now holds 25 destinations, all sectoral apart from the four partial oblasts, and `open` is null.

- **New destinations.**
  - RU again: Regulation (EU) 2024/1485.
  - IR, twice more: Regulation (EU) No 359/2011 and Regulation (EU) 2023/1529.
  - MM (No 401/2013), VE (2017/2063) and LY (2016/44).
  - LB (No 1412/2006), SO (No 147/2003), SS (2015/735) and SD (No 747/2014).
  - CF (No 224/2014), CD (No 1183/2005), ZW (No 314/2004) and HT (2022/2309).
- **The rule, stated exactly.** A destination is a place to which a measure restricts a supply, whatever the sector.
  The previous session's words, "every measure that attaches to a place", would also have made Iraq a destination,
  but Iraq's Article 3 restricts trade in property removed from Iraq and no supply to it.
- **Regulations read that name no destination.** Iraq's, Somalia's No 356/2010, Yemen's and Afghanistan's
  regulations and the blocking statute are read entries with no destination. A store screening a customer from those
  countries gets a clear answer from the embargo and a party answer from `eu.fsf`.
- **CF and CD.** Their measures restrict supply only to armed groups, or to non-governmental persons operating there,
  so a destination there is sectoral: a match needs the review to establish who the buyer is.
- **Generation numbering.** The importer numbers a second reading published on the same day with the previous
  generation plus one (`generation_of`), so this rewritten reading never replays the earlier generation's number with
  new bytes. The CTest writes into an emptied directory, so its generation stays 20261101000000.

Screened together with the US embargo, Iran now has four hits (blocked by the US programme), North Korea two
(blocked) and the Russian Federation four (review). Venezuela has two sectoral hits (review). Myanmar, Haiti and the
Central African Republic have one EU hit each (review). Iraq, Yemen and Turkey are clear.

## Counsel

Six questions join the open review `counsel.2026-11-01`, which now holds 73. As the user decided, none holds a row.

- **Regulation (EU) No 359/2011.** Is any Cerid component within Annex IV? Candidates are NET-DEBUG's capture of a
  product's own traffic (pcapng, qlog, session recording), the crash collector and OPS.1's telemetry. Or does the
  General Note's mass-market or public-domain exclusion take it out?
- **Regulation (EU) No 401/2013.** The same Annex question. And if a Cerid export's cryptography is ever classified a
  dual-use item (5D002, not mass-market), how does the store establish under Article 3a that a buyer in Myanmar/Burma
  is not a military end-user or the Border Guard Police?
- **Regulation (EU) 2017/2063.** The same Annex question, and whether supporting the Venezuelan State's use of a
  product built with Cerid could be a monitoring or interception service under Article 7(1)(c).
- **Regulation (EU) 2024/1485.** The same Annex question for Russia, including where Article 2a of Regulation (EU)
  No 833/2014 already reaches the encryption.
- **Regulation (EC) No 2271/96.** Suppose the publisher (the user's Turkish company) establishes an operator,
  subsidiary or employee in the Union, or a developer incorporated there screens with Cerid. Does Article 5 then
  forbid refusing a customer in Cuba or Iran because 31 CFR 515 or the Iranian Transactions and Sanctions Regulations
  require it? Must that operator leave `us.ofac.embargo` out, apply it only where US jurisdiction attaches, or seek
  the Commission's authorisation? Does Article 2's 30-day duty apply?
- **Regulation (EC) No 1412/2006.** Is licensing or supporting a customer that builds a military training simulation
  "technical assistance related to military activities"? The question covers Lebanon and the like provisions for
  South Sudan, Sudan, Somalia, the DRC, the CAR, Zimbabwe, Haiti, Libya, Venezuela and Myanmar/Burma. Must the store
  ask buyers there for their end use?

<a id="fetch"></a>
## A fetch cut short

The second fetch stopped with a traceback after 122 files. One response ended early, and
`http.client.IncompleteRead` escaped `fetch_sources`. That function catches `OSError`, but an `IncompleteRead` is an
`http.client.HTTPException`, not an `OSError`. So one truncated body lost the whole fetch and its manifest instead
of being recorded as not read. `import-sanctions.py` had the same gap: its command handler would print a traceback,
not its one-line error.

Both now catch `http.client.HTTPException` beside `OSError`:

- `fetch_sources` records the item with its error, and the next run reads it again, as it already did for a refused
  connection or a timeout;
- the importer fails with `import-sanctions: IncompleteRead(...)` and writes nothing.

The fetch was then repeated in full into an emptied directory.

## Verification

- **Python.**
  - `SanctionsLists` and `RegisterReview` pass 42 cases.
  - The real-EU-generation case now holds the 25 destinations, the 21 texts of the reading, the five read entries
    without a destination and the null `open`.
  - Two new and extended cases cover the truncated response: the fetch records `IncompleteRead` for one source and
    reads the rest, and the importer's fetch fails cleanly and writes no file. With either fix reverted, its case
    errors.
  - Five register mutations were each caught: Haiti dropped; an Iraqi destination added; `open` set; Yemen's
    reading unverified; Regulation (EU) 2023/1529's measure made comprehensive.
- **Guards.** These pass:
  - `check-registers.py`: 150 legal entries, 136 pinned, 73 open counsel questions.
  - `gen-comply-rules.py --check` (EU generation 8, reviewed once for the batch) and `check-generated.py` (the EU
    rule file refreshed).
  - `check-master-plan.py`, `check-repository.py` and `check-allman-braces.py`.
- **C++.** These ran on the track's machine in two runs, both on the linux-gcc-debug tree: b8db7ff6 (06:55-07:06 UTC)
  and 8fd7348e (07:18-07:22 UTC), the work-in-progress commits of this tree. The results below are 8fd7348e's.
  - `crd-comply-tests`, `crd-fuzz-comply-sanctions`, `ceridc` and `crd-ceridc-tests` built with no warning.
  - The 61 scoped CTests (comply, ceridc and the `crd-no-` and `crd-check` guards) passed. `crd-check-isa-isolation`
    was left out: it judges every object in the tree, and other tracks' targets are not built in this partial tree,
    which is what made it fail in batch 23.
  - The four `[.imported]` cases passed (600 assertions). `[sanctions]` ran 4 576 assertions in 18 cases.
  - Two register mutations each made `crd-comply-sanctions-imported` fail: Haiti dropped (on b8db7ff6) and
    Regulation (EU) 2024/1485 dropped (on 8fd7348e). With the register restored, the fixture chain passed again.
  - `ceridc screen --authority us.ofac.embargo,eu.council.embargo` on the written lists answered:
    - blocked: IR (four hits) and KP (two);
    - review: RU (four), VE (two), MM, HT and CF;
    - clear: IQ, YE and TR.

## Next

1. **The regime index in the plan.** The `open: null` above rests on the regime index read once by hand. The umbrella
   entry's primary is the map's page shell, whose digest does not move when the EU adds a regime, so the scheduled
   scan cannot notice a new place regime. Route `https://www.sanctionsmap.eu/api/v1/regime` into the plan for
   `eu.restrictive-measures`, pinned by digest, so a changed index appears in the next report and the enumeration is
   reproducible through the repository's scripts.
2. **COMPLY.1.**
   - Read the UK's regime regulations through legislation.gov.uk's `uksi/` reader (`gb.sanctions` is SAMLA 2018) and
     the UN Security Council resolutions, each for its own embargo generation.
   - Then Turkey's asset-freeze decisions and their importer, the Wassenaar Category 5 Part 2 and Turkey's dual-use
     communiqué.
3. **COMPLY.0.** Read the other eCFR texts of batch 19's fetch (COPPA, Section 508, the CVAA), then the CELLAR and UK
   acts.
