#pragma once

// crd-ceir-gpu — the CEIR-13d GPU LOWERING bridge (ADR-0125; ADR-0109 §4.2). `lower_region` turns a ceir.compute /
// ceir.transfer REGION into a pure, value-typed, INSPECTABLE command list (§162) — dispatches + interleaved 4d-derived
// barriers. ⛔ Handles stay UNRESOLVED in the list (the CEIR KernelRef + resource Value*s); they bind at EXECUTE (CEIR-13z /
// CKIR-by-identity) — ⛔ SUPERSEDED (ADR-0126): the execute surface is the ADR-0100 IComputeContext (a resolver → a
// `ComputePipeline` + `ComputeBuffer` bindings), NOT a command_model `DispatchDesc` (that is the SEPARATE RAF frame-graph
// surface). The lowering NEVER drives an IComputeContext (compile ≠ run,
// §158) — tests assert on the returned list, no GPU needed. This is a BRIDGE header: it names gpu-context types; crd-ceir
// core never does (I4/I5).

#include <crd/ceir/context.hpp>
#include <crd/ceir/hazard.hpp>       // HazardKind — the barrier's ordering relationship
#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/command_model.hpp> // crd::gpu::DispatchKind — the lowered dispatch's Direct/Indirect selector

namespace crd::ceir::gpu
{
// What a lowered command IS. ⛔ append at END (a value enum in a bridge-public header). `Dispatch` covers direct + indirect
// (via `dispatch_kind`); `Transfer` covers the 13b movement ops (via `transfer_kind`); `Barrier` orders a hazarding pair.
enum class LoweredKind : crd::u8
{
    Dispatch = 0,
    Transfer,
    Barrier,
};

// The lowered TRANSFER op (CEIR-13b §50). ⛔ this is our OWN enum — the 5 SHIPPED 13b ops — NOT crd::gpu::TransferKind
// (Clear/Copy/Blit/Resolve), which would silently SUBSET upload/readback/mip_gen (the NO-FOLLOW shape). Blit/Resolve are
// §50 named-forwards (not shipped at 13b). ⛔ append at END.
enum class LoweredTransferKind : crd::u8
{
    Copy = 0,
    Upload,
    Readback,
    Clear,
    MipGen,
};

// One entry of the inspectable command list. The `op` back-pointer carries the source dispatch's UNRESOLVED identities (the
// 13c `kernel` symbol + the binding operands) — the executor (ADR-0126 `execute_lowered`) resolves them to a
// `ComputePipeline` + `ComputeBuffer` bindings on the ADR-0100 IComputeContext surface at execute (13z).
// The VALUE fields (grid, dispatch_kind) are resolved HERE where they are known (a compile-time `arith.const` grid);
// `dynamic_grid` marks a non-const grid deferred to execute. A `Barrier` carries the `HazardKind` + the ordered ops.
// ⛔ The exact field set is finalized WITH the lowering logic (CEIR-13d tick 1b) — this is the scaffolded contract.
struct LoweredCommand
{
    LoweredKind      kind = LoweredKind::Dispatch;
    const Operation* op   = nullptr; // provenance + execute-time kernel/binding resolution (the 13z seam)
    // ── Dispatch ──
    crd::gpu::DispatchKind dispatch_kind = crd::gpu::DispatchKind::Direct;
    // Direct: the workgroup grid — resolved ALL-OR-NOTHING from the three arith.const grid operands. ⛔ MEANINGLESS when
    // `dynamic_grid` (any operand non-const OR Indirect) — left at the defaults, NOT partially filled (a half-real grid is a
    // §162 trap). A resolved zero group is lowered AS-IS (ZeroDraw is the execute-time validate_dispatch's rejection, not the
    // lowering's — the lowering is a faithful TRANSLATOR; find_dispatch_misuse is the IR verifier and checks grid TYPES not values).
    crd::u32 groups_x = 1;
    crd::u32 groups_y = 1;
    crd::u32 groups_z = 1;
    // true ⇒ the grid is not lowering-time data: any Direct grid operand is non-const (resolved at execute). ⛔ NOT set for
    // Indirect (its grid comes from the args buffer — a different §162 answer, `dispatch_kind`, not a failed resolution).
    bool dynamic_grid = false;
    // ── Transfer ── the movement op; src/dst ride `op` (unresolved Value*s, bound at execute). `clear_value` is the RESOLVED
    // fill word for a Clear whose `value` attr is present (§162 "clear to 0x7"); `has_clear_value` gates it (meaningless else).
    LoweredTransferKind transfer_kind    = LoweredTransferKind::Copy;
    crd::i64            clear_value       = 0;
    bool               has_clear_value   = false;
    // ── Barrier ── ONE incoming HAZARD into the FOLLOWING command, on ONE root `resource`. ⛔ CEIR-13d part 2: derived from
    // PRECISE per-command accesses (a dispatch's bindings + `access` string; a transfer's static per-operand effects) — the
    // 13a ambient MemoryReadWrite is NARROWED here (in the bridge only; the core op's declared effects + `ops_hazard` stay
    // conservative). `before` = the NEAREST earlier op writing `resource` (reverse scan) — the last writer, deterministic +
    // the most useful §162 source. `after` = the dispatch it precedes. ⭐ CEIR-13z-3: PER-RESOURCE — a dispatch that reads N
    // buffers written by prior dispatches emits N Barriers, one per conflicting root resource, in `after`'s binding-operand
    // order (the part-2 "one barrier per dispatch, strongest" DROPPED conflicts — a 13d correctness completion, not just an
    // execution convenience). `resource` = the conflicting root Value (nullptr ⇒ ambient/whole-class — barrier ALL buffers).
    HazardKind       hazard   = HazardKind::None;
    const Operation* before   = nullptr;
    const Operation* after    = nullptr;
    const Value*     resource = nullptr; // ⭐ 13z-3: the conflicting root resource (nullptr = ambient/whole-class)
};

// Lower `block`'s compute (later: transfer) ops to an inspectable command list, with a barrier interleaved between any op
// pair whose `ops_hazard` is non-None (conservative — no transitive reduction; that is the §79 scheduler's job). ⛔ `out`
// is CLEARED then filled (the out-param house pattern of the hazard/lifetime/plan collectors). Pure; not noexcept.
// ⛔ CEIR-13d tick 1a: the CONTRACT + a stub (yields an empty list). The dispatch lowering + barriers are tick 1b.
void lower_region(const Context& ctx, const Block& block, containers::Array<LoweredCommand>& out);
} // namespace crd::ceir::gpu
