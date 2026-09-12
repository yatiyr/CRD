#pragma once

// crd-ceir-gpu — the §69 HETEROGENEOUS-EXECUTION PARTITIONER for ceir.ml (CEIR-24c). Given a module of ml ops + a set of
// PROVIDERS (each advertising which ops it can CLAIM whole), `partition_ml` produces an INSPECTABLE assignment of each ml op to a
// provider OR to the CKIR fallback. This is the §69 "advertise / assign" core. CEIR-29a LANDED the full DEVICE-FREE partitioner
// here: provider-class descriptor fields (29a-1) + maximal-subgraph greedy-grow (29a-2) + authored pinning via
// transform.assign_provider → provider_from_transform (29a-3). The DEVICE-executing native-graph provider (CUDA Graphs) + the
// boundary-transfer plan stage are CEIR-29b; the two-class boundary proof + CONSTRAINED provider-choice mode are CEIR-29c. A provider either CLAIMS an op whole (a native fused kernel — e.g. the
// coopvec per-invocation MLP) or it doesn't; a NOT-claimed op falls back to the CEIR-22/23 CKIR expansion (expand_ml_op).
//
// ⛔ DEVICE AVAILABILITY IS AN INPUT, NOT A QUERY: each MlProvider carries an `available` flag the CALLER sets from device caps
//    (e.g. cooperative_vector()). The partitioner stays PURE + device-free — the "can't-claim → fallback" negative is testable
//    with available=false (everything falls back). This mirrors the plan/execute split (a caps input, not a device call).
// ⛔ the advertise predicate is a SPECIALIZED-KERNEL SELECTION (the fusion/QuantGemm-scheme scar): it must check FULL semantic
//    attrs (op NAME per I6, the exact activation vocab, element kind, rank, layer-shape support), never just structure — a gelu
//    ml.mlp claimed by a relu-only native kernel is a silent wrong-function.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gpu/expand_ml.hpp> // MlExpandResult (apply_partition expands the fallback ops)
#include <crd/ceir/id.hpp>
#include <crd/ceir/semantics.hpp> // ProviderClass (§69) + DeterminismClass (§27) — the descriptor's class fields

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>

#include <type_traits>

