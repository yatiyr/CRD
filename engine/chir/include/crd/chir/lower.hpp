// lower.hpp — the CHIR→CEIR lowering (CEIR-32d, ADR-0128 D1/D5). One-way: crd-chir -> crd-ceir, never a back-edge.
//
// `lower_chir` walks a CHIR `SourceModel` (the ONE source model both projections produce — text via parse_chir, graph
// via read_schema) and emits a CEIR module per the ADR-0128 D1 node->op map:
//   EventHandler -> func.func + a time-domain attr (ADR-0116; NO new `event` op — zero-new-ops)
//   Query        -> the entity view = the func's block-args (component storages); v1 "existing registration bridge"
//   ParallelFor  -> task.parallel_for(%lo,%hi,%step){ ^(%iv): ... core.yield } — body STATE-FREE (verifier) + self-contained
//   Await        -> async.launch{ ... } -> %token ; async.await(%token)   (token consumed exactly once)
//   StateDecl    -> core.state(%init, %next) -> %current                   (ONE §20 StateEdge cell per declaration, CEIR-32e)
//   StateUpdate  -> folds into its cell's %next feedback (no separate op — the CEIR-32e unification of D1/D3)
//
// ⛔ THE D2 QUERY-VIEW READ PATH: §143's `parallel update` iterates the query result, an OUTER value (the storages are
// func block-args). The parallel body references that outer value directly — which is SSA-legal + structure-verifier
// clean; self-containment ("outer captures are UndefinedValue at execution", task.ceirop.toml:19) is an EXECUTION caveat,
// not a verify rejection (`check_parallel_region` is an exec pre-flight checking arity + yield + state-freedom, never
// operand origin). Seeding the capture at execution is the ADR-named CEIR-32e/runtime refinement; 32d lands the
// structurally-correct read that VERIFIES + round-trips + PARITY-gates.
//
// ⛔ DETERMINISTIC: `lower_chir` is a pure function of the SourceModel, so both projections (which produce the SAME model
// — proven in 32c by semantic_hash) lower to the BYTE-IDENTICAL CEIR module (the text/graph parity anchor, §180 #13).

#pragma once

#include <crd/chir/node.hpp>

namespace crd::ceir
{
class Context;
class Module;
} // namespace crd::ceir

namespace crd::chir
{
// ⛔ ADR-0128 D3 (CEIR-32e) — the reserved LOW band CHIR pins its state-cell CEIR stable ids into. Each program-scope
// `state` declaration's `core.state` cell gets `id = 1 + (chir_node_id % kChirStateIdReserve)` (source-derived from the
// decl's position-independent CHIR StableId, so a body/reorder/insert edit keeps it → hot reload MIGRATES the cell's value
// by id; a RENAME changes the CHIR id → a new cell id → a state-schema change). `lower_chir` floors the module watermark to
// this value so assign_stable_ids draws every OTHER (sequential, reload-invisible) id strictly ABOVE the band —
// watermark-safe (the monotone-id invariant). The band width makes a collision among realistic state-cell counts
// negligible; a collision is resolved deterministically (a position-independent probe), never a silent alias.
inline constexpr crd::u64 kChirStateIdReserve = crd::u64(1) << 24;

// Lower `m` one-way into a CEIR module owned by `ctx` (registers func/core/arith/task/async on `ctx`, idempotent).
// Returns the module, or nullptr if `m` has no Program root (a graceful empty lowering).
[[nodiscard]] crd::ceir::Module* lower_chir(const SourceModel& m, crd::ceir::Context& ctx);

} // namespace crd::chir
