#pragma once

// ckir_lower.hpp — the material LOWERING pass (D-007 B7): a graph→IR compiler that turns an authored material graph into a
// stage-assigned, optimized, variant-ready program. Three transforms, every one ROUND-TRIP BIT-STABLE (the lowered material
// evaluates IDENTICALLY to the original — verified on the CPU oracle + observed pixel-identical on both backends):
//   1. `classify` — the "cheapest correct stage" (FREQUENCY) analysis: each node is Constant / Uniform / Vertex / Fragment,
//      the coarsest stage at which it can be correctly computed. This is the foundation the other two build on.
//   2. `lower` — runs the existing semantics-preserving optimizer (const-fold → DCE → CSE hash-cons, `KGraph::optimize`),
//      and (B7-b) hoists uniform subexpressions off the per-fragment path.
//   3. `specialize` (B7-b) — a static switch/select on a compile-time selector becomes the selected branch; `optimize`
//      then folds the dead branches away, yielding a `ShaderOption` VARIANT. (The full permutation matrix + dedup is D3.)
//
// FREQUENCY is exactly ADR-0102's variability axis: a Const is compile-time; a `UniformBlock` read is per-draw (its `dset`
// is the frame/pass/material/object slot); a vertex builtin / a VS stage input is per-vertex; an interpolated varying, a
// fragment builtin (FragCoord), a derivative, an implicit-LOD texture sample, or a storage read is per-fragment. An interior
// node is the MAX of its inputs, pinned to Fragment by the fragment-only ops. Computing a node at its frequency (not finer)
// is the cheapest correct choice; the transforms move work coarser (fold to Constant, hoist to Uniform) without changing a
// single evaluated value.

#include <crd/kir/ckir.hpp>

