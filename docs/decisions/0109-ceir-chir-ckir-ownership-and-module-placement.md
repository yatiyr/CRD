# ADR-0109 — CEIR / CHIR / CKIR ownership, the one-way layer contract, and `crd-ceir` module placement

**Status:** **ACCEPTED** (2026-08-07, user-approved at the CEIR-0c gate) — the D-007 **CEIR band**. Builds on
ADR-0108 (Accepted — the owned language stack) and formalizes the layer contract + the module the whole detour is
implemented in. **This ADR is now the binding contract CEIR-1 implements** (the names + edges below are fixed).
Refined by CEIR-0d (native intrinsics) + CEIR-0e (CHIR-0 design note).
**Phase:** D-007 (CEIR programme). Law: mission `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md`
§2 (CEIR vs CKIR), §4 (why CHIR is above CEIR), §85 (CKIR integration), §10 (semantic identity), §69 (providers).
**Relationships:** extends ADR-0101/0103 (the IR-is-source-of-truth + gpu-context-owns-every-program invariants) up
one layer; consumes ADR-0108. Inventory evidence: `docs/design/ceir-0a-execution-path-inventory.md` §10 (this ADR
resolves the working position it pinned).
**Tags:** `[ceir]` `[chir]` `[ckir]` `[ir]` `[architecture]` `[module-edges]` `[substrate]` `[north-star]`

---

## 1. Context

ADR-0108 authorized a Cerid-owned executable-program stack. Before CEIR-1 writes the first type, three things must be
pinned so nothing is retrofitted later: **which layer owns what**, **which way lowering flows**, and **which module
CEIR lives in with which dependency edges** (a wrong edge here is a cycle or a bloated core that the whole detour
inherits). The mission is explicit that the *layer separation may not be revisited by ADR* (§3) — only the names and
extensions may. This ADR fixes the separation and the names.

## 2. Decision — the three layers, their currencies, and what each must NEVER contain

