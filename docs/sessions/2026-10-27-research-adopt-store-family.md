# RESEARCH-ADOPT: the STORE family (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

[RESEARCH-ADOPT](../ROADMAP.md#slice-research-adopt) added its last part, the STORE family: a Cerid-based store under
its own name (chosen later) for Turkish customers first, then nearby countries, sold by the user's Turkish company
([decision](2026-10-26-user-decisions-platform-floors-and-research.md)). No research proposal table covers it, so the
rows are written from that decision, areas 2, 4 and 6 to 9 of the
[publishing research](../research/2026-10-27-publishing-packaging-and-app-management-2026-comparison.md) and the
store obligations already in [COMPLY.0](../ROADMAP.md#slice-comply.0). Every part is now adopted and RESEARCH-ADOPT is
Done. The table has 1,068 rows.

## Rows (15, all Open)

| Row | Content |
|---|---|
| [STORE.0](../ROADMAP.md#slice-store.0) | Decision and ADR: scope, seller model, `crd-store`, data, services, threats |
| [STORE.1](../ROADMAP.md#slice-store.1) | Customer and developer accounts, families, age, KVKK rights, OIDC provider |
| [STORE.2](../ROADMAP.md#slice-store.2) | Catalogue, storefront (client and rendered web pages), discovery, reviews |
| [STORE.3](../ROADMAP.md#slice-store.3) | Checkout via SHIP.7, e-archive invoices, KDV, distance sales, withdrawal |
| [STORE.4](../ROADMAP.md#slice-store.4) | Windows and Linux client, library, install and update, platform services |
| [STORE.5](../ROADMAP.md#slice-store.5) | Developer side: onboarding, payouts, submission and review, the portal |
| [STORE.6](../ROADMAP.md#slice-store.6) | Mods, marketplace, community and moderation inside the store |
| [STORE.7](../ROADMAP.md#slice-store.7) | Operations, security, incident duties and support |
| [STORE.8](../ROADMAP.md#slice-store.8) | Turkish launch qualification and documentation |
| [STORE.9](../ROADMAP.md#slice-store.9) | Regional expansion, one register block per country |
| [STORE-MAC](../ROADMAP.md#slice-store-mac) | macOS client |
| [STORE-WEB](../ROADMAP.md#slice-store-web) | WASM web client and in-browser play |
| [STORE-SOCIAL](../ROADMAP.md#slice-store-social) | Friends, presence, invites, chat, voice and video |
| [STORE](../ROADMAP.md#slice-store) | Parent |
| [STORE-MOBILE](../ROADMAP.md#slice-store-mobile) | Phones, tablets and headsets that allow a third-party store |

## Placement

The family splits only where a child needs a later row, as the SHIP family does:

- STORE.0 to STORE.6 sit after EXT, where the delivery plan (`docs/design/delivery-plan.md`, not yet committed)
  keeps the family's slot in P24: they need SHIP.6, SHIP.7, COMPLY.8, SHIP.9 and EXT.5. SHIP.10 now requires
  STORE.5, since `ceridc publish` targets the store.
- STORE.7 needs OPS.1's SLOs and data inventory; STORE.8 needs SHIP.11 and SHIP. Both, and STORE.9, follow SHIP.
  QUAL.2 requires STORE.8 (the Turkish launch), not the parent, so a country decision never holds QUAL.2.
- STORE-MAC, STORE-WEB and STORE-MOBILE follow their platform rows. STORE-SOCIAL needs NET-LIVE for voice and video
  and COMPLY.9 for the electronic-communications duties of chat, so it follows NET-LIVE; the parent follows it.
- Mobile and headset clients close in STORE-MOBILE, after the parent, as the later SHIP platforms close in their own
  rows. Consoles distribute only through their platform holders' stores, so there is no console client.

When the operator applies the delivery plan, `build/ci-final/roadmap-order.json` needs these 15 IDs; the split above
is the reason list its contiguity rule asks for.

## Decisions recorded

- **One owner each.** `crd-store` consumes `crd-ship`: entitlements, licences and payments stay SHIP.7's, the updater
  SHIP.3's, store adapters and platform-service interfaces SHIP.6's. The Cerid store is a first-party adapter behind
  SHIP.6's one interface, which now also carries cloud saves, achievements, stats, leaderboards and the overlay.
- **Accounts are a real gap.** No row owned user accounts, identity, presence, achievements or cloud saves outside
  Steamworks (searched 2026-10-27). STORE.1 owns accounts and is the OpenID Connect provider Cerid products, remote MCP
  sign-in and the SHIP.9 hub use; STORE.4 owns the platform services.
- **Web storefront at launch.** The store services render the web storefront (no scripts needed, strict CSP, no
  third-party trackers, WCAG 2.2 AA and the EAA's e-commerce duties), so the Turkish launch does not wait for the WEB
  rows; STORE-WEB later adds the WASM client and in-browser play, with the rendered pages as the fallback.
- **User decisions left open, with options in STORE.0:** whether products not built with Cerid are accepted (the store
  SDK's C ABI serves both); the seller model per product line (own-name sales or an intermediary under Law No. 6563);
  the store's name; and, in STORE.9, the country list and order. The candidate countries in STORE.9 are geography, not
  legal findings; each country's law enters the register from primary sources before it opens.
- **Counsel items named, not guessed:** gift cards and a wallet against the Law No. 6493 electronic-money rules;
  whether store chat makes the store a social network provider under Law No. 5651 (the under-15 bar) or a
  communications service under COMPLY.9; developer agreements and payout withholding.
- **No new row is Later**, by the user's decision of 2026-10-27. NET-SERVER is not amended: the store services follow
  the CRASH-FIX.2 operated-service pattern on the NET.5 foundation and need no game-server fleet.

## Amendments

SHIP.6, SHIP.7, SHIP.8, SHIP.9, SHIP.10, SHIP.11, EXT.5, QUAL.2, COMPLY.0 and OWN.1 carry a dated amendment;
COMPLY.8 links the family. Requirements added: SHIP.10 STORE.5, SHIP.11 STORE.4, QUAL.2 STORE.8. The
[ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md) note records that the store is a
qualified Cerid product built on SHIP, not a second publishing path.

## Primary sources

As in the [COMPLY family](2026-10-27-research-adopt-comply-family.md), every instrument a STORE row names is read in
its current consolidated text from its primary source and dated in the COMPLY.0 register before that row closes. The
legal facts in these rows come from the research and the register seed; the research cites secondary sources for most
of them, and its uncertain facts (ICTA's rating rules, the Law No. 6563 liability ruling) are watch items.

## CI read

- Completed runs 40415857302 to 40432898137 were all cancelled with no failed job. Runs 40434280766 (queued) and
  40436504004 (pending) are push runs on d6528387 and d47348d6; no complete-tier run was dispatched.
- Run [40409214033](https://github.com/yatiyr/CRD/actions/runs/40409214033) (head cf4b8a13) completed with three
  failed jobs, linux-gcc-release (121876231573), linux-gcc-relwithdebinfo (121876231527) and linux-gcc-shipping
  (121876231690). Each failed only `simd convert_truncate gives INT32_MIN for NaN and out-of-range lanes`
  (test_simd.cpp lines 662 and 663), the GCC constant fold e9828cc2 removed. No run on e9828cc2 or later has completed,
  so the hosted confirmation is pending. Its 22 other jobs passed.
- Needs CI audit: DIAG.11c and DIAG.12 need a green complete-tier run; none exists, so both stay Needs CI.

## Local checks

check-master-plan.py and check-repository.py pass; the ctest guards pass on win-debug. Only documents changed.