namespace crd::ceir::gpu
{
// A provider's ADVERTISE predicate: can this provider CLAIM `op` whole (a native fused kernel)? PURE (const Context&, const op) —
// FULL semantic attrs, no device call, no captures (a plain function pointer). Returns false for ops it cannot claim.
using MlAdvertiseFn = bool (*)(const Context&, const Operation*);

// A §69 execution provider descriptor: a name, an `available` flag (the caller sets it from device caps — an INPUT), and the
// advertise predicate. An unavailable provider claims nothing (skipped in the assign loop, even if it could semantically claim).
struct MlProvider
{
    containers::StringView name;
    bool                   available = false;
    MlAdvertiseFn          advertise = nullptr;
    // CEIR-29a-1 §69 — the provider-CLASS descriptor fields (APPENDED). ⛔ ~~INERT until 29a-2/3 read them~~ **[STRUCK: CEIR-29c-3a
    // READS `provider_class` — the `PartitionConstraint` class FILTER discriminates two classes claiming one op; `memory_domain` +
    // `determinism` stay descriptor-only — NO consumer reads them YET (a `DeterminismClass` constraint, e.g. "BitExact or fallback",
    // is the natural NEXT filter axis, name-forward to 29z/30-0)].** ⛔ Keep MlProvider TRIVIALLY COPYABLE (a POD passed by span/value):
    // every member is trivial (StringView / bool / fn-ptr / enum) — do NOT add a non-trivial member. Every REAL provider
    // sets all three explicitly (the coopvec helpers do); the defaults are the honest module-level state.
    ProviderClass          provider_class = ProviderClass::Gpu;          // this partitioner IS crd-ceir-gpu ⇒ a registered provider is a gpu-bridge one unless it says otherwise
    containers::StringView memory_domain  = {};                          // the §24 vocab (resource.declare's); "" ⇒ unspecified — validate with crd::ceir::is_memory_domain
    DeterminismClass       determinism    = DeterminismClass::Unspecified; // §27 (semantics.hpp); a provider claims its OWN honest class (coopvec = DeterministicWithinTarget: a fixed-order fused kernel, reproducible on a given target)
    // CEIR-29a-2 §70/§102 — the SUBGRAPH-claim capability (the census-seed's advertise_region(Region*) → a bool: a Region is
    // the wrong granularity — the mixed 24z region holds one claimable + one not). false = PER-OP claiming (coopvec: each op
    // is its own native kernel, subgraph=-1). true = the partitioner GREEDY-GROWS a maximal run of consecutive claimable
    // SIBLING ml ops under ONE shared subgraph id (a native-graph provider running the run as one program). The claim-test is
    // the SAME per-op `advertise`; this flag only says whether claimed ops FUSE into a run.
    bool                   claims_subgraphs = false;
};
// CEIR-29a-1: MlProvider is a POD descriptor passed by span/value — pin trivial-copyability so a future non-trivial
// field can't silently break the ConstSpan<MlProvider> passing (the house `ConcurrentQueue<T>` static_assert pattern).
static_assert(std::is_trivially_copyable_v<MlProvider>,
              "MlProvider must stay trivially copyable (a POD provider descriptor passed by span/value)");

// One op's assignment: `provider` = the index into the providers span that CLAIMS it, or -1 for the CKIR FALLBACK (no provider
// claimed it). `op` points at the ml op in the module.
struct MlAssignment
{
    const Operation* op       = nullptr;
    crd::i32         provider = -1; // -1 == CkirFallback
    // CEIR-29a-2 §102 — the SUBGRAPH group this op was claimed into: ops sharing (provider, subgraph) are ONE native unit
    // (a run the provider runs as one program). ⛔ MONOTONIC across the WHOLE module (never reset per-provider/per-block —
    // the monotone-watermark discipline: two runs of the same provider in different blocks MUST get different ids, else
    // 29c merges them across a boundary). -1 = a per-op singleton (a fallback op, or a claims_subgraphs=false claim).
    crd::i32         subgraph = -1;
};

// The inspectable partition: one MlAssignment per ml op in the module (pre-order). `claimed(i)` counts the ops a given provider
// claimed; `fallback()` counts the CKIR-expansion ops.
struct MlPartition
{
    containers::Array<MlAssignment> assignments;
    explicit MlPartition(memory::IAllocator* a) : assignments(a) {}
    [[nodiscard]] crd::u32 fallback() const noexcept
    {
        crd::u32 n = 0;
        for (crd::usize i = 0; i < assignments.size(); ++i) { n += assignments[i].provider < 0 ? 1U : 0U; }
        return n;
    }
    [[nodiscard]] crd::u32 claimed_by(crd::i32 provider) const noexcept
    {
        crd::u32 n = 0;
        for (crd::usize i = 0; i < assignments.size(); ++i) { n += assignments[i].provider == provider ? 1U : 0U; }
        return n;
    }
    // CEIR-29a-2: how many DISTINCT subgraphs (maximal runs, id >= 0) `provider` claimed. A per-op claimer (claims_subgraphs
    // = false) has 0 — its ops are -1 singletons; a subgraph claimer has one id per maximal run. O(n^2) but n is an op count.
    [[nodiscard]] crd::u32 subgraphs_of(crd::i32 provider) const noexcept
    {
        crd::u32 n = 0;
        for (crd::usize i = 0; i < assignments.size(); ++i)
        {
            if (assignments[i].provider != provider || assignments[i].subgraph < 0) { continue; }
            bool first = true;
            for (crd::usize j = 0; j < i; ++j)
            {
                if (assignments[j].subgraph == assignments[i].subgraph) { first = false; break; }
            }
            n += first ? 1U : 0U;
        }
        return n;
    }
    // CEIR-29a-2: the op-count of the maximal run `id` (a claimed subgraph). 0 for id < 0 (a singleton is not a subgraph).
    [[nodiscard]] crd::u32 subgraph_size(crd::i32 id) const noexcept
    {
        if (id < 0) { return 0U; }
        crd::u32 n = 0;
        for (crd::usize i = 0; i < assignments.size(); ++i) { n += assignments[i].subgraph == id ? 1U : 0U; }
        return n;
    }
};

// CEIR-29c-3a §102 — the CONSTRAINED provider-choice descriptor threaded into partition_ml. TWO composed knobs, filter-then-prefer:
//   `has_class`+`provider_class` — a HARD CLASS FILTER (a §102 portability boundary, "run on a provider of THIS class or not at
//      all"): when has_class, ONLY providers whose `provider_class` matches are eligible to claim; an op no eligible provider
//      claims falls back to -1, NEVER to a provider of another class (the graceful-wrong-answer trap — a class constraint is a
//      requirement, not a 29a-3 preference). This is DIFFERENT from `pinned`: the pin falls through, the class does not.
//   `pinned` — the 29a-3 authored provider PREFERENCE, but WITHIN the class-filtered set (filter is OUTER): a pin to a provider
//      the class filter excluded is IGNORED (never overrides the filter); an in-class pin is tried first among the eligible.
// -1 pinned + has_class=false is the 24c invariant (no pin, no class → first-available).
struct PartitionConstraint
{
    bool          has_class      = false;
    ProviderClass provider_class = ProviderClass::Gpu; // meaningful only when has_class
    crd::i32      pinned         = -1;
};

// Assign every ml.mlp / ml.attention op in `m` (pre-order) to the FIRST ELIGIBLE provider whose advertise predicate accepts it
// (eligible = available + advertise + — when `constraint.has_class` — `provider_class` matches), honoring `constraint.pinned` as
// the in-class preference, or to the CKIR fallback (-1). ⛔ const Module — a pure inspection (no rewrite). ⛔ const Context — reads
// types + attrs. ~~The AUTHORED source of the pin (transform.assign_provider {provider}) lands in 29a-3b; the authored CLASS
// constraint (transform.constrain_provider_class {class}) lands in 29c-3b.~~ **[STRUCK: both LANDED — provider_from_transform
// (29a-3b) resolves the pin NAME to a span index; constraint_from_transform (29c-3b) resolves the class NAME to a ProviderClass;
// compose_partition_constraint folds them into this PartitionConstraint (filter OUTER, pin inner).]**
[[nodiscard]] MlPartition partition_ml(const Context& ctx, const Module& m, containers::ConstSpan<MlProvider> providers,
                                       memory::IAllocator* alloc, PartitionConstraint constraint = {});

// CEIR-29a-3b §71/§102 — the AUTHORED-PIN loader: resolve a `transform.assign_provider` directive's `provider` NAME to the
// INDEX into `providers` that partition_ml takes as `pinned`. This is plan_options_from_transform's sibling (a transform-module
// walk) but it lives HERE, not in tensor_pipeline, so the tensor planner stays PARTITION-BLIND (no MlProvider dependency).
enum class ProviderPinKind : u8
{
    None = 0,              // no transform.assign_provider directive — no pin (partition_ml runs first-available)
    Resolved,             // the name matched exactly ONE provider — `index` is its span index
    UnknownProvider,      // the name (or an empty/invalid `provider` attr) matched NO provider — a typed reject, never a silent -1
    DuplicateProviderName // the span ITSELF has two providers sharing a name — ambiguous by construction (a span defect, found first)
};
[[nodiscard]] containers::StringView provider_pin_kind_name(ProviderPinKind k) noexcept;

// The pin-resolution result. On Resolved, `index` is the span index to pass as partition_ml's `pinned`; on a reject `index` is
// -1 and `op` points at the offending assign_provider directive (null for a DuplicateProviderName span defect, found before the
// walk). ⛔ the CALLER must SURFACE a reject — passing it through as a silent -1 degrades a typo'd pin into "no pin" (the
// graceful-wrong-answer trap: a misspelled provider would run first-available instead of erroring).
struct ProviderPin
{
    crd::i32         index = -1;
    const Operation* op    = nullptr;
    ProviderPinKind  kind  = ProviderPinKind::None;
};
// Resolve the (at-most-one — find_transform_misuse guards DuplicateDirective) transform.assign_provider directive in
// `transform_mod` against `providers` by NAME equality. FIRST a pairwise duplicate-name scan over the WHOLE span (a shared name
// ⇒ DuplicateProviderName BEFORE resolving — the span is broken whichever name the pin targets); THEN the directive: absent ⇒
// None; an empty/invalid/non-string `provider` or a name matching zero providers ⇒ UnknownProvider; exactly one match ⇒ Resolved.
// ⛔ the directive is found REGION-RECURSIVELY (matching find_transform_misuse's walk — the at-most-one guard this relies on is
// recursive, so a NESTED directive must not be silently missed). ⛔ matches op NAME (ctx.op_name, const) never op.kind (I6).
[[nodiscard]] ProviderPin provider_from_transform(const Context& ctx, const Module& transform_mod,
                                                  containers::ConstSpan<MlProvider> providers);

// CEIR-29c-3b §102 — the AUTHORED-CLASS-CONSTRAINT loader: resolve a `transform.constrain_provider_class` directive's `class`
// NAME to a ProviderClass (via semantics.hpp provider_class_from_name). The ProviderPin sibling for the class-filter half of
// PartitionConstraint; it lives HERE (not tensor_pipeline) so the tensor planner stays partition-blind. ⛔ NO `has_class` field
// on the result — `kind == Resolved` IS has_class (a second boolean that must agree with the enum is the drift trap).
enum class ClassConstraintKind : u8
{
    None = 0,             // no transform.constrain_provider_class directive — no class filter (partition_ml runs unconstrained)
    Resolved,             // the `class` name matched a ProviderClass — `provider_class` is it
    UnknownProviderClass, // the name (or an empty/invalid `class` attr) matched NO ProviderClass — a typed reject, never a silent no-op
};
[[nodiscard]] containers::StringView class_constraint_kind_name(ClassConstraintKind k) noexcept;

// The class-resolution result. On Resolved, `provider_class` is the filter to pass in a PartitionConstraint; on a reject `op`
// points at the offending directive (null when absent). ⛔ the CALLER must SURFACE a reject — a silent no-class degrades a typo'd
// class into "unconstrained" (the graceful-wrong-answer trap the assign_provider loader also guards).
struct ClassConstraint
{
    ProviderClass       provider_class = ProviderClass::Gpu; // meaningful only when kind == Resolved
    const Operation*    op             = nullptr;
    ClassConstraintKind kind           = ClassConstraintKind::None;
};
// Resolve the (at-most-one — find_transform_misuse guards DuplicateDirective) transform.constrain_provider_class directive in
// `transform_mod`: absent ⇒ None; an empty/invalid/non-string `class`, or a name matching no ProviderClass ⇒ UnknownProviderClass;
// a match ⇒ Resolved. ⛔ found REGION-RECURSIVELY (matching find_transform_misuse's recursive walk); matches op NAME (I6). NO
// `providers` span needed — the class is parsed from the string, not matched against providers (unlike provider_from_transform).
[[nodiscard]] ClassConstraint constraint_from_transform(const Context& ctx, const Module& transform_mod);

// COMPOSE the two authored halves into ONE PartitionConstraint (filter OUTER, pin inner): the class from `cc` (has_class iff it
// Resolved), the pin index from `pin`. The ONE place a class-constraint + a pin become a PartitionConstraint — a hand-composed
// struct at each call site is how `has_class` and the loader `kind` would disagree. ⛔ pass the loaders' results straight in; a
// reject (`cc.kind`/`pin.kind` != Resolved/None) is the CALLER's to surface FIRST — compose does not itself reject.
[[nodiscard]] PartitionConstraint compose_partition_constraint(const ClassConstraint& cc, const ProviderPin& pin) noexcept;

// CEIR-30c §103/§146 — the AUTHORED-PLACEMENT loader. The provider_from_transform / constraint_from_transform SIBLING for the
// SHARDING-placement half: those place the PARTITIONER's CLAIM (which provider runs an ml op); this places the MESH's RANKS (which
// device class runs each shard's stage). Resolves a `transform.place_mesh` directive → the per-rank `rank_classes` + the unowned-
// stage `fallback` that feed stage_class_from_placement (tensor_pipeline_exec) → the per-stage placement execute_two_class runs.
enum class PlacementKind : u8
{
    None = 0,             // no transform.place_mesh directive — no authored placement
    Resolved,             // the directive resolved — rank_classes + fallback are valid
    UnknownMesh,          // `mesh` resolves to NO dist.mesh in the payload (or is non-Symbol/absent)
    MultiAxisMesh,        // the mesh has >1 axis (shape has a comma) — this slice places a 1-D mesh only; a `mesh_axis` attr is name-forward
    UnknownProviderClass, // a `classes` segment OR `fallback` matched NO ProviderClass (empty/unknown) — covers BOTH attrs
    RankCountMismatch,    // the `classes` count != the (axis-0) mesh extent (a rank placed nowhere, or a phantom rank)
};
[[nodiscard]] containers::StringView placement_kind_name(PlacementKind k) noexcept;

// The placement result. On Resolved, `rank_classes[r]` is mesh rank r's device class + `fallback` is the unowned-stage (all-reduce
// COMBINE) class; on a reject `op` points at the offending directive (null when absent). ⛔ the CALLER must SURFACE a reject — a
// silent no-placement degrades a typo'd class / short list into "everything on the fallback" (the graceful-wrong-answer trap the
// assign_provider / constrain_provider_class loaders also guard). Holds an arena Array ⇒ constructed with an allocator.
struct Placement
{
    containers::Array<ProviderClass> rank_classes;
    ProviderClass                    fallback = ProviderClass::Gpu; // meaningful only when kind == Resolved
    const Operation*                 op       = nullptr;
    PlacementKind                    kind     = PlacementKind::None;
    explicit Placement(memory::IAllocator* a) : rank_classes(a) {}
};
// Resolve the (at-most-one — find_transform_misuse guards DuplicateDirective) transform.place_mesh directive in `transform_mod`
// against `payload` (its dist.mesh ops, for the rank-count validation): absent ⇒ None; an unknown/non-Symbol `mesh` ⇒ UnknownMesh;
// a bad class in `classes`/`fallback` ⇒ UnknownProviderClass; a classes-count != the mesh extent ⇒ RankCountMismatch; else Resolved.
// ⛔ found REGION-RECURSIVELY (I6 op name); reuses the sharding.hpp mesh-query helpers (gather_meshes/resolve_mesh/mesh_extent).
[[nodiscard]] Placement placement_from_transform(const Context& ctx, const Module& transform_mod, const Module& payload,
                                                 memory::IAllocator* alloc);

// Expand ONLY the CKIR-FALLBACK-assigned ops (provider < 0) into the 22/23 vocab (expand_ml_op each); the CLAIMED ops are left
// in place for the caller to dispatch natively (the §136 crown's coopvec path). ⛔⛔ CEIR-29b-2b RECONCILE — TWO KINDS of claim,
// and apply_partition's leave-in-place serves only ONE: a SEMANTIC-ENGINE provider (coopvec) REPLACES the claimed op with a
// native fused kernel, so it is left un-expanded here. A LAUNCH-GRAPH provider (cuda_graphs) does the OPPOSITE — it captures
// the EXPANDED gemm/relu/gemm dispatches into one cudaGraph (`begin_capture`/`end_capture`, 29b-2a), so its caller does NOT
// call apply_partition for it (it EXPANDS the run, then wraps the dispatches). ⛔ ~~29b captures the run into ONE native graph
// AND inserts the boundary transfer~~ **[STRUCK: 29b-2a did the capture; the boundary transfer is a plan STAGE at 29c (one CUDA
// memory domain at 29b ⇒ no cross-domain transfer yet); and "the run" a launch-graph captures is the EXPANDED plan, NOT a
// left-in-place claim]**; a per-op claim (subgraph=-1) is a single native kernel as before. Returns the expand count + first error.
[[nodiscard]] MlExpandResult apply_partition(Context& ctx, Module& m, const MlPartition& partition);

// The coopvec native provider's advertise predicate (the CLAIM check for the per-invocation cooperative-vector MLP): op is
// ml.mlp; activation == "relu"; input/weights/output all Float + rank-2; >=2 weights (coopvec needs >=1 HIDDEN layer); a UNIFORM
// hidden width (all intermediate widths equal — coopvec's single `hidden`); every dim (in/hidden/out) in [1, 1024]
// (CoopVecMlpConfig::valid). ⛔ false for ml.attention (no native attention kernel — the real can't-claim case) + for any
// non-relu / non-uniform-hidden / oversized ml.mlp.
[[nodiscard]] bool coopvec_can_claim_mlp(const Context& ctx, const Operation* op);

// CEIR-29b-2b — the CUDA-GRAPHS provider's advertise predicate. ⛔ A LAUNCH-GRAPH provider, NOT a semantic-engine one (coopvec):
// it does NOT replace the ml.mlp with a native fused kernel — it captures the CKIR-EXPANDED pipeline's dispatches (the SAME
// gemm/relu/gemm the fallback runs) into ONE cudaGraph. So the CLAIM only names WHICH runs this launch-mode covers (for the
// partition + 29c pinning); the run still EXPANDS + plans normally, then `begin_capture`/`end_capture` wrap the dispatches
// (29b-2a). Coverage = what emit_contract_cuda + relu.ckir + emit_elementwise_cuda cook: op is ml.mlp; input + >=2 weights
// (>=1 hidden layer); activation == "relu" (emit_contract_cuda does NOT unwrap a fused GemmRelu — the specialized-kernel scar,
// a gelu claim is a silent wrong-function); input/weights/output all Float + rank-2. ⛔ NO uniform-hidden / [1,1024] clamp
// (coopvec's CoopVecMlpConfig limits) — the CKIR gemm is general-dims, so the launch-graph provider covers general layer shapes.
[[nodiscard]] bool cuda_graphs_can_claim(const Context& ctx, const Operation* op);
} // namespace crd::ceir::gpu
