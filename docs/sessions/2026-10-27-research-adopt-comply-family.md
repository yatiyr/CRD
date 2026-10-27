# RESEARCH-ADOPT: the merged COMPLY family (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

[RESEARCH-ADOPT](../ROADMAP.md#slice-research-adopt) finished Part 4: the COMPLY per-domain children and the parent.
Per the operator note, Part 4's COMPLY family and the
[publishing research](../research/2026-10-27-publishing-packaging-and-app-management-2026-comparison.md)'s COMPLY.1
and COMPLY.2 are one family with no duplicate IDs and no lost obligation. The table now has 1,033 rows.

## ID mapping

| Row | Content | Placed |
|---|---|---|
| [COMPLY.0](../ROADMAP.md#slice-comply.0) | Register and checks; absorbs the research's COMPLY.1 (P-1) | Before NET.2 |
| [COMPLY.1](../ROADMAP.md#slice-comply.1) | Cryptography and export control | Before NET.2 |
| [COMPLY.2](../ROADMAP.md#slice-comply.2) | Privacy and data protection | Before CRASH-FIX.1 |
| [COMPLY.3](../ROADMAP.md#slice-comply.3) | Security and vulnerability duties | Before SUPPLY.1 |
| [COMPLY.4](../ROADMAP.md#slice-comply.4) | Licences and intellectual property | Before SUPPLY.1 |
| [COMPLY.5](../ROADMAP.md#slice-comply.5) | Online safety, user content and children | Before EXT.5 |
| [COMPLY.6](../ROADMAP.md#slice-comply.6) | Artificial intelligence | Before AGENT.1 |
| [COMPLY.7](../ROADMAP.md#slice-comply.7) | Accessibility | Before I2D-3.d |
| COMPLY.8 | Store compliance kit (the research's COMPLY.2, P-10) | After SHIP.7 |
| [COMPLY.9](../ROADMAP.md#slice-comply.9) | Electronic communications and broadcasting | Before NET-LIVE |
| [COMPLY](../ROADMAP.md#slice-comply) | Parent | After COMPLY.9 |

COMPLY.1 to COMPLY.7 and COMPLY.9 carry COMPLY.0's domains; COMPLY.8 (consumer law, paid random items, age ratings
and the runtime kits) lands with the publishing rows in the next batch. When they land, every "COMPLY.1" in the
research's proposal texts means COMPLY.0 and every "COMPLY.2" means COMPLY.8. The parent then adds COMPLY.8 to its
Requires (COMPLY.8 sits before it).

## Placement and decisions

- Each child sits directly before the first row it governs, and that row requires it: NET.2 (COMPLY.1 in place of
  COMPLY.0), CRASH-FIX.1 (COMPLY.2 in place of COMPLY.0), SUPPLY.1 and SEC.1, EXT.5, AGENT.1 and AI.1, I2D-3.d and
  A11Y-GAME, and NET-LIVE. OPS.1 and ANTICHEAT require COMPLY.2; 4.2d and COLLAB.1 require COMPLY.5. COMPLY.0 now
  requires NET-STD, whose register it shares.
- P-1 asked for its register after SEC.1. The register already stands before NET.2, because no data may leave a
  machine before it exists; the earlier position is the complete option. The CRA checks P-1 tied to SEC.1 are
  COMPLY.3's, which SEC.1 requires.
- Turkish law is a block inside every child, never a separate child. Instruments COMPLY.0 named are assigned: KVKK
  6698 and 7499 and the Penal Code's data offences to COMPLY.2; 7545 and the IT offences to COMPLY.3 (its product
  and export permits also to COMPLY.1); 5846 and 6769 to COMPLY.4; 5651 to COMPLY.5 and COMPLY.8; 5378 to COMPLY.7;
  5809, BTK and RTÜK (Law No. 6112) to COMPLY.9 (BTK encryption rules also to COMPLY.1); 6502, 6563 and 5070 to
  COMPLY.8 and the STORE rows.
- COMPLY.0 also carries the operator's store obligations: Law No. 6493 payment institutions, merchant of record and
  e-archive invoicing, KDV and the digital service tax, ETBIS, distance sales and withdrawal (6502), KVKK, the 5651
  game-platform duties and one register block per country the store enters.
- New rows are Open, not Later, by the user's decision of 2026-10-27 that no row stays Later; this includes COMPLY.9
  and the parent beside NET-LIVE.
- Registrations, filings, accounts and counsel reviews are user actions taken by the publisher (the user's Turkish
  company) after everything is implemented and qualified. Each row names its own and none blocks implementation.

## Primary sources

Each child requires every instrument to be read in its current consolidated text from its primary source and dated
in the register before the row closes. The research cites law-firm and news summaries for most legal facts, so its
dates are not primary readings. A check from this host on 2026-10-27: EUR-Lex (CELEX 32024R2847, the CRA) returned
an empty page to the fetcher, and mevzuat.gov.tr showed only the gazette record of Law No. 7545 (Official Gazette
2025-03-19, No. 32846), with the text in its document links. The readings belong to the register entries COMPLY.0
and its children write, where each one carries its source and date. Facts the research marks uncertain (ICTA's
rating rules, the Law 6563 ruling, the Texas law, the AI Act marking date) are watch items.

## Delivery plan

The proposed delivery plan (`docs/design/delivery-plan.md`, not yet committed) refuses new COMPLY rows until its
order JSON names them. COMPLY.1 to COMPLY.7, COMPLY.9 and COMPLY need to be added to its groups at the positions above.

## Left

COMPLY.8 after SHIP.7; the publishing rows SHIP.0 to SHIP.11, the SHIP parent and the six platform rows; amendments
P-22 to P-52 with the EXT.5 move; the STORE family.

## CI read

Every completed run from 40416850706 to 40432785080 was cancelled with no failed job; 40431395983 and 40432785080 ran
no job. The in-progress run 40409214033 (head cf4b8a13) failed `simd convert_truncate gives INT32_MIN for NaN and
out-of-range lanes` on linux-gcc-relwithdebinfo, linux-gcc-release and linux-gcc-shipping, each with lanes 2 and 3
reading 0x7fffffff and 0, the fold e9828cc2 removed. No complete-tier run is green, so DIAG.11c and DIAG.12 stay
Needs CI.