| Layer | Owns (answers) | Currency | Must NEVER contain |
|---|---|---|---|
| **CHIR** (future language layer; design-only until CEIR-29) | source-language semantics: modules, generics, ADTs, closures, traits, ownership, async/state/events, reflection (§4) | a structured source model (semantic nodes + stable ids + source spans + layout metadata, §10) | execution/scheduling/resource-planning decisions (those are CEIR's); it *lowers/erases* into CEIR |
| **CEIR** (this detour) | **what work exists, how data/resources flow, what controls execution, where work runs, how work makes more work, how resources synchronize, which lower program implements each op** (§2) | a typed-SSA + graph/CFG/structured-region IR (`Module`/`Operation`/…) — text + binary + builder, all canonical | ⛔ a shading language or a bytecode (that is CKIR/backend — the ADR-0103 I1/I2 invariants extend UP to CEIR: `crd-ceir` never names GLSL/HLSL/WGSL/MSL/CUDA/SPIR-V/DXIL/PTX); ⛔ CHIR source syntax; ⛔ backend command-descriptor fields (§158) |
| **CKIR** (shipped, ADR-0101/0103; UNCHANGED) | **what one GPU invocation computes** — per-thread shader/kernel math | `KGraph`+`KEntry` → opaque `IGpuProgram` | orchestration/scheduling (that is CEIR's) |

**CKIR is not modified by this detour.** CEIR references a CKIR program by **content-hash IDENTITY** (a plain-value
`KernelRef` = asset id + interface hash, §85 §107), never by holding a `KGraph`. The CKIR→`IGpuProgram` compile
happens inside the GPU provider, not inside `crd-ceir` (see §4). The CEIR compiler may *generate* CKIR during
lowering (CEIR-23 fusion) — but that generation, too, is a provider/bridge concern, not core-`crd-ceir`.

## 3. Lowering is strictly one-way (§3 §144)

```
authoring (text · visual · domain frontends · C++ builder · importers)
        ↓  emit the SAME canonical CEIR (no privileged path — ADR-0108, §121)
CHIR   ──lower/erase──▶ CEIR
CEIR   ──lower──▶  CKIR (by identity / generated)   ∥   execution providers (host/GPU/NPU/…)
                                    ↓
                        compiled execution plan ──▶ backends
```

- **Never sideways, never up.** CEIR does not call CHIR; CKIR does not call CEIR; a provider does not reach back
  into the CEIR IR (it receives a lowered region + a provider-compile request, §69).
- **Provenance flows the opposite direction** (§111): every lowering records source spans so a backend error maps
  backend-op → plan-op → CEIR-op → CHIR/text-span. Provenance is metadata, not a control edge.

## 4. Module placement — `crd-ceir`, host-only, and provider dependency-inversion (the acyclicity argument)

### 4.1 The core module
**`engine/ceir` (`crd-ceir`)** — a **host-only foundational module**. Dependencies, and ONLY these:

```
crd-ceir → crd-core · crd-log · crd-memory · crd-containers · crd-units
```

All five are leaves/near-leaves (verified: `crd-units` deps = core + containers only), and none depends on
`crd-ceir`, so the edge set is **acyclic by construction**. `crd-ceir` holds: the IR (§5 types), the dialect
registry + verifier + printer/parser + `ModuleBuilder`, the semantic-identity model (§6), the **abstract** provider
interface, and the **abstract** kernel-reference type. It links **no GPU, no jobs, no kir, no render-graph.**

### 4.2 Why not link gpu-context / render-graph / kir / jobs directly
Because (b, the load-bearing reason, verified) it would drag the entire GPU/render stack into a foundational module
every consumer links — the ADR-0096 link-isolation discipline forbids that; and (a) it would foreclose the
**accepted CEIR-12 band plan** in which `render-graph` lowers its frame passes *into* CEIR (`ceir.frame`, §126) —
a direct `crd-ceir → render-graph` edge would make that flow a cycle. So CEIR uses **dependency inversion**:

- The **provider interface** `crd::ceir::IExecutionProvider` (§69: advertise ops/types/domains/costs; compile a
  region; execute; sync) lives in `crd-ceir` as an **abstract** interface — host-only, no backend types.
- Provider **implementations** live in SEPARATE bridge modules that depend on BOTH `crd-ceir` and the backend:
  - `crd-ceir-host` (the jobs provider — §31, born at CEIR-6b) → `crd-ceir` + `crd-jobs`.
  - `crd-ceir-gpu` (the render/compute provider) → `crd-ceir` + `crd-gpu-context` + `crd-render-graph` + `crd-kir`.
    **This is where the CEIR-0a `record_*` functions and CKIR compilation live** — they STAY in render-graph/
    gpu-context and are *invoked by* this bridge; they do not move into `crd-ceir`. (Resolves CEIR-0a §10.)
- The host registers provider implementations into the `crd-ceir` runtime at startup (the registration pattern,
  §150 — no central engine switch knows every provider).

```
crd-ceir (host-only core)  ◀── crd-ceir-host ──▶ crd-jobs
        ▲                  ◀── crd-ceir-gpu  ──▶ crd-gpu-context · crd-render-graph · crd-kir
        └── consumers (scene-render, hesap, …) link crd-ceir + the bridges they need
```

Result: `crd-ceir` is small, host-only, acyclic, and testable with a reference executor and NO device (§118) —
exactly the property that lets CEIR-1..8 land before any GPU contact (CEIR-10).

### 4.3 The invariants (grep-gated, extending ADR-0103 I1/I2 up a layer)
- **I3 — no shading language / bytecode name crosses into `crd-ceir` or any non-bridge consumer.** GLSL/HLSL/WGSL/
  MSL/CUDA/SPIR-V/DXIL/PTX appear only inside a backend TU (already I1/I2); `crd-ceir` additionally never names them
  — it holds `KernelRef` identities, not kernels.
- **I4 — no backend/provider type appears in a `crd-ceir` public header.** Consumers hold `crd::ceir::*` and the
  abstract `IExecutionProvider`, never a `VkDevice`/`IComputeContext`/`IGpuProgram`.
- **I5 — the module graph stays acyclic** (`crd-ceir` depends on none of its providers). The existing link-isolation
  gate (ADR-0096) is extended to assert the `crd-ceir` edge set.

## 5. Finalized C++ names for the CEIR-1 types (this is a CEIR-0c deliverable)

Namespace **`crd::ceir`**. Coding standard applies (CamelCase types, `lower_case` functions/members, `m_` members,
`kCamelCase` globals). These names are now **binding for CEIR-1** (the tracker's "working proposal" caveat is lifted
for them):

| Type | Role |
|---|---|
| `Context` | owns the dialect registry + interned `Type`/`Attribute`/string tables + arena allocators |
| `Module` | a top-level program unit (functions + symbols) |
| `Operation` | one op: interned `OpId` + `ValueId` operand/result spans + attribute map + region list (arena-allocated) |
| `Value` | a typed SSA value with a def-use chain; identified by `ValueId` |
| `Block` | a sequence of operations inside a region |
| `Region` | a nested body; `RegionKind::{Graph, SsaCfg}` (§13) |
| `SymbolTable` / `Symbol` | named functions/modules for cross-reference (`ceir.func`, §34) |
| `Type`, `Attribute` | interned type + attribute values (§16 / §8 property system). **The `crd-units` dependency is earned here:** a high-level `Type` may carry a **dimension tag** sourced from `crd::units` dimension ids (§17), checked by the verifier and **erased by an explicit op at the raw/kernel boundary** (the ADR-0078 two-layer rule). The full design lands at **CEIR-3e**; the dep is pre-declared now so `Type` reserves the tag slot from day one. That — not general use — is why an IR core links a units library. |
| `Dialect` | a registered `(name, op-set, verifier, printer/parser hooks)` (§7) — analyses dispatch via interfaces on it, never `switch(op.kind)` |
| `OpId` | interned `(dialect, name)` identity — a hashed id resolved once (the `ExecutorTypeId` precedent) |
| `SourceLoc` | provenance span carried on every `Operation` (§111) |
| `KernelRef` | a plain-value CKIR reference: `{ asset_id, interface_hash }` (§85) — NOT a `KGraph` |
| `IExecutionProvider` | the abstract provider seam (§69) — implemented in bridge modules only. ⛔ **Append new pure-virtuals at the END** (the D135 vtable-stability discipline; the rhi-compute LTCG-SEGV scar) — this is a public abstract interface in a foundational module dispatched across separate bridge targets, exactly where a mid-vtable insert silently mis-dispatches. |
| `ModuleBuilder` | the fluent C++ authoring API; emits ordinary canonical IR through the same verifier (§121) |

Textual form (MLIR-shaped, deterministic printer): `%v = dialect.op(%a, %b) {attr = value} : type`. Dialect names are
`lower_case` dotted (`ceir.core`, `ceir.frame`, `ceir.compute`).

## 6. The semantic-identity model (§10) — fixed before the editor, per the mission

The canonical program is **operations / values / regions / symbols** — NOT graph coordinates or edges (§166). To
make text ⇄ visual ⇄ builder one model with stable diff/merge:

1. **Stable semantic ids.** Every function/subgraph/op carries an identity independent of its textual position or
   graph coordinates, so a semantic diff is position-independent and node movement produces no semantic diff.
2. **Source spans on every op** (`SourceLoc`) — provenance from day one (retrofitting it is how source maps die).
3. **Layout metadata is SEPARATE from semantics.** Graph coordinates, comments, and grouping live in a side table
   keyed by semantic id — never in `Operation`. The deterministic printer emits semantics; the editor owns layout.

This is shared CHIR/CEIR infrastructure and is why CEIR-1c lands `SourceLoc` + the identity scheme with the core,
not later.

## 7. Consequences

**Positive:** a small, host-only, acyclic, device-free-testable core; the GPU/jobs stacks are providers behind an
abstract seam, so the core never bloats and never cycles; CKIR is untouched and referenced by identity; the CEIR-0a
`record_*`/CKIR-compile code stays where it is (no churn); the names + edges CEIR-1 needs are fixed. The `crd-units`
edge is the two-layer typed-quantity boundary (ADR-0078 §17), reserved on `Type` now, designed at CEIR-3e — see §5.

**Negative / cost:** a bridge module per provider class (`crd-ceir-host`, `crd-ceir-gpu`, later `-npu`, `-remote`)
is more CMake targets than a monolith; the dependency-inversion indirection (an abstract `IExecutionProvider`
between the IR and the device) has a small cost the compiled-plan tier (§153) must not pay per-op — the plan
pre-resolves provider ops to function pointers, so the indirection is compile-time, not hot-path.

**Risk:** the `KernelRef`-by-identity boundary must be airtight — if any `crd-ceir` header ever includes a `kir`
type, I3/I4 break and the core stops being host-only. Mitigation: the I3/I4/I5 grep gates land with CEIR-1.

## 8. References

- Mission §2 §3 §4 §10 §69 §85 §111 §144 §150 §158 §166.
- ADR-0108 — the owned language stack (this ADR's parent).
- ADR-0101 / ADR-0103 — IR-is-source-of-truth + gpu-context-owns-every-program; the I1/I2 invariants I3/I4 extend.
- ADR-0096 — module link-isolation gate (extended to the `crd-ceir` edge set as I5).
- `docs/design/ceir-0a-execution-path-inventory.md` §10 — the provider working-position this ADR formalizes.
- ADR-0078 — units (the `crd-units` dependency that keeps quantity types at the CEIR boundary, §17).
