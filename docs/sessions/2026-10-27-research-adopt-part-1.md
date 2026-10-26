# RESEARCH-ADOPT Part 1: engine, GPU, rendering and geometry (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

[RESEARCH-ADOPT](../ROADMAP.md#slice-research-adopt) applied every row of the Part 1
[proposal table](../research/2026-10-26-state-of-the-art-2026-comparison.md#gaps-and-proposed-roadmap-changes), which
the user approved on 2026-10-26. Placement is by anchor; the research's Order numbers were stale after the
SIMD-DISPATCH, HW-RESILIENCE and CRASH-FIX insertions. The table was renumbered (944 rows).

## New rows

- After CORE-USE: [JOBS-QOS](../ROADMAP.md#slice-jobs-qos). This is the primary option. The alternative, amending
  JOBS-HARD.2, was not taken because that row is Later and the frame graph and audio need QoS sooner. JOBS-HARD.2 now
  requires JOBS-QOS and qualifies it.
- After RAH-0: [GPU-TIER.0](../ROADMAP.md#slice-gpu-tier.0), [RAH-SYNC](../ROADMAP.md#slice-rah-sync),
  [DX12-AGILITY](../ROADMAP.md#slice-dx12-agility), [VK-PROFILE](../ROADMAP.md#slice-vk-profile).
- [RAH-5.d](../ROADMAP.md#slice-rah-5.d) (RAH-5 now closes on it); [RAH-7.e](../ROADMAP.md#slice-rah-7.e) and
  [RAH-7.f](../ROADMAP.md#slice-rah-7.f) (RAH-7 closes on .f); [GPU-FORENSICS](../ROADMAP.md#slice-gpu-forensics)
  after RAH-8.
- After WORLD.1: GEOM-K.0 to .4, GEOM-EXACT.1 and .2, GEOM-QUAL, NAV.0, .1, .2, .3, .5 and .6. GEOM-K.0 takes the
  primary position. The alternative position after RENDERER-RUNTIME was not taken, because the user approved the
  proposals as written.
- After EYLEM-v1k: NAV.4 and NAV.7.

Each new row carries the full proposal text. Some rows also carry the matching area's gap text: the generation rules
on RAH-5.d, the in-flight generation names on GPU-FORENSICS, live tile rebuilds on NAV.0 and SM 7 SPIR-V on
DX12-AGILITY. A row also states when a download needs the user's approval first.

## Requirements added beyond the table

- RAH-7.e requires JOBS-QOS, whose compile-job QoS class it uses. NAV.1 requires CGP-3, which does its GPU
  voxelization.
- RAH-1a.2 requires GPU-TIER.0, so every RAH child follows the ladder.
- Amended rows take only requirements that sit earlier in the table. "RAH-7.a to .d requires RAH-7" stays as text,
  because a parent closes after its children.

## Amendments

The tier fallback clause for every RAH child is in the execution contract's new
[GPU capability tiers and fallback](../design/renderer-ui-execution-contract.md#gpu-capability-tiers-and-fallback)
section. Each RAH-1 to RAH-8 parent points at that section. The clause is not copied into thirty child rows.
RAH-1a.1 (Recorded) is unchanged.

Amended rows: SIMD-DISPATCH, JOBS-PORT.2 (the ARM64 release row for SVE, SVE2 and SME), OWN.1, RAH-2.a, RAH-2.c,
RAH-7.a to .d, RAH-8.a, MLR-render.1, MLR-render.2, MLR-2.runtime, TPR-1.runtime, PRESENT.2, RAH-4.c, RTX-0.runtime,
RTX-4.runtime, PQP-4.renderer, TPR-2.runtime, INTEROP.1, MAT-0.runtime, GVA-5.runtime, ANIM.2, PHYS-QUAL.plan,
QUAL.1, MAC.1, CMD.4, AGENT.3, I2D-2.b, I2D-6.b, BREP.2 and OPS.1. Rows that shared one proposal got the part that
belongs to each. [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md) already recorded the SIMD-DISPATCH amendment.

## OPS.1 and the crash-to-fix rows

The OPS.1 amendment puts a consented crash and telemetry service with symbol upload before the first desktop release.
OPS.1 changed as follows:
- It moved directly before QUAL.2, and its state changed from Later to Open.
- It requires DIAG and HW-RESILIENCE.2 instead of COLLAB and EMBED.
- Its distributed, collaborative and embedded extension moved to OPS.3, which now requires COLLAB and EMBED. Nothing
  was dropped.

The operator note's rules applied:
- The five CRASH-FIX rows moved with OPS.1, directly after it, and are now Open. QUAL.2 requires OPS.1 and CRASH-FIX.
- The research overlaps were folded in. CRASH-FIX.1 takes GPU dumps from GPU-FORENSICS. CRASH-FIX.3 treats pack
  contents as untrusted data under AGENT.3's hostile-input corpus.
- Part 3's AGENT.3 guardrail amendment still lands with Part 3.

## Gap text that has no proposal row

Superluminal as a CPU sampling peer (area 4) has no proposal row and no row was invented for it. Slang as a measured
peer and import route (area 1) is Part 3's CKIR-LANG.1.

## Left

[Part 2](../research/2026-10-26-beyond-games-2026-comparison.md#gaps-and-proposed-roadmap-changes) (beyond games) and
[Part 3](../research/2026-10-26-building-with-cerid-2026-comparison.md#gaps-and-proposed-roadmap-changes) (building
with Cerid): every new row and amendment, plus the design references and ADR notes they cite. Their "(P1)" references now resolve
to GEOM-K.3, JOBS-QOS, GPU-TIER.0 and NAV.3.
