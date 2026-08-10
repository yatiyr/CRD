# ADR-0125 — The `crd-ceir-gpu` lowering bridge (CEIR-13d): CEIR compute/transfer regions → an inspectable command list

**Status:** **ACCEPTED** (2026-08-10, under the standing autonomous loop grant [[project_ceir_autonomous_loop_grant]];
design advisor-reviewed at the decomposition fork) — the D-007 **CEIR band 13 (Compute + transfer)**, slice **CEIR-13d**.
Births the `crd-ceir-gpu` bridge module (ADR-0109 §4.2) and its lowering pass.
**Phase:** D-007. Law: §158 (CEIR orchestrates the proven layer — it does not invent GPU plumbing) · §79 (scheduling /
barriers) · §26/§4d (the effect-derived hazards the barriers come from) · §162 (the plan is inspectable). Inbound:
CEIR-13a (`ceir.compute`), 13b (`ceir.transfer`), 13c (the KernelRef contract). Outbound: CEIR-13z (execution wires the
list through a live `IComputeContext` / the `IExecutionProvider` seam).
**Tags:** `[ceir]` `[ceir-gpu]` `[bridge]` `[compute]` `[lowering]` `[barriers]`

---

## 1. Context

CEIR-13a/b/c built the host-authorable `ceir.compute` + `ceir.transfer` op vocabulary in `crd-ceir` core; the ops
reference kernels by identity and declare 4d effects, but nothing yet turns a CEIR region into GPU commands. 13d is that
lowering. ⛔ ADR-0109 §4.2 forbids `crd-ceir` core from linking gpu-context (I4/I5) — so the lowering lives in a NEW bridge
module, `crd-ceir-gpu`, that depends on BOTH `crd-ceir` and the backend. `engine/ceir-gpu` did not exist before this slice.

## 2. Decision

### 2.1 The module births — `crd-ceir-gpu` (ADR-0109 §4.2), edges added AS CONSUMED

A new static library `engine/ceir-gpu` (`crd-ceir-gpu`), the `crd-ceir-host` template. ADR-0109 §4.2 lists its eventual
deps as `crd-ceir + crd-gpu-context + crd-render-graph + crd-kir`. ⛔ **Amendment-by-narrowing (per the I5 acyclic gate):**
the lowering pass needs ONLY `crd-ceir` + `crd-gpu-context` (the `command_model.hpp` value types) — the `crd-render-graph`
and `crd-kir` edges are added WHEN a consumer arrives (the render dialect / kernel-program resolution, 14 / 13z). A link
edge with zero includes is the acyclic gate's dead-edge smell (and the 12b open-tag rationale — a home when consumed, not
before). This is a narrowing of §4.2's full list, not a violation.

### 2.2 The lowering is a PURE VALUE-LIST function — compile ≠ run (§158)

`lower_region(ctx, block) → Array<LoweredCommand>` — a CEIR-side, value-typed, **inspectable** (§162) command list;
execution against a live device is a SEPARATE step (13z). ⛔ The lowering NEVER drives an `IComputeContext` / touches a
`VkDevice` — that would be untestable without a GPU, un-inspectable, and would fuse the compile and run phases the 11b/12d
precedents keep apart. The list is asserted on directly in tests (ordering, descs, barrier placement) — no GPU, no mock
encoder needed. This is the plan-producing-tier-with-downstream-consumer shape — the reason 13d takes an ADR where 13a–c
(op-vocabulary/analysis slices) did not.

### 2.3 Handles are UNRESOLVED in the list — they bind at execute (the 13z / CKIR-by-identity seam)

A device pipeline + device buffers do not exist at lowering time. So a `LoweredCommand` carries the CEIR-NATIVE identities —
the **unresolved KernelRef** (the 13c `kernel` symbol + interface pin), the grid (resolved from `arith.const`, or marked
dynamic → resolved at execute), and the bindings by their CEIR SSA `Value*` + access. The executor RESOLVES these at execute
(13z) — CKIR-by-identity landing exactly where 13c left it.

⛔ **SUPERSEDED in place (ADR-0126): the execute surface is the ADR-0100 `IComputeContext`, NOT `command_model.hpp`
`DispatchDesc`.** This clause originally said a `LoweredCommand` binds to a `DispatchDesc` and `validate_dispatch` runs on
the resolved desc. Primary-source evidence (13z-1 recon) corrected it: the CKIR proof kernels + oracles run entirely on the
ADR-0100 surface (`ComputePipeline`/`ComputeBuffer`/`ComputeRecorder`), and `DispatchDesc`/`ICommandEncoder` is the SEPARATE
RAF frame-graph command model (`render-graph::record_compute_dispatch`), NOT dead code. So `execute_lowered` (ADR-0126)
resolves to a `ComputePipeline` + `ComputeBuffer` bindings, and `validate_dispatch`'s semantics are re-homed into the typed
`ExecuteError` (`UnresolvedKernel`/`ZeroDispatch`/`BindingArity`/`UnmappedBinding`). See ADR-0126 §"Two GPU surfaces".

### 2.4 Barriers are CONSERVATIVE, hazard-derived (§79 refinement named-forward)

Ordering rides the CEIR-4d effect machinery: the lowering walks op pairs and emits a barrier between any pair whose
`ops_hazard(before, after)` is non-None. Conservative — NO transitive reduction (that is the scheduler's job, §79, a
named-forward; the 12c hazard doc already pins it). This makes the **upload→first-read barrier BY CONSTRUCTION** (the band's
headline scar): a write-then-read pair hazards RAW, so a barrier lands between them without special-casing.

### 2.5 Named-forwards this ADR pins (land in later 13d ticks)

- **13a's ambient `MemoryReadWrite` narrowing** — ✅ LANDED in **part 2**: a dispatch's conservative whole-Memory effect
  narrowed to per-binding read/write from the `access` string (bridge-side `gather`; the core op stays conservative).
- **The 12c view→root retrofit into `ops_hazard`** — ✅ LANDED in **part 3**: `op_access_at` (core) and `gather` (bridge)
  both normalize each captured resource through `Context::resource_root`, so a transfer/dispatch through a view now hazards
  its buffer. A view laundered through a region-yield/call-result still escapes (a deeper alias hole, named-forward).
- **Execution** (list → live `IComputeContext` via the `IExecutionProvider` seam) — 13z.
- **Dynamic (non-const) grid** + **render-graph/kir edges** — resolved/added as consumed (13z / 14).

## 3. Consequences

The band's compute pipeline gains its first GPU-facing artifact — an inspectable, testable-without-a-GPU command list, the
clean seam the 13z proof (add/reduce/scan/FFT as CEIR assets, Vulkan+DX12) executes through. 13d is delivered across
checkpoints (the 13c two-tick precedent): **(1a, this ADR)** module scaffold + the `lower_region` contract; **(1b)** the
dispatch lowering + hazard-derived barriers; **(2)** transfer lowering + the upload-barrier test + the ambient narrowing;
**(3)** the view→hazard retrofit + flip. Each checkpoint gated; the 13d row flipped at part 3 (all four checkpoints DONE).

`crd-ceir` core is UNTOUCHED (I4/I5 hold — the gpu-context dependency lives only in the new bridge). The
`crd-ceir-invariants` gate continues to prove the core stays host-only.
