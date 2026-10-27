# RESEARCH-ADOPT Part 5: publishing, packaging and app management (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

[RESEARCH-ADOPT](../ROADMAP.md#slice-research-adopt) applied the proposal table ("Gaps and proposed roadmap
changes") of the
[publishing research](../research/2026-10-27-publishing-packaging-and-app-management-2026-comparison.md), which the
user approved on 2026-10-27. Placement is by anchor, because the table's numbers were stale. The table now has 1,053
rows. RESEARCH-ADOPT stays Partial: the STORE family is left.

## New rows (20)

- After DATA-TRUST: [SHIP.0](../ROADMAP.md#slice-ship.0), the decision and ADR (`crd-ship`, the release data model,
  release and update trust, the changeability matrix).
- After EDITOR.5: [SHIP.1](../ROADMAP.md#slice-ship.1) to [SHIP.3](../ROADMAP.md#slice-ship.3) (headless export,
  crdr v2, updates and rollout); EDITOR.6 is their client.
- After EDITOR.7: [SHIP.4](../ROADMAP.md#slice-ship.4) and [SHIP.5](../ROADMAP.md#slice-ship.5) (Windows and Linux
  packages); the EDITOR parent requires both.
- After DX.4: [SHIP.6](../ROADMAP.md#slice-ship.6), [SHIP.7](../ROADMAP.md#slice-ship.7),
  [COMPLY.8](../ROADMAP.md#slice-comply.8), [SHIP.8](../ROADMAP.md#slice-ship.8), [SHIP.9](../ROADMAP.md#slice-ship.9),
  then EXT.5 and EXT (moved), then [SHIP.10](../ROADMAP.md#slice-ship.10).
- After OPS.1: [SHIP.11](../ROADMAP.md#slice-ship.11) and the [SHIP](../ROADMAP.md#slice-ship) parent, before
  QUAL.2, which requires it.
- After MAC: [SHIP-MAC](../ROADMAP.md#slice-ship-mac). After WEB: [SHIP-WEB](../ROADMAP.md#slice-ship-web) and
  [SHIP-ENTERPRISE](../ROADMAP.md#slice-ship-enterprise).
- After 6.0, 6.1 and 6.3: [SHIP-CONSOLE](../ROADMAP.md#slice-ship-console),
  [SHIP-MOBILE](../ROADMAP.md#slice-ship-mobile) and [SHIP-XR](../ROADMAP.md#slice-ship-xr).

The research's COMPLY.1 is COMPLY.0 and its COMPLY.2 is COMPLY.8
([merge](2026-10-27-research-adopt-comply-family.md)). Each row carries its proposal text, the gap text of its
area, its peers and budgets, the downloads that need the user's approval first and the user actions (accounts,
certificates, registrations, contracts). New rows are Open.

## Requirements beyond the proposal table

Every one names a row that sits earlier and that the proposal text already uses:

- SHIP.0: OWN.1 (registers `crd-ship`) and DATA-TRUST (it sits after it).
- SHIP.1: CRASH-FIX.2 (symbol store), SCRIPT.4 (shipping allowlists), VCS.2 (cook-cache keys).
- SHIP.2: SUPPLY.3 (signed manifest) and OWN.1 (codec owner).
- SHIP.7: COMPLY.1 (licence-file cryptography and export control), COMPLY.2 (payment and receipt data) and NET.2,
  whose primitives give the hybrid post-quantum licence path; the proposal's "when NET.2's primitives exist" is
  settled because NET.2 sits earlier.
- COMPLY.8: the COMPLY children whose domains it generates for (COMPLY.1, .2, .4, .5, .6, .7).
- SHIP.8: I18N.1 (store listings). SHIP.9: DATA-TRUST.2 (countersignatures), COMPLY.5 (notice and action), COMPLY.8.
- SHIP.10: DX.2 (templates). SHIP.11: CEIR-JIT.3 (AOT selection). SHIP: every child and COMPLY.8.
- SHIP-CONSOLE: CEIR-JIT.3 (AOT-only builds). EDITOR: SHIP.4 beside SHIP.5 ("installers on both platforms").
- DX.1: SHIP.2 (`ceridc package` produces its output). 6.3: CEIR-JIT.3, as 6.0 and 6.1 already have.

OPS.1 sits after SHIP.3 and COMPLY.8, which read its consented signals and data inventory. Those rows say the signals
join when OPS.1 lands, and SHIP.11 (after OPS.1) qualifies the rollout gates and the generated labels against them.

## The EXT.5 move

P-38 moves EXT.5 after SHIP.9, since the marketplace needs SHIP.7, COMPLY.8 and SHIP.9. The EXT parent sat directly
after EXT.5 and required only EXT.3, and the validator refuses a parent before its children, so the parent moved too
and now requires EXT.3, EXT.4 and EXT.5: a parent closes on every child. AGENT.1 was the parent's only dependent. It
needs the extension substrate for its command producers, not the marketplace, so it now requires EXT.3 and EXT.4,
and its text records why.

## Amendments

P-22 to P-52 were applied in place to EDITOR.6, EDITOR.7, EDITOR, QUAL.2, SUPPLY.1 to .4, SEC.1, CEIR-JIT.3,
CRASH-FIX.1, CRASH-FIX.2, OPS.1, OPS.2, OPS.3, EXT.4, EXT.5, 4.2d, NET-SERVER, HW-RESILIENCE.1, DOC.2, API-STAB.0,
DATA-TRUST.2, AGENT.3, ANTICHEAT, I18N.1, A11Y-GAME, DX.1, JOBS-PORT.2, 6.0, 6.1, 6.3, EMBED.2 and PORTFOLIO.1.
Requirements added: EDITOR.6 SHIP.3; EDITOR SHIP.4 and SHIP.5; QUAL.2, OPS.2, NET-SERVER and PORTFOLIO.1 SHIP;
CEIR-JIT.3 SHIP.0; 4.2d SHIP.6; JOBS-PORT.2 SHIP.5; EMBED.2 SHIP.3; DX.1 SHIP.2; EXT.5 SHIP.7, COMPLY.8 and SHIP.9.
6.0, 6.1, 6.3 (P-50) and EXT.5 (P-38) changed from Later to Open; every other Later row waits for the re-plan.

Gap text with no table row went onto the rows the research names: signatures logged and bound to provenance and the
eligible entities for Artifact Signing (SUPPLY.3); the OS floor declared and tested per package (SHIP.4, SHIP.5);
store metadata as an authored asset (SHIP.6); prices and offers as signed generations (SHIP.7); the install-size board
and the anti-tamper adapter rule (SHIP.2); the ICTA 15-day request window (COMPLY.8); the time-to-live and
install-latency peers (SHIP.8, SHIP.9, SHIP.10).

[COMPLY.0](../ROADMAP.md#slice-comply.0) and the [COMPLY](../ROADMAP.md#slice-comply) parent now link COMPLY.8, and
the parent requires it. [OWN.1](../ROADMAP.md#slice-own.1) registers `crd-ship` and decides where the crdr v2 codecs
live.

## ADR and design notes

- [ADR-0038](../decisions/0038-cooked-binary-container-format.md#amendment-2026-10-27-crdr-v2-content-packaging):
  crdr v2 (per-chunk checksums, a signed Merkle manifest, aligned and content-defined chunks, mount layers, the codec
  set with its SSE2 CPU path, honest optional encryption).
- [ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md): published means installed,
  updated, rolled back and uninstalled through SHIP; irreversible publication needs a recorded human approval.
- [Execution contract](../design/renderer-ui-execution-contract.md#publishing-and-application-acceptance): publishing
  is headless first and the editor's export is one client.

## Delivery plan

The proposed delivery plan (`docs/design/delivery-plan.md`, untracked, not this batch's file) needs the new IDs in its
order: SHIP.0 to SHIP.11, SHIP, COMPLY.8, SHIP-MAC, SHIP-WEB, SHIP-ENTERPRISE, SHIP-CONSOLE, SHIP-MOBILE and SHIP-XR at
the positions above, and EXT.5 and EXT after SHIP.9.

## Left

The STORE family: a Cerid-based store (its own name later) for Turkish customers first, then nearby countries, built
on SHIP.6, SHIP.7, COMPLY.8, SHIP.9 and EXT.5, with COMPLY.0's store obligations (Law No. 6493, e-archive invoicing,
KDV and the digital service tax, ETBIS, Law No. 6502, KVKK and the 5651 game-platform duties).

## CI read

The completed runs from 40415857302 to 40432898137 were all cancelled with no failed job. The in-progress run
40409214033 (head cf4b8a13) has three failed jobs, linux-gcc-release, linux-gcc-relwithdebinfo and
linux-gcc-shipping, each failing only `simd convert_truncate gives INT32_MIN for NaN and out-of-range lanes`
(test_simd.cpp lines 662 and 663), the GCC fold e9828cc2 removed. No complete-tier run is green, so DIAG.11c and
DIAG.12 stay Needs CI.

## Local checks

check-master-plan.py and check-repository.py pass. In the first run of the ctest guards on win-debug (`-j 8`),
`crd-check-allocation-results` printed nothing and hit its 120 s timeout. It did not reproduce: the script alone took
2.5 s, the test alone 2.7 s, and a second run of all 15 guards passed in 6.5 s. The batch changed only documents, and
the script reads only sources, so the stall is recorded here as a non-reproducing local event, not chased.
