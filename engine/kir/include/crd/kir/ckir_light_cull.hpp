#pragma once

// ckir_light_cull.hpp — CEIR-18a-2 Stage 2b: the RE-AUTHORED Forward+/clustered light-cull PRODUCER builder.
//
// ⛔ HISTORY: CEIR-18a-1 built this as `build_cluster_light_cull`, emitted `scene_light_cull.ckir`, then DELETED the
// builder — but the whole 18a-1 batch was uncommitted, so `git log -S` found NOTHING to resurrect (the
// "uncommitted delete = lost source" scar). Re-authored here as the SAME sphere-vs-AABB test as ckir_render.hpp's
// `light_cluster_cull`, but written COMPONENT-WISE on scalar nodes (the compute/statement tier is scalar — the vec3
// helpers are RASTER-tier and `eval_cpu_kernel` asserts on Vec*/Dot). ⛔ KEEP THIS IN-TREE as the `[.emitckir]` regen
// source until it is COMMITTED; its deletion is a LATER slice (never inside the same uncommitted batch that created it).
//
// FORM (advisor-locked 2026-08-16): a `.ckir` COMPUTE kernel in the `palette_snapshot.ckir` mould — ONE group-buffer
// BufferDecl, all section bases read from HEADER WORDS the kernel indexes by baked constant (`buffer_load(buf, word)`),
// writing the per-cluster light list IN PLACE. The producer reads `header[list_off_word]` — the SAME word the clustered
// forward FS reads (`hdru(kHdrClusterOff)`) — so producer and consumer agree by construction on BOTH the raw-private
// (region_base 0) and absolute-consolidated paths, with no second base derivation to drift.
//
// ⛔ LAYERING: kir must NOT include scene-render's kHdr* constants. The header WORD INDICES are PARAMETERS; the scene
// emit site passes kHdrClusterOff / the froxel-AABB word / the light-input word, and states the triple-equality
// `num_lights == kMaxScenePointLights == crdl count.point` there.
//
// DISPATCH: one 1-workgroup dispatch per group; each invocation handles ONE cluster (`cluster = GlobalInvocationId.x`,
// threads >= num_clusters idle). Serial per-cluster compaction (ascending survivors, NO atomics — the bit-exact
// `eval_cpu_kernel` parity depends on it); EVERY slot written (survivors then `null_index = num_lights` padding — never
// rely on pre-cleared memory). No count output (no scene consumer; the ported kit derives counts from the list).

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

struct LightCullParams
{
    crd::u32 num_clusters = 16U; // tiles_x * tiles_y * num_slices
    crd::u32 num_lights   = 4U;  // == kMaxScenePointLights == crdl count.point (stated at the emit site)
    crd::u32 cap          = 8U;  // max_per_cluster (list stride)
    crd::u32 null_index   = 4U;  // padding sentinel; MUST be >= num_lights so the FS guard `slot < count` rejects it
    crd::u32 threads      = 64U; // workgroup size
    // header WORD INDICES holding each section's ABSOLUTE base offset (the scene passes the kHdr* constants):
    crd::u32 list_off_word  = 110U; // header[list_off_word]  = the per-cluster light-list base   (= kHdrClusterOff)
    crd::u32 aabb_off_word  = 113U; // header[aabb_off_word]  = the froxel-AABB table base         (= kHdrFroxelAabbOff)
    crd::u32 light_off_word = 114U; // header[light_off_word] = the {center.xyz, radius} light base (= kHdrLightViewOff)
};