namespace crd::kir::lower
{

// The stage at which a node can be correctly computed, ordered coarsest → finest. `u8` values are comparable so `max`
// gives the finest of two frequencies.
enum class Frequency : crd::u8
{
    Constant = 0, // compile-time (a `Const`, or a fold of only-constant inputs)
    Uniform  = 1, // per-draw: a `UniformBlock`/texture binding, or a chain over only uniforms
    Vertex   = 2, // per-vertex: a vertex builtin (VertexIndex) or a VS stage input
    Fragment = 3, // per-fragment: an interpolated varying, FragCoord, a derivative, an implicit-LOD sample, a storage read
};

[[nodiscard]] inline Frequency freq_max(Frequency a, Frequency b) noexcept { return static_cast<crd::u8>(a) >= static_cast<crd::u8>(b) ? a : b; }

// An op that MUST run per-fragment regardless of its inputs' frequency: the 2×2-quad derivatives, an implicit-LOD texture
// sample (needs those derivatives), and a fragment storage read. (Explicit-LOD / grad / fetch / gather / size do NOT force
// it — they are legal earlier — so they classify by their inputs, like any other op.)
[[nodiscard]] inline bool is_fragment_forcing(KOp op) noexcept
{
    return is_fragment_only_op(op) || op == KOp::TexSample || op == KOp::StorageLoad;
}

// classify(g, entry, out) — fill `out[i]` (size ≥ g.size()) with node i's cheapest-correct-stage frequency. One forward
// pass suffices: a node's operands always have smaller ids (the graph is built bottom-up / topologically ordered).
inline void classify(const KGraph& g, const KEntry& e, Frequency* out) noexcept
{
    const bool is_vertex = (e.stage == KStage::Vertex);
    const int  n         = g.size();
    for (int i = 0; i < n; ++i)
    {
        const KNode& nd = g.node(i);
        Frequency    f  = Frequency::Constant;
        switch (nd.op)
        {
        case KOp::Const: f = Frequency::Constant; break;
        case KOp::UniformBlock: case KOp::Texture: case KOp::Sampler: f = Frequency::Uniform; break;
        case KOp::Builtin:
        {
            const crd::u32 st = builtin_info(static_cast<KBuiltin>(nd.iidx)).stages;
            // a vertex-only builtin (VertexIndex/InstanceIndex) is per-vertex; anything readable in a fragment (FragCoord,
            // InnerCoverage, …) is per-fragment. (Compute builtins never reach a raster material.)
            f = (st == stage_mask::kVertex) ? Frequency::Vertex : Frequency::Fragment;
            break;
        }
        // a stage input is a vertex ATTRIBUTE in a VS (per-vertex) but an INTERPOLATED varying in an FS (per-fragment).
        case KOp::StageIn: f = is_vertex ? Frequency::Vertex : Frequency::Fragment; break;
        // compute leaves — not part of a raster material; treat as per-invocation (finest) so nothing hoists across them.
        case KOp::Input: case KOp::Iota: f = Frequency::Fragment; break;
        default:
        {
            const int ops[4] = {nd.a, nd.b, nd.c, nd.d};
            for (int k = 0; k < 4; ++k) { if (ops[k] >= 0) { f = freq_max(f, out[ops[k]]); } }
            for (int k = 0; k < static_cast<int>(nd.n_ext); ++k) { f = freq_max(f, out[g.ext_operand(nd, k)]); }
            if (is_fragment_forcing(nd.op)) { f = Frequency::Fragment; }
            break;
        }
        }
        out[i] = f;
    }
}

// The frequency of a whole entry's result set — the finest frequency any of its live outputs / sinks reaches. (A fragment
// sink — discard, frag-depth, a storage write — is inherently per-fragment.)
[[nodiscard]] inline Frequency entry_frequency(const KEntry& e, const Frequency* freq) noexcept
{
    Frequency f = Frequency::Constant;
    if (e.position >= 0) { f = freq_max(f, freq[e.position]); }
    if (e.frag_depth >= 0) { f = freq_max(f, freq[e.frag_depth]); }
    if (e.discard_cond >= 0) { f = Frequency::Fragment; }
    if (e.storage_write_value >= 0) { f = Frequency::Fragment; }
    for (int k = 0; k < e.n_out; ++k) { f = freq_max(f, freq[e.out[k].node]); }
    return f;
}

// lower(g, roots, n_roots) — the semantics-preserving optimizer (const-fold → DCE → CSE hash-cons). Updates `roots[]` to the
// new ids. This is the value-preserving core every material lowering runs; it is bit-stable and idempotent (proven in
// tests/kir/test_ckir_lower.cpp + the pre-existing test_ckir_opt.cpp). Kept as the single named entry point so B8's cook
// path calls `lower`, not `optimize`, and picks up the stage transforms below for free.
inline void lower(KGraph& g, int* roots, int n_roots) { g.optimize(roots, n_roots); }

// Lower a whole material ENTRY: gather its live roots (clip position · frag-depth · discard · shading-rate · storage
// write · every colour output), run `lower`, and write the renumbered ids back into the entry. The single call a cook /
// renderer makes — after it, `create_program(g, e)` compiles the optimized graph. BIT-STABLE: the entry renders identically
// (verified pixel-identical on both backends in tests/gpu-context-*).
inline void lower_entry(KGraph& g, KEntry& e)
{
    // ⛔⛔ EVERY entry field that names a live value node is a ROOT — `lower`/DCE renumbers the graph and rewrites
    // these slots in place, so an OMITTED one is left DANGLING (a stale pre-DCE id the emitter later dereferences,
    // reading a garbage operand off the end of the compacted graph). `mesh_prim` and the task amplification nodes
    // (`task_emit`, `task_payload[]`) were missing: a MESH entry whose primitive-index subtree is DISJOINT from
    // `position` — exactly a real Nanite cluster unpack — had its `mesh_prim` DCE'd out from under it. The F6
    // skeleton never surfaced it (its trivial `mesh_prim` shared `position`'s subtree, so it survived by accident).
    int* slots[kMaxStageOutputs + 12];
    int  n = 0;
    if (e.position >= 0) { slots[n++] = &e.position; }
    if (e.frag_depth >= 0) { slots[n++] = &e.frag_depth; }
    if (e.discard_cond >= 0) { slots[n++] = &e.discard_cond; }
    if (e.shading_rate >= 0) { slots[n++] = &e.shading_rate; }
    if (e.mesh_prim >= 0) { slots[n++] = &e.mesh_prim; }         // B4 MESH: the per-primitive local-index triple
    if (e.task_emit >= 0) { slots[n++] = &e.task_emit; }         // B4 TASK: the mesh-workgroup amplification count
    for (crd::u32 k = 0; k < e.n_task_payload; ++k) { if (e.task_payload[k] >= 0) { slots[n++] = &e.task_payload[k]; } }
    if (e.storage_write_index >= 0) { slots[n++] = &e.storage_write_index; }
    if (e.storage_write_value >= 0) { slots[n++] = &e.storage_write_value; }
    for (int k = 0; k < e.n_out; ++k) { slots[n++] = &e.out[k].node; }
    int roots[kMaxStageOutputs + 12];
    for (int i = 0; i < n; ++i) { roots[i] = *slots[i]; }
    lower(g, roots, n);
    for (int i = 0; i < n; ++i) { *slots[i] = roots[i]; }
}

// ── stage split: the uniform/fragment boundary (B7-b) ────────────────────────────────────────────────────────────────
// The maximal Uniform-frequency subexpressions consumed on the PER-FRAGMENT path — a Uniform node with a Fragment-frequency
// consumer (or that is a fragment entry output). These are exactly the sites to HOIST off the per-fragment path: compute
// them per-draw (as a uniform) or per-vertex (a FLAT varying) instead of per-pixel. The hoist is BIT-STABLE by construction
// — a Uniform value is constant across the primitive, so evaluating it once (per-draw / flat-per-vertex) delivers the
// identical value the fragment would have recomputed. `seen` is scratch of size ≥ g.size(); returns the boundary count
// (writes up to `cap` node ids into `out`). (The physical VS-varying / uniform-buffer materialization lands with B8's cook
// + renderer plumbing; B7 owns the classification + the value-neutral boundary.)
inline int uniform_boundary(const KGraph& g, const KEntry& e, const Frequency* freq, crd::u8* seen, int* out, int cap) noexcept
{
    const int n = g.size();
    for (int i = 0; i < n; ++i) { seen[static_cast<crd::usize>(i)] = 0; }
    int        cnt = 0;
    const auto add = [&](int u)
    {
        if (u < 0 || freq[u] != Frequency::Uniform || seen[static_cast<crd::usize>(u)] != 0) { return; }
        seen[static_cast<crd::usize>(u)] = 1;
        if (cnt < cap) { out[cnt] = u; }
        ++cnt;
    };
    for (int i = 0; i < n; ++i)
    {
        if (freq[i] != Frequency::Fragment) { continue; } // only a fragment consumer creates a hoistable boundary
        const KNode& nd = g.node(i);
        add(nd.a);
        add(nd.b);
        add(nd.c);
        add(nd.d);
        for (int k = 0; k < static_cast<int>(nd.n_ext); ++k) { add(g.ext_operand(nd, k)); }
    }
    if (e.stage == KStage::Fragment) // a Uniform value emitted straight to a fragment output is recomputed per-pixel for nothing
    {
        for (int k = 0; k < e.n_out; ++k) { add(e.out[k].node); }
        if (e.frag_depth >= 0) { add(e.frag_depth); }
    }
    return cnt;
}

// ── static switch → ShaderOption variant (B7-b) ──────────────────────────────────────────────────────────────────────
// specialize(g, option, value, roots, n) — pin a `ShaderOption` selector to a compile-time `value`, then lower: the static
// switch/select it drives collapses to the chosen branch and DCE removes the other → a smaller VARIANT that evaluates
// IDENTICALLY to the runtime switch taken with that selector. Destructive (KGraph::pin_const rewrites in place) — build or
// copy the graph per variant. The variant KEY is (option, value); the full permutation matrix + dedup is D3.
// Collapse every `Select` whose condition is now a compile-time constant to its chosen branch (aliasing the node to the
// live branch, so DCE reclaims the dead one). Run after pinning the option + a const-fold has turned the switch condition
// constant. `Select` covers `ifgreater`/`ifequal`/`switch` (all lower to `Select` chains in the node library).
inline void fold_static_branches(KGraph& g) noexcept
{
    const int n = g.size();
    for (int i = 0; i < n; ++i)
    {
        const KNode& nd = g.node(i);
        if (nd.op != KOp::Select || nd.c < 0 || g.node(nd.c).op != KOp::Const) { continue; }
        g.alias(i, g.node(nd.c).cval != 0.0 ? nd.a : nd.b); // cond ? a : b
    }
}

inline void specialize(KGraph& g, int option, crd::f64 value, int* roots, int n_roots)
{
    g.pin_const(option, value); // the ShaderOption is now a compile-time constant
    g.optimize(roots, n_roots); // fold the option into the switch conditions
    fold_static_branches(g);    // static-cond selects → their chosen branch
    g.optimize(roots, n_roots); // DCE the dead branches + CSE
}

} // namespace crd::kir::lower
