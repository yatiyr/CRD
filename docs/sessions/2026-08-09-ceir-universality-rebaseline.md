# CEIR universality quest — foundation-closure review + roadmap re-baseline (2026-08-09)

**What happened.** The user stopped the autonomous grind after CEIR-7b and issued the universality quest: prove the
CEIR/CHIR/CKIR architecture can be Cerid's common programmable substrate for essentially every computational domain
(engine, MATLAB-class, DAW, DCC, CAD/CAM, EDA/PCB, simulation, ML, media, agents) — no second graph runtime,
scheduler, scripting VM, or execution architecture per domain — then re-baseline the tracker around what the audit
finds. No broad implementation during the quest; preserve everything shipped.

**Inventory (quest §3, from code + git).** Bands 1–7b shipped, 232 tests × 4 configs, only the 7b close
uncommitted. Verified facts that decided the gap matrix: `TypeKind`/`AttrKind`/`EffectFamily`/`RegionKind` are
closed enums (attr.hpp:6 itself promised "CEIR-2/3 extend it" — never landed); the effect mask has 5 spare bits
(u32, 27 families); no stable semantic IDs anywhere (ops pointer-identified, state cells instance-keyed — ADR-0109
§6 promised identity at 1c, only SourceLoc landed); `OpInterface` registry exists but the trait/interface policy is
undocumented; no AnalysisManager/PassManager/rewrite framework; no incremental-evaluation model (and the tree
already holds ≥2 dependency graphs — render-asset-core `DependencyGraph` + CookDb edges); no transaction model;
capability contracts ownerless since 7a. Also found: the CEIR-0 band header was stale ⬜ while 0a–0z were all ✅
ACCEPTED — fixed.

**The verdict (advisor-shaped, user-decided).** Four forks put to the user:
1. **Numbering → FULL RENUMBER** (quest §113): closed 0–7 frozen; new CEIR-8/9; 7c/7d/7z→10; old 8–13→11–16; old
   14–32→17–35. Executed high→low so nothing double-renamed; ~15 ledger pins in closed rows re-struck by the same
   token renames; stragglers (bare-number refs like "CEIR-21/26", "@ 12f", "CEIR-12/13") fixed individually.
2. **Band 7 → PAUSED at 7b**: 7c consumes stable state identity (8d), 7d consumes the incremental model (8h) —
   building them first guaranteed rework.
3. **Cadence → advisor-approved, fully autonomous** ADRs (the user's gold-standard mandate, verbatim in the
   tracker banner).
4. **CEIR-9 → 8 separate proof slices** + gate (failure attribution over row economy).

**Deliverables written.**
- `docs/research/2026-08-09-ceir-universality-review.md` — the standing U-§127 report: §A current architecture
  (evidence), §B the universality gap matrix (concept → file:line → gap → owning row), §I the 17-domain proof-matrix
  skeleton (9z fills it), §N the re-baseline record, §O honest remaining limitations + the pre-closure U-§126
  answer.
- The re-baselined tracker: the re-baseline banner + mapping table; **CEIR-8 FOUNDATION CLOSURE** (8a open-world
  types · 8b open-world attrs · 8c effect-mask u64 + extensible effect locations · 8d stable semantic identity ·
  8e trait/interface split + region-kind reservation · 8f capability contracts + domain/safety split + typed time
  domains · 8g compiler-infrastructure skeleton · 8h incremental-evaluation unification · 8i transactions · 8z the
  U-§121 20-item gate); **CEIR-9 UNIVERSALITY VALIDATION** (9a notebook/incremental · 9b DAW/timeline · 9c DCC
  modifier graph · 9d CAD parametric+transaction · 9e PCB/EDA+external provider · 9f game/ECS effects · 9g agent
  transaction · 9h the MANDATORY external-plugin proof (U-§116, zero central-enum edits, grep-gated) · 9z the
  17-domain matrix + the U-§126 answer); **CEIR-10** (the re-homed 7c/7d/7z, contracts verbatim, foundation inputs
  named); capability-contract flags in 7a/7b struck in place (owned by 8f).
- context.md focus rewritten; memory (master-spine + loop-grant) updated to the new numbering.

**Key framing (why this is not scope creep).** CEIR-8 pays RECORDED debts (the ADR-0109 §6 identity IOU, the
attr.hpp attr-kinds IOU, the 7a capability flag, the 7a instance-keyed-state note); CEIR-9 is proof, not features
(mock dialects, U-§115); nothing shipped is rewritten — every foundation slice extends the live substrate with all
232 prior tests green (format changes are versioned recooks, named per slice).

**State.** The autonomous loop is STOPPED (user direction) — resumes on the user's word at **CEIR-8a** (the
open-world type model). The 7b close is still the uncommitted working set; this quest's edits are docs-only.

## Proposed commit — the re-baseline (user commits; NO AI trailer; docs-only, rides with or after the 7b commit)

```
docs(ceir): universality-quest re-baseline -- foundation closure + validation bands

- docs/research/2026-08-09-ceir-universality-review.md (NEW): the standing U-s127
  report -- current-architecture evidence, the universality gap matrix (closed
  type/attr/effect/region vocabularies, pointer identity, missing pass/incremental/
  transaction/capability models -- several recorded IOUs: ADR-0109 s6 identity @1c,
  attr.hpp CEIR-2/3 attr kinds, the 7a capability flag), the 17-domain proof-matrix
  skeleton, the re-baseline record, honest remaining limitations.
- D-007 tracker FULL-RENUMBERED (user verdict, quest s113): bands 0-7 frozen (band-0
  stale header fixed -- 0a-0z were all accepted; band 7 PAUSED at 7b); NEW CEIR-8
  FOUNDATION CLOSURE (open-world types/attrs, effect u64 + extensible locations,
  stable semantic identity, trait/interface + region reservation, capabilities +
  domain/safety + time domains, AnalysisManager/PassManager/rewrite/DiagnosticEngine
  skeleton, incremental unification, transactions, the U-s121 20-item gate); NEW
  CEIR-9 UNIVERSALITY VALIDATION (8 mock-domain proofs + the mandatory external-
  plugin proof + the 17-domain gate); 7c/7d/7z -> CEIR-10; old 8-13 -> 11-16; old
  14-32 -> 17-35; mapping table + ledger pins re-struck in closed rows; capability
  contracts now OWNED (CEIR-8f).
- context.md focus + session log. Loop stopped; resumes at CEIR-8a on user's word.
```