// Build the light-cull compute kernel into (g, e). Returns false only on a degenerate parameter (num_lights > cap, or a
// zero grid) — a caller that ignores it still gets a well-formed but empty graph.
[[nodiscard]] inline bool build_cluster_light_cull(KGraph& g, KEntry& e, const LightCullParams& p)
{
    if (p.num_clusters == 0U || p.cap == 0U || p.num_lights > p.cap || p.null_index < p.num_lights) { return false; }

    const auto sh = make_shape({1});
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh, DType::U32); };
    // read a FLOAT word from the u32 group buffer (bit-reinterpret, the pull-shader convention).
    const auto lf = [&](int buf, int idx) { return g.int_bits_to_float(g.cast(g.buffer_load(buf, idx), DType::I32)); };
    const auto uadd = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto umul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    // ONE group buffer (set 0, binding 0), writable — reads the header + sections, writes the list in place.
    const int buf = g.buffer_decl(DType::U32, 0, 0, true);

    // section bases, read from the header words (indirection — the same word the FS reads for the list).
    const int list_base  = g.buffer_load(buf, ku(p.list_off_word));
    const int aabb_base  = g.buffer_load(buf, ku(p.aabb_off_word));
    const int light_base = g.buffer_load(buf, ku(p.light_off_word));

    // this invocation's cluster = LocalInvocationIndex (a SCALAR uint; the dispatch is ONE workgroup of `threads`, so the
    // flat local index IS the cluster). ⛔ NOT GlobalInvocationId (a uvec3): the compute/statement tier is SCALAR — a
    // vec3 builtin + swizzle cannot be evaluated by `eval_cpu_kernel` (it asserts on Vec*/Swizzle/Dot). Guard the tail.
    const int cluster = g.builtin(KBuiltin::LocalInvocationIndex);
    const int active  = g.binary(KOp::CmpLt, cluster, ku(p.num_clusters));
    const int guard   = g.stmt_if_begin(active);

    // this cluster's AABB (min.xyz, max.xyz) — 6 float words at aabb_base + cluster*6, read COMPONENT-WISE (scalar tier).
    const int abase = uadd(aabb_base, umul(cluster, ku(6U)));
    const int minx = lf(buf, abase);
    const int miny = lf(buf, uadd(abase, ku(1U)));
    const int minz = lf(buf, uadd(abase, ku(2U)));
    const int maxx = lf(buf, uadd(abase, ku(3U)));
    const int maxy = lf(buf, uadd(abase, ku(4U)));
    const int maxz = lf(buf, uadd(abase, ku(5U)));

    // this cluster's list slot base. ⛔ MATERIALIZE it: it is consumed inside EVERY per-light store-`if` (siblings), so
    // without this it emits INSIDE the first block and reads 'undeclared' in the next (the if-block shared-temp scar).
    const int lbase = uadd(list_base, umul(cluster, ku(p.cap)));
    g.stmt_materialize(lbase);

    // ⛔ COMPONENT-WISE sphere-vs-AABB (the render:: vec3 helpers are RASTER-tier; the compute tier is scalar). Closest-
    // point clamp per axis → squared distance → `Step(sq, r²)` = 1 iff sq ≤ r² (branchless, matches light_cluster_cull).
    const auto clampf = [&](int x, int lo, int hi) { return g.binary(KOp::Max, g.binary(KOp::Min, x, hi), lo); };
    const auto sqd = [&](int cxx, int cyy, int czz) {
        const int dx = g.binary(KOp::Sub, cxx, clampf(cxx, minx, maxx));
        const int dy = g.binary(KOp::Sub, cyy, clampf(cyy, miny, maxy));
        const int dz = g.binary(KOp::Sub, czz, clampf(czz, minz, maxz));
        return g.binary(KOp::Add, g.binary(KOp::Add, g.binary(KOp::Mul, dx, dx), g.binary(KOp::Mul, dy, dy)),
                        g.binary(KOp::Mul, dz, dz));
    };

    // serial compaction over the (unrolled) lights: survivors written ascending, `w` the running write cursor.
    const int half = g.constant(0.5, sh, DType::F32);
    int       w    = ku(0U);
    for (crd::u32 l = 0; l < p.num_lights; ++l)
    {
        const int lo     = uadd(light_base, ku(l * 4U));
        const int cx     = lf(buf, lo);
        const int cy     = lf(buf, uadd(lo, ku(1U)));
        const int cz     = lf(buf, uadd(lo, ku(2U)));
        const int radius = lf(buf, uadd(lo, ku(3U)));
        const int sq     = sqd(cx, cy, cz);
        const int hit    = g.binary(KOp::Step, sq, g.binary(KOp::Mul, radius, radius)); // 1.0 iff sq ≤ r² else 0.0
        // store the POINT-ARRAY index `l` at slot `w`, ONLY when hit; advance `w` by the (0/1) hit.
        const int sif = g.stmt_if_begin(g.binary(KOp::CmpGt, hit, half));
        g.stmt_buffer_store(buf, uadd(lbase, w), ku(l));
        g.stmt_if_end(sif);
        // advance the write cursor and MATERIALIZE it at the guard-body scope — the next light's store-`if` (a sibling
        // block) and the padding loop both read it, so it must be declared outside any store block (shared-temp scar).
        w = uadd(w, g.cast(hit, DType::U32));
        g.stmt_materialize(w);
    }
    // pad the tail [w .. cap-1] with null_index (unrolled; store only where slot >= w so survivors are untouched, and
    // EVERY slot ends written — the 18a-1 "never rely on pre-cleared memory" property).
    for (crd::u32 s = 0; s < p.cap; ++s)
    {
        const int pif = g.stmt_if_begin(g.binary(KOp::CmpGe, ku(s), w));
        g.stmt_buffer_store(buf, uadd(lbase, ku(s)), ku(p.null_index));
        g.stmt_if_end(pif);
    }

    g.stmt_if_end(guard);

    e                   = KEntry{};
    e.stage             = KStage::Compute;
    e.local_size[0]     = p.threads;
    e.local_size[1]     = 1U;
    e.local_size[2]     = 1U;
    e.kernel_body_begin = 0;
    e.kernel_body_count = g.stmt_count();
    return true;
}

} // namespace crd::kir
