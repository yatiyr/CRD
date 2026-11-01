# COMPLY.0: the remaining mevzuat.gov.tr laws, read

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.0](../ROADMAP.md#slice-comply.0) and [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C,
batch 17; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[official texts](../design/registers.md#official-texts). The fetch read here is the first read's
([report](2026-11-01-legal-register-first-read.md)); the user's approval of it, quoted, is in
[the first read's note](2026-11-01-comply-0-first-read.md#the-users-decision).

## CI first

No failure comes from a track C change that main has not already fixed.

- **Run 41254049879 (31404ec4), the complete run batch 12 dispatched.** win-tidy failed on the four findings in
  `test_arguments.cpp` that a40af1bf fixed. linux-gcc-release and linux-gcc-shipping failed at the LTO link of
  `crd-compression-tests` on track B's `test_deflate.cpp`, which 95ec6965 rebuilt. Every other finished job passed;
  linux-gcc-debug, linux-gcc-asan (shard 2), linux-clang-tsan, linux-gcc-relwithdebinfo, linux-gcc-public-checks,
  win-debug, win-relwithdebinfo, win-asan (shard 1) and win-shipping-profile were still running.
- **Run 41247045494 (67977fda).** linux-gcc-asan passed, so all three Linux runs of the concurrent comply tests are
  green; its nine Windows failures are the C2797 that d9be0cfd fixed.
- Every push run since is cancelled by the next push before its jobs start. A complete run is dispatched on this
  batch's head once it is on main.

<a id="reading"></a>
## What was read

The first read fetched 16 consolidated laws from mevzuat.gov.tr; the KVKK and the Cybersecurity Law were read then,
and Law No. 5651 waits for the full re-research the gazette reading owes. This batch read the other 13, each
article an entry cites against its obligation, and corrected the register where the text says otherwise. The
reader's version and the SHA-256 are the fetch's.

| Law (version read) | Entries | What the text says, and what changed |
|---|---|---|
| No. 5237, Penal Code (`2026-08-08 (7593)`) | `tr.tck.personal-data-offences` (recorded), `tr.tck.information-system-offences` (recorded) | Articles 135 (recording, half as much again for the special categories), 136 (giving, disseminating or obtaining), 137 (aggravated), 138 (not destroying after the legal periods), 139 (complaint) and 140 (security measures for legal persons). Article 243 is access or remaining in a system and, since Law No. 6698, monitoring its data transfers (243(4)); 244 is hindering a system and altering, deleting or inserting data. Article 245 is bank and credit card misuse, not a system offence: the entry cited 243-245 and now cites 243, 244, 245/A (devices, programs and codes made exclusively for these offences, which bears on security tools) and 246. In force on 1 June 2005 (Article 344). |
| No. 6415, terrorist financing (`2020-12-27 (7262)`) | `tr.asset-freeze` (not recorded) | Article 5: the President's decisions implementing UN Security Council resolutions 1267, 1988, 1989 and 2253, published in the Official Gazette and applied without delay; Article 6: a foreign state's request, decided by the President on reciprocity; Article 7(3): domestic freezes by the Ministers of Treasury and Interior, approved by a court within 48 hours; Articles 11-12: a decision binds on its publication; 12(3): the designated person and anyone owing to or owed by one report to MASAK within 30 days; 13(1): no one asked to apply a freeze may make or facilitate a transaction; 15: imprisonment for not applying one or for providing funds or financial services to a designated person, and an administrative fine of TRY 10 000 to 2 million on the legal person. The articles are now cited. Law No. 7262's own proliferation freezes are not in this fetch, so the entry is not recorded. |
| No. 6112, broadcasting (`2023-12-27 (7491)`) | `tr.6112.internet-broadcasting` (not recorded) | Article 29/A (added by Law No. 7103, in force on 27 March 2018): a media service provider offering radio, television or on-demand services only on the internet needs an RTÜK licence and a platform operator transmitting them a transmission authorisation; 29/A(4) puts individual communication, platforms not dedicated to such services and pure hosts outside it; 29/A(5) leaves the procedure to a joint RTÜK and BTK regulation. The obligation says so and `effective` is the article's day, not the law's. The entry names the regulation, which was not read. |
| No. 6563, e-commerce (`2024-10-24 (7529)`) | `tr.6563.commercial-messages` (recorded), `tr.6563.etbis` (not recorded) | Article 6: prior consent, written or electronic (traders and craftsmen excepted); Article 7: the content matches the consent and identifies the sender with contact details; Article 8: refusal at any time, easy and free, honoured within three business days. The message management system (IYS) is Article 11(5): a consent not transferred to it is invalid. The entry now cites 11(5). ETBIS is defined in Article 2(1)(ı) and Article 11(4) makes reporting to the Ministry's system a duty, but registration itself is the regulation's, which was not read. |
| No. 6502, consumer protection (`2026-06-24 (7587)`) | `tr.6502.distance-contracts` (not recorded) | Article 48: pre-contract information as the regulation lists, with the seller's burden of proof; 14 days to withdraw, extended up to a year when its information was missing (48(4)); since Law No. 7392 a marketplace intermediary shares liability for the pre-contract information and keeps the records (48(5)-(6)); 48(7) leaves exceptions, among them the digital-content waiver, to the regulation. Article 49 is financial services and no longer cited. |
| No. 5070, e-signature (`2021-01-28 (7263)`) | `tr.5070.e-signature` (recorded) | The law's term is the secure electronic signature, made with a secure creation device on a qualified certificate (Article 4); Article 5 gives it a handwritten signature's effect, except for transactions needing an official form or ceremony and guarantee contracts other than bank letters of guarantee and insurers' surety bonds. The obligation used the EU term "qualified" and omitted the exceptions; corrected. |
| No. 5846, intellectual works (`2021-12-21 (7346)`) | `tr.5846.software-works` (recorded) | Article 2: computer programs and their preparatory designs are works, the ideas and principles under them and their interfaces are not; Article 18: an employee's work made on duty is exercised by the employer unless agreed otherwise (now cited); Article 48: transfer and licence; 49: onward transfer only with the author's written consent; 52: contracts on financial rights are written and list each right. |
| No. 6769, industrial property (`2020-07-22 (7251)`) | `tr.6769.industrial-property` (recorded), `tr.turkpatent.trademarks` (not recorded) | Trademarks: protection only by registration (Article 7(1)), the holder's rights (7(2)), infringement (29); application (11), ten-year term renewed in the last six months (23). Patents: a computer program as such is no invention (82(2)(c)); the holder's rights (85). Both entries cited no article and now do. The trademark entry also names the EUIPO filing, whose Regulation (EU) 2017/1001 was not read. |
| No. 5378, persons with disabilities (`2023-06-22 (Anayasa Mahkemesi)`) | `tr.5378.accessibility` (recorded) | Article 3(1)(f): accessibility includes information and communication technology; 3(1)(g): the TSE standards; Article 4/A: discrimination is prohibited and reasonable accommodation is made; Article 7: information services and ICT are made accessible. The ICT sentence is Law No. 6518's wording, in force on 19 February 2014, which `effective` now holds. |
| No. 3065, VAT (`2026-07-24 (7590)`) | `tr.3065.vat` (recorded) | Article 1: deliveries and services in Turkey; Article 6(b): a service is in Turkey when performed or used there; Article 9(1), second sentence (Law No. 7061, from 1 January 2018): a provider without residence, office or seat in Turkey selling electronic services to natural persons not registered for VAT declares and pays the VAT itself, under the Ministry's scope and procedure; Article 28: the President sets the rate. Invoicing is `tr.213.e-archive`'s; the obligation no longer claims it. |
| No. 7194, digital service tax (`2023-05-18 (Anayasa Mahkemesi)`) | `tr.7194.digital-service-tax` (not recorded) | Article 1: advertising, digital content including games and in-game applications, interaction platforms and intermediation; Article 4: exempt while the previous year's revenue was below TRY 20 million in Turkey or EUR 750 million worldwide, counted for the group; Article 5: 7.5 %; Article 6: monthly, declared by the end of the next month; Articles 1-7 in force on 1 March 2020. Article 8 amends another law and is no longer cited. The consolidated text's note says Presidential Decision No. 10767 (Official Gazette of 25 December 2025, No. 33118) applies 5 % from 2026 and 2.5 % from 2027. The decision itself was not fetched (the gazette page timed out from this host), so the entry is not recorded until it is read. |
| No. 6493, payment services (`2020-06-18 (7247)`) | `tr.6493.payment-institutions` (recorded) | Article 13(2): only banks and payment service providers provide payment services; 14 and 18: payment and electronic money institutions act with the Central Bank's licence; 28: acting without one is an offence; 12(2)(h) and 18(5): an instrument or prepaid balance usable only in the issuer's own network or for a limited range is outside the law until the Central Bank brings it in by volume, which bears on store credit and wallets. The entry cited no article and now does. |
| No. 5809, electronic communications (`2026-07-24 (7590)`) | `tr.5809.authorisation` (not recorded), `tr.btk.encryption` (not recorded) | Article 9: authorisation by notification before operating, with a usage right where numbers or frequencies are needed; 63(1): operating without notification is an offence; the authorisation provisions took effect six months after publication (Article 68), on 10 May 2009, which `effective` now holds. Encryption is Article 39, not Article 5 (the Ministry's powers): the BTK sets the procedure for coded or encrypted communication within an electronic communication service, and 63(6) punishes a breach. The entry cites Article 39 and 63(6) now. Both entries rest on BTK regulations not read. |

**The rule for recording.** `read` records an entry only when every instrument it names was read, as for
`tr.kvkk.transfer-abroad` in the first read: nine entries are recorded; the other eight are corrected and wait for
the instrument each names (Law No. 7262, the RTÜK and BTK internet broadcasting regulation, the e-commerce service
provider regulation, the Distance Contracts Regulation, Regulation (EU) 2017/1001, Presidential Decision No. 10767 and
the BTK authorisation and encryption rules).

## What changed

- `cmake/legal-register.json`: 17 entries corrected as above; the nine recorded ones carry the version, the fetch day
  and the SHA-256 of the text read, with this note as their verified source.
- `assets/comply/tr.rules.toml`: the TR rule generation is regenerated and reviewed (generation 5), its record in
  `rule_generations` and the generated-source manifest refreshed.

## Verification

- **The reading.** `register-review.py read` recorded the nine entries from the first read's fetch, all or nothing;
  the guard then held each one's verified source to this note.
- **The register and generations.** `check-registers.py` passes (106 entries, 93 pinned); `gen-comply-rules.py
  --check` finds all 11 generations current after the TR review; `check-generated.py` passes with the refreshed
  manifest.
- **Python.** `Registers`, `LegalRegister`, `ComplyRules`, `CryptoInventory`, `SanctionsLists`, `RegisterReview` and
  `RegisterIds` pass: 64 cases.
- **C++.** On the track's machine (linux-gcc-debug, 39d4e053), `crd-comply-tests`, `crd-ceridc-tests` and `ceridc`
  built with no warning, and the 158 scoped CTests passed, among them the committed generations decoding and binding
  the crash pool, `crd-comply-rules-drift`, `crd-check-registers`, `crd-generated-sources` and
  `crd-repository-tools`. A first run on a commit without this note failed `crd-check-registers` on the nine sources
  citing it, so the note is in the same commit as the register. The machine is stopped.

## Next

1. COMPLY.0, host by host: the CELLAR acts not yet read (NIS2, the PLD, the AI Act, the DSA, the DSM directive, the
   EAA, the consumer rights directive, the EECC, the AVMSD), the UK acts, COPPA, Section 508, the CVAA, OFAC's
   chapter V and the Wassenaar Category 5 Part 2.
2. The instruments this reading named: Law No. 7262 and Presidential Decision No. 10767 from the Official Gazette, and
   the secondary instruments on mevzuat.gov.tr (the KVKK By-Law on transfers abroad and the Communiqué on the
   obligation to inform, the Distance Contracts Regulation, the e-commerce service provider regulation, the RTÜK and
   BTK regulation, the BTK authorisation and encryption rules).
3. The full re-research of Law No. 5651, readers for the platform pages and the hosts not read.
4. COMPLY.1: Turkey's asset-freeze decisions and their importer, now that Law No. 6415's articles are read.
