#pragma once

// crd-ceir-gpu — CEIR-30a-2 (sec-103) the SHARDING PROPAGATION analysis for ceir.dist. Given a module carrying dist.shard
// placement seeds + tensor ops, `propagate_sharding` computes an INSPECTABLE `Value -> Sharding` map by ONE forward pre-order
// pass: a dist.shard SEEDS a Sharded value; tensor.elementwise takes the MEET of its operands; tensor.reduce over the SPLIT
// axis produces a PARTIAL (a per-device partial that a dist.all_reduce completes to Replicated). This is the sec-103 "the
// compiler PROPAGATES sharding annotations" half; the MATERIALIZATION (inserting the all_reduce/peer_copy where a Partial meets
// a consumer, and the 1-device identity) is CEIR-30a-3, which READS this map. The partition_ml precedent: a device-free compiler
// analysis over crd-ceir dialects lives in ceir-gpu (crd-ceir owns the dist dialect + find_dist_misuse). ⛔ I6 — the walk
// switches on op NAME, never op.kind. CEIR-30b-3a adds `lower_sharded_reduction` (below materialize_sharding): LOWER the analysed
// sec-140 reduction into an explicit per-rank tensor plan + a placement producer (rank_lineage), the device-free half of 30b-3.
// ⛔ TOTAL — the analysis ALWAYS produces a map; an unresolvable state is `Conflict` (a
// lattice value the gate asserts + 30a-3 refuses to materialize past), NEVER a silent Replicated (the sec-70 pre-lowering lie)
// and never an early-out that loses the map. ⛔ the map is keyed on arena Value* pointers and is valid ONLY while the module is
// UNMUTATED — 30a-3 mutates (inserts ops), so it must RE-RUN this analysis after inserting, never patch the returned map.

