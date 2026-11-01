# COMPLY.0 and COMPLY.1 closed at engineering depth

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [COMPLY.0](../ROADMAP.md#slice-comply.0) and [COMPLY.1](../ROADMAP.md#slice-comply.1)
(track C, batch 32; the [parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[legal domain](../design/registers.md#legal-domain) and [review](../design/registers.md#snapshot-and-review).

## The user's decision

The user's words (2026-11-01, about 14:50 local time): "I wanted you to finish all net rows and you stalled on very
currently unnecessary things. Of course we think about the laws but it seems you are over complicating everything! I
WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!"

The order that came with it: COMPLY ends now, at engineering depth. COMPLY.0 and COMPLY.1 close on their engineering
evidence (the legal register and its guards, the rule files and generations, the sanctions importers and screening,
the cryptography inventory, `ceridc comply` and `ceridc screen`); every legal text not yet read becomes one dated
refresh entry of the legal register at its normal cadence, never a clause of either row; no more legal texts are read
in this track unless a NET or CRASH-FIX row's code needs one specific obligation. Then NET, all of it, in order.

## CI first

No failure comes from a track C change. Complete run 41298463221 (13453acb) and the complete run dispatched for the
MASAK lists, 41302856153 (92af745a), were still queued at 11:41 UTC with no job started; no push run since 438b8932 has
started.

## The refresh

The register's one dated refresh is the calendar's fourth-quarter review of 2026 (day 2026-12-31, owed by 2027-01-25
with the 25 days' lag). Its new `reads` names the 63 entries whose instruments were not read:

| Jurisdiction | Entries |
|---|---|
| TR | `tr.dual-use-export`, `tr.5651.content`, `tr.5651.game-platforms`, `tr.5651.game-platform-ratings`, `tr.5651.icta-rules`, `tr.5651.under-15`, `tr.btk.personal-data`, `tr.kvkk.ai-recommendations`, `tr.ai-bill` |
| EU | `eu.edpb.pseudonymisation`, `eu.nis2`, `eu.pld`, `eu.ai-act.transparency`, `eu.ai-act.literacy-and-prohibitions`, `eu.ai-act.gpai`, `eu.dsa`, `eu.dsm-copyright`, `eu.eaa`, `eu.cpc.virtual-currencies`, `eu.consumer-rights`, `eu.digital-fairness-act`, `eu.eecc`, `eu.avmsd` |
| GB | `gb.aadc`, `gb.osa`, `gb.ofcom-codes`, `gb.psti`, `gb.equality-act` |
| US, US-CA, US-TX, US-UT | `us.coppa`, `us.section-508`, `us.ada`, `us.cvaa`, `us.secure-software-attestation`, `us.dmca`, `us-ca.ccpa`, `us-tx.app-store-accountability`, `us-ut.app-store-accountability` |
| INT | `int.wassenaar`, `int.iarc`, `int.pegi`, `int.esrb`, `int.open-source-licences` |
| CN | `cn.pipl`, `cn.synthetic-content-labelling`, `cn.cryptography-law`, `cn.commercial-cryptography` |
| AU, BR, CA, JP | `au.online-safety`, `br.lgpd`, `br.eca-digital`, `ca.pipeda`, `jp.appi` |
| PLATFORM | `platform.apple-sdk-minimum`, `platform.apple-privacy-manifest`, `platform.apple-age-rating`, `platform.apple-accessibility-labels`, `platform.play-target-api`, `platform.play-16kb-pages`, `platform.android-developer-verification`, `platform.play-data-safety`, `platform.cab-code-signing`, `platform.steamworks`, `platform.epic`, `platform.microsoft-store` |

The same refresh carries the readings the two rows used to list: Ofcom's illegal-harms and children's codes on their
own PDFs and the Chinese Cryptography Law through the national database's JSON route (both hosts refuse the fetcher);
readers for the platform pages and the 13 sources not read (moved or wrong primaries corrected, the home pages
replaced); MASAK's table against the Official Gazette's latest decisions of each type; and the Darfur renewal, now its
own calendar event on 2026-12-11, the day resolution 2830 (2026) ends (`un.res-2830-2026.sudan-renewal`'s sunset).
The full re-research of Law No. 5651 is the scan the register already owes by 2026-11-26 (`review.next`, from the
gazette run's triggers).

## What changed

- **Scheduled reads** (`scripts/check-registers.py`). A legal calendar event may name `reads`: entries not yet read,
  each once (an unknown entry, a repeat, an empty list or an entry already read fails; the standards domain refuses the
  key, since its scans read every record). "Reading precedes closing" now accepts an unread entry whose rows are all
  Done, or which is implemented, while a scan still owed (an event after the latest run) reads it; once that scan is
  recorded without the reading, the guard fails again.
- **The review tool** (`scripts/register-review.py`). `read` drops each entry it records from the calendar's `reads`
  (an emptied list is removed); `due` prints how many instruments each scan still owed reads.
- **`int.wassenaar` is implemented.** Its only row is COMPLY.1 and its check (every export's cryptography inventory
  against its reviewed classification) is active, so a planned state would fail the guard at the flip; its evidence
  is `scripts/gen-crypto-inventory.py` and its cases. The INT rule generation is reviewed to generation 7, and
  `crd-comply-tests` now checks that `channel.distribution` answers it as proven from the committed generation.
- **A defect in the rule decoder, fixed.** The first build refused the committed INT generation (`bad-value line 52
  key state`): `crd::comply` refused an implemented rule whose text was unread or whose counsel was needed, while the
  register guard has accepted the second since the counsel decision of 2026-11-01 and now accepts the first while a
  scan owes the reading. No implemented rule existed, so it never showed. The decoder now requires only the active
  check (`rule_proof` in `engine/foundation/comply/src/rules.cpp`); the answer still reports the unread text and the
  open counsel item. `crd-comply-tests` decodes such a rule as proven, and `crd-fuzz-comply-rules`' restated
  invariant and generator follow (its generated implemented rules are now read or unread).
- **The rows** are narrowed in the user's words: the refresh, never a clause, holds the readings; the MCP surface of
  `ceridc comply` and `ceridc screen` is [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt)'s tools generated from every
  command; the CHIR authoring of the entries follows [LANG.2](../ROADMAP.md#slice-lang.2) (proposed to the user for
  the [COMPLY](../ROADMAP.md#slice-comply) parent, since both rows sit later in the table).

## Evidence

- A dry run with COMPLY.0 and COMPLY.1 set Done failed five times before the change (three watch entries and
  `int.wassenaar` unread with all rows Done, `int.wassenaar` planned with all rows Done) and passes after it.
- `python scripts/test-repository-tools.py LegalRegister RegisterReview Registers`: the new case
  `test_a_scan_still_owed_reads_what_a_closed_row_left_unread` (owed, partly owed, covered by a run, six malformed
  `reads`, the standards domain) and the extended `read` and `due` case pass.
- On the cloud machine (`cerid-track-c`, linux-gcc-debug, 0d04ead1): `crd-comply-tests`, `crd-fuzz-comply-rules`,
  `ceridc` and `crd-ceridc-tests` build with no warning; the 62 CTests matching `comply|ceridc|^crd-no-|^crd-check`
  (the ISA-isolation guard aside) pass, the rules fuzz corpus included; 20000 random generated sequences replayed
  through `crd-fuzz-comply-rules` pass its oracles.

## State

Both rows: every engineering clause is proven; COMPLY.0 waits on [NET-STD](../ROADMAP.md#slice-net-std) and table
order, COMPLY.1 on COMPLY.0. The next work is [NET.2](../ROADMAP.md#slice-net.2).