#include <crd/ceir/context.hpp>
#include <crd/ceir/id.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::gpu
{
// The lattice value for one tensor VALUE. `Replicated` is the default (every device has the full tensor) — a Value ABSENT from
// the map is Replicated (never stored). `Sharded` = split along tensor `axis` across the mesh's `mesh_axis`. `Partial` = each
// device holds a per-device partial (produced by a reduce/contraction OVER the sharded axis) that a dist.all_reduce with the
// SAME mesh_axis + fn completes to Replicated. `Conflict` = no single sharding is valid without a reshard/all_gather (both
// ledgered) — the analysis stays total, the gate asserts it, 30a-3 refuses to materialize past it.
enum class ShardingKind : crd::u8
{
    Replicated = 0,
    Sharded,
    Partial,
    Conflict,
};
[[nodiscard]] containers::StringView sharding_kind_name(ShardingKind k) noexcept;

// One value's sharding. Fields are interpreted per `kind`: `mesh` = the resolved dist.mesh op (Sharded/Partial; nullptr else —
// a POINTER, gathered once, so "same mesh?" is pointer equality). `axis` = the sharded tensor axis (Sharded) or, for Partial,
// the MOST-RECENT reduced tensor axis (provenance ONLY — a further reduce of a Partial overwrites it; 30a-3 reads mesh_axis+fn,
// not this; a Partial over mesh_axis 0 vs 1 of a 2-D mesh are DISTINCT states, hence mesh_axis is carried). `mesh_axis` = the
// mesh axis (Sharded/Partial). `fn` = the reduce fn {sum,prod,max,min} (Partial only; `mean` is unreachable — find_dist_misuse
// rejects it, so the propagation never sees it).
struct Sharding
{
    ShardingKind           kind      = ShardingKind::Replicated;
    const Operation*       mesh      = nullptr;
    crd::i32               axis      = -1;
    crd::i32               mesh_axis = -1;
    containers::StringView fn        = {};
};
// Field-wise identity (the gate asserts by identity; the meet + all_reduce match compare with it). ⛔ Replicated/Conflict compare
// on `kind` alone (their other fields are the defaults), so two Conflicts are equal and two Replicateds are equal.
[[nodiscard]] bool operator==(const Sharding& a, const Sharding& b) noexcept;
[[nodiscard]] inline bool operator!=(const Sharding& a, const Sharding& b) noexcept { return !(a == b); }

// The inspectable result: a Value -> Sharding map (a Value ABSENT = Replicated). `of(v)` is the accessor honoring that default.
struct ShardingMap
{
    containers::HashMap<const Value*, Sharding> map;
    explicit ShardingMap(memory::IAllocator* a) : map(a) {}
    [[nodiscard]] Sharding of(const Value* v) const noexcept
    {
        const Sharding* const s = map.find(v);
        return s != nullptr ? *s : Sharding{};
    }
};

// The MEET of two operand shardings (tensor.elementwise): Replicated is BOTTOM (absorbed — the other wins, a replicated operand
// is on every device); two EQUAL non-Replicated → that value; anything else (different mesh/axis, Sharded vs Partial, or either
// Conflict) → Conflict. Commutative.
[[nodiscard]] Sharding meet_sharding(const Sharding& a, const Sharding& b) noexcept;

// Propagate sharding over module `m` in ONE forward pre-order pass. Meshes are gathered ONCE up front (a dist.shard/all_reduce
// `mesh` symbol resolves against that small table, NOT a per-op module walk). ⛔ const Context& — reads names/attrs/types,
// interns nothing. The returned map is valid only while `m` is unmutated (see the header note). ⛔ 1-DEVICE IDENTITY: a
// dist.shard over a mesh axis of EXTENT 1 is degenerate — the shard IS Replicated (the fact lives HERE, in the analysis, so
// every consumer sees it; the 30a-3 materialization then inserts NO collective for a 1-device mesh — the identity falls out).
// ── mesh-query helpers (hoisted from the anon namespace at CEIR-30c so the placement loader can reuse them, not copy them) ──
// gather every dist.mesh op in `r` (region-recursive) into `out`. resolve_mesh returns the dist.mesh whose `name` == `name` (or
// null). mesh_extent returns the `shape` extent of `mesh_op` along `mesh_axis` (0 if malformed / out of range; 1 = a degenerate
// single-device axis — the 1-device identity). ⛔ const Context& — reads names/attrs, interns nothing.
void gather_meshes(const Context& ctx, const Region* r, containers::Array<const Operation*>& out);
[[nodiscard]] const Operation* resolve_mesh(const Context& ctx, const containers::Array<const Operation*>& meshes,
                                            containers::StringView name) noexcept;
[[nodiscard]] crd::i32 mesh_extent(const Context& ctx, const Operation* mesh_op, crd::i32 mesh_axis) noexcept;

[[nodiscard]] ShardingMap propagate_sharding(const Context& ctx, const Module& m, memory::IAllocator* alloc);

// The result of the 30a-3 MATERIALIZATION pass: how many dist.all_reduce ops were inserted, and whether a Conflict was seen
// (the pass REFUSES to materialize past a Conflict — it cannot know the right collective — and reports it).
struct MaterializeResult
{
    crd::u32 inserted     = 0;
    bool     had_conflict = false;
};
// CEIR-30a-3 (sec-68/sec-103) — MATERIALIZE the collectives sharding annotations imply: insert a dist.all_reduce wherever a
// Partial ESCAPES (reaches a consumer that is NOT itself a dist.all_reduce / tensor.reduce / tensor.elementwise — those
// COMPOSE the partial), completing it to Replicated ("sharding annotations lower to communication AUTOMATICALLY"). ⛔ ITERATIVE:
// each insert MUTATES the module (invalidating a prior ShardingMap's pointer keys), so the pass RE-RUNS propagate_sharding
// after every insert (never patches the map); it terminates because each insert makes one Partial Replicated. Inserts at the
// escaping Partial's DEF (dominating every use), RAUW-all then restores the all_reduce's own operand (the cycle-free trick).
// ⛔ a Partial whose escaping use is in an UNRELATED block is Conflict-safe today only because region ops are unknown-op ⇒
// Conflict (the 30a-2 region-carried-sharding ledger item, extended). ⛔ assumes a misuse-clean module (run the walks first).
// COST: O(#inserts × module) — re-propagates + re-scans from the top per insert (fine for real programs: one insert per escaping
// Partial); a hang-guard caps it. `had_conflict` is complete only on the TERMINATING scan (find escape early-returns on the
// first Partial) — this loop guarantees that; a caller that breaks the loop early would see a partial flag.
[[nodiscard]] MaterializeResult materialize_sharding(Context& ctx, Module& m, memory::IAllocator* alloc);

// The result of the 30b-3a LOWERING pass: `ranks` = the mesh extent the reduction was split across (the size-E mesh axis; 1 for
// the degenerate single-device identity), `had_conflict` = a Conflict (or a chain that is not the clean sec-140 pattern) blocked it.
struct LowerResult
{
    crd::u32 ranks        = 0;
    bool     had_conflict = false;
};
// CEIR-30b-3a (sec-103/sec-140) — LOWER a sec-140 sharded reduction into an EXPLICIT per-rank plan + a PLACEMENT producer.
// Precondition: materialize_sharding has RUN (the dist.all_reduce completing the Partial exists — authored or inserted). Walks
// propagate_sharding's map for the sec-140 pattern `declare/import -> dist.shard(mesh,axis) -> tensor.reduce(axis,fn) ->
// dist.all_reduce(fn)`: for a mesh of extent E>=2 it REPLACES the chain with E per-rank shard declares of shape [N/E,...] + E
// tensor.reduce ops (each TAGGED provider=rank in `rank_lineage`, the placement producer plan_tensor_pipeline_partitioned reads)
// + an (E-1)-op tensor.elementwise combine tree (fn -> {sum:add,prod:mul,max:max,min:min}), RAUW-ing the all_reduce's result to
// the combine and erasing the old chain (consumer-first — the materialize model reversed). The 1-device IDENTITY falls out: a mesh
// of extent 1 -> the shard is Replicated -> every degenerate dist op is stripped, leaving the unsharded reduce (ranks==1,
// rank_lineage empty — a 1-device mesh has no rank to place). ⛔ REFUSES (had_conflict, module BYTE-IDENTICAL — the conflict scan
// runs BEFORE any mutation): any Conflict in the module, or a chain whose shape/sharding is not the clean sec-140 pattern (a
// partial rewrite would corrupt the module). ⛔ the emitted module carries NO dist ops — plan_tensor_pipeline_partitioned consumes
// only tensor/resource ops. ⛔ `rank_lineage` is APPENDED (the caller owns it, keyed on the per-rank reduce Operation*).
[[nodiscard]] LowerResult lower_sharded_reduction(Context& ctx, Module& m, memory::IAllocator* alloc,
                                                  containers::HashMap<const Operation*, crd::i32>& rank_lineage);
} // namespace crd::ceir::gpu
