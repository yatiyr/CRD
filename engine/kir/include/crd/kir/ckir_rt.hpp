#pragma once

// ckir_rt.hpp — D-007 B9/RT: the CKIR RAY-TRACING kernel library. Inline-ray-query compute kernels (VK_KHR_ray_query /
// DXR-1.1 inline) that ride the AS-bound compute dispatch (VulkanRayTracingContext). RT-1 gave the primitive
// (`trace_ray_closest`); this is where the real RT effects live — shadows now, then AO / reflections / GI / a path-tracing
// megakernel. Every kernel: TLAS at binding 0, its buffers after. Verified GPU-vs-CPU-brute-force-ray-triangle within
// geometric tolerance (RT traversal is not bit-exact across vendors — the honest RT contract).

#include <crd/kir/ckir.hpp>

namespace crd::kir::rt
{

// RT-1: one thread per ray — cast the ray at the TLAS and store the closest-hit distance (or tmax on miss). Buffers:
// TLAS (b0), rays (b1, 6 floats each = origin.xyz, dir.xyz), out distance (b2, one float each).
[[nodiscard]] inline crd::kir::KEntry build_ray_trace_kernel(crd::kir::KGraph& g, crd::u32 local_size)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };

    const int as   = g.accel_struct_decl(0, 0);
    const int rays = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int out  = g.buffer_decl(k::DType::F32, 0, 2, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = g.binary(k::KOp::Add, g.binary(k::KOp::Mul, g.builtin(k::KBuiltin::WorkgroupIndex), cu(local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  base = g.binary(k::KOp::Mul, tid, cu(6U));
    const auto ld   = [&](crd::u32 c) { return g.buffer_load(rays, g.binary(k::KOp::Add, base, cu(c))); };
    const int  t    = g.trace_ray_closest(as, ld(0), ld(1), ld(2), ld(3), ld(4), ld(5), cf(0.001), cf(1.0e30));
    g.stmt_buffer_store(out, tid, t);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// FA-2 (portable RT PIPELINE authored in CKIR): the three stages of the "big rig" trace — raygen reads a ray from a buffer
// (indexed by gl_LaunchIDEXT.x), traces it through the pipeline (invoking closest-hit / miss), and stores the payload's t; the
// closest-hit writes gl_HitTEXT; the miss writes a large t. `use_ser` weaves a SER reorder HINT into the raygen (honored where the
// target supports SER, dropped otherwise). Each stage is its OWN KGraph (the emitter scans the graph for that stage's decls).
// Buffers: TLAS (b0), rays (b1, 6f each: origin.xyz + dir.xyz), out distance (b2). Lowers to GLSL rgen/rchit/rmiss + DXR HLSL.
[[nodiscard]] inline crd::kir::KEntry build_rt_pipeline_raygen(crd::kir::KGraph& g, bool use_ser)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const int as   = g.accel_struct_decl(0, 0);
    const int rays = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int out  = g.buffer_decl(k::DType::F32, 0, 2, true);
    const int pl   = g.ray_payload_decl(1);
    const int mark = g.kernel_stmt_mark();
    const int lid  = g.vec_comp(g.builtin(k::KBuiltin::LaunchId), 0); // gl_LaunchIDEXT.x — the 1-D launch index
    const int base = g.binary(k::KOp::Mul, lid, cu(6U));
    const auto ld  = [&](crd::u32 c) { return g.buffer_load(rays, g.binary(k::KOp::Add, base, cu(c))); };
    g.stmt_trace_ray_pipeline(as, pl, ld(0), ld(1), ld(2), ld(3), ld(4), ld(5), cf(0.001), cf(1.0e30));
    if (use_ser) { g.stmt_reorder_thread(); }
    g.stmt_buffer_store(out, lid, g.payload_load(pl, 0));
    k::KEntry e;
    e.stage             = k::KStage::RayGen;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
[[nodiscard]] inline crd::kir::KEntry build_rt_pipeline_closesthit(crd::kir::KGraph& g)
{
    namespace k    = crd::kir;
    const int pl   = g.ray_payload_decl(1);
    const int mark = g.kernel_stmt_mark();
    g.stmt_payload_store(pl, 0, g.builtin(k::KBuiltin::HitT)); // payload.t = gl_HitTEXT (the closest hit distance)
    k::KEntry e;
    e.stage             = k::KStage::ClosestHit;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
[[nodiscard]] inline crd::kir::KEntry build_rt_pipeline_miss(crd::kir::KGraph& g)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const int pl       = g.ray_payload_decl(1);
    const int mark     = g.kernel_stmt_mark();
    g.stmt_payload_store(pl, 0, g.constant(1.0e30, sh1, k::DType::F32)); // payload.t = tmax (miss)
    k::KEntry e;
    e.stage             = k::KStage::Miss;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// P4 (the portable OMM FALLBACK, authored in CKIR): an ANY-HIT shader that alpha-tests a SUB-TRIANGLE region — the candidate hit
// is IGNORED where the barycentric u+v < `cutoff`, so that half of each triangle is "transparent" and rays pass through it. This
// is the software equivalent of a 2-state opacity micromap: when the adapter lacks hardware OMM, a portable consumer builds a
// non-opaque scene + this any-hit shader and gets alpha-tested geometry that's CORRECT (just slower than the HW micromap).
[[nodiscard]] inline crd::kir::KEntry build_rt_pipeline_anyhit_alpha(crd::kir::KGraph& g, double cutoff)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    (void)g.ray_payload_decl(1); // the any-hit shares the payload signature (unused here)
    const int mark = g.kernel_stmt_mark();
    const int bary = g.builtin(k::KBuiltin::HitBary);
    const int u    = g.vec_comp(bary, 0);
    const int v    = g.vec_comp(bary, 1);
    const int a    = g.binary(k::KOp::Add, u, v);
    const int cut  = g.constant(cutoff, sh1, k::DType::F32);
    g.stmt_ignore_hit_if(g.binary(k::KOp::CmpLt, a, cut)); // transparent where u+v < cutoff ⇒ ignore ⇒ traversal continues
    k::KEntry e;
    e.stage             = k::KStage::AnyHit;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

struct RtShadowConfig
{
    float    light[3]   = {0.0F, 5.0F, 0.0F}; // world-space point light the shadow rays test against
    crd::u32 local_size = 64U;
};

// RT hard shadows: one thread per surface point — cast a shadow ray from the point toward the light and store the visibility
// (1 = lit, 0 = occluded). The ray is parameterised P + t·(L−P), so t∈(ε,1) hits BETWEEN the point and the light ⇒ occluded
// (no normalise needed — the closest-hit t is in units of |L−P|). Buffers: TLAS (b0), positions (b1, 3 floats each),
// visibility out (b2). This is the direct RT VISIBILITY LEAF the B14 ReSTIR / GI shadow term consumes.
[[nodiscard]] inline crd::kir::KEntry build_rt_shadow_kernel(crd::kir::KGraph& g, const RtShadowConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };

    const int as  = g.accel_struct_decl(0, 0);
    const int pos = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int vis = g.buffer_decl(k::DType::F32, 0, 2, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = g.binary(k::KOp::Add, g.binary(k::KOp::Mul, g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  base = g.binary(k::KOp::Mul, tid, cu(3U));
    const int  px   = g.buffer_load(pos, g.binary(k::KOp::Add, base, cu(0U)));
    const int  py   = g.buffer_load(pos, g.binary(k::KOp::Add, base, cu(1U)));
    const int  pz   = g.buffer_load(pos, g.binary(k::KOp::Add, base, cu(2U)));
    const int  dx   = sub(cf(static_cast<double>(cfg.light[0])), px); // ray dir = L − P (un-normalised; the light is at t = 1)
    const int  dy   = sub(cf(static_cast<double>(cfg.light[1])), py);
    const int  dz   = sub(cf(static_cast<double>(cfg.light[2])), pz);
    // tmin small (skip the origin self-hit), tmax just under 1 (don't count the light-side surface as an occluder).
    const int  t    = g.trace_ray_closest(as, px, py, pz, dx, dy, dz, cf(1.0e-3), cf(0.999));
    // occluded iff a hit landed strictly before the light ⇒ visibility = (t < 0.999) ? 0 : 1.
    const int  occl = g.binary(k::KOp::CmpLt, t, cf(0.999));
    const int  v    = g.select(occl, cf(0.0), cf(1.0));
    g.stmt_buffer_store(vis, tid, v);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

namespace detail
{
// triple32 (Wellons) integer hash → a uniform float in [0,1). Deterministic ⇒ bit-identical GPU/oracle (u32 ops wrap mod
// 2^32 in both — see apply_binary_typed). Uses the top 24 bits so float(h>>8) is EXACT. The reproducible sampling that lets
// the noisy RT estimators (AO now; ReSTIR/path-tracing later) be validated GPU-vs-oracle and denoised consistently by TAA.
[[nodiscard]] inline int rt_hash01(crd::kir::KGraph& g, int seed)
{
    namespace k       = crd::kir;
    const k::Shape sh = k::make_shape({1});
    const auto     cu = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh, k::DType::U32); };
    const auto     x  = [&](int a, int b) { return g.binary(k::KOp::BitXor, a, b); };
    const auto     m  = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     r  = [&](int a, crd::u32 s) { return g.binary(k::KOp::Shr, a, cu(s)); };
    int            h  = seed;
    h = x(h, r(h, 17U)); h = m(h, cu(0xED5AD4BBU));
    h = x(h, r(h, 11U)); h = m(h, cu(0xAC4C1B51U));
    h = x(h, r(h, 15U)); h = m(h, cu(0x31848BABU));
    h = x(h, r(h, 14U));
    return g.binary(k::KOp::Mul, g.cast(r(h, 8U), k::DType::F32), g.constant(1.0 / 16777216.0, sh, k::DType::F32));
}
} // namespace detail

struct RtaoConfig
{
    crd::u32 samples    = 32U;  // hemisphere rays per point (== TAA frames when 1/frame)
    float    radius     = 3.0F; // occlusion falloff radius (hits beyond it don't darken)
    crd::u32 local_size = 64U;
};

// RT AMBIENT OCCLUSION: one thread per surface point — cast `samples` cosine-weighted hemisphere rays around the normal and
// return the ambient VISIBILITY (1 = open, 0 = fully occluded), the mean of (ray missed within `radius`). Cosine weighting
// makes the estimator the plain average of visibility (the cosθ/π cancels the pdf). This is the batch-of-rays-per-pixel LOOP
// pattern the path tracer needs: a runtime `For` over samples, a `trace_ray_closest` per iteration, occlusion accumulated via
// buffer RMW. Buffers: TLAS (b0), positions (b1, 3f), NORMALS (b2, 3f, unit), AO out (b3, 1f).
[[nodiscard]] inline crd::kir::KEntry build_rtao_kernel(crd::kir::KGraph& g, const RtaoConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     dvv = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     uxor = [&](int a, int b) { return g.binary(k::KOp::BitXor, a, b); };

    const int as  = g.accel_struct_decl(0, 0);
    const int pos = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int out = g.buffer_decl(k::DType::F32, 0, 3, true);

    const int mark = g.kernel_stmt_mark();
    const int tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int b3   = umul(tid, cu(3U));
    const int px   = g.buffer_load(pos, uadd(b3, cu(0U)));
    const int py   = g.buffer_load(pos, uadd(b3, cu(1U)));
    const int pz   = g.buffer_load(pos, uadd(b3, cu(2U)));
    const int nx   = g.buffer_load(nrm, uadd(b3, cu(0U)));
    const int ny   = g.buffer_load(nrm, uadd(b3, cu(1U)));
    const int nz   = g.buffer_load(nrm, uadd(b3, cu(2U)));

    // orthonormal tangent frame around N (Duff et al. 2017 — branchless, never divides by zero since |sign+nz| ≥ 1).
    const int sign = g.select(g.binary(k::KOp::CmpGe, nz, cf(0.0)), cf(1.0), cf(-1.0));
    const int fa   = dvv(cf(-1.0), add(sign, nz));
    const int fb   = mul(mul(nx, ny), fa);
    const int tx   = add(cf(1.0), mul(sign, mul(mul(nx, nx), fa)));
    const int ty   = mul(sign, fb);
    const int tz   = mul(g.unary(k::KOp::Neg, sign), nx);
    const int bx   = fb;
    const int by   = add(sign, mul(mul(ny, ny), fa));
    const int bz   = g.unary(k::KOp::Neg, ny);
    // ray origin, nudged off the surface along N to dodge the self-hit.
    const int ox = add(px, mul(cf(1.0e-3), nx));
    const int oy = add(py, mul(cf(1.0e-3), ny));
    const int oz = add(pz, mul(cf(1.0e-3), nz));

    g.stmt_buffer_store(out, tid, cf(0.0)); // occlusion accumulator ← 0

    const int floop = g.stmt_for_begin(cu(cfg.samples));
    const int s     = g.kernel_loop_var(floop);
    const int seed  = uadd(umul(s, cu(0x9E3779B9U)), cu(1U));
    const int u1    = detail::rt_hash01(g, uxor(umul(tid, cu(0x632BE5ABU)), seed));
    const int u2    = detail::rt_hash01(g, uxor(umul(tid, cu(0x85157AF5U)), umul(seed, cu(0xC2B2AE35U))));
    // cosine-weighted hemisphere sample (local frame z = N): r=√u1, φ=2π·u2, z=√(1−u1).
    const int rr  = g.unary(k::KOp::Sqrt, u1);
    const int phi = mul(cf(2.0 * 3.14159265358979323846), u2);
    const int lx  = mul(rr, g.unary(k::KOp::Cos, phi));
    const int ly  = mul(rr, g.unary(k::KOp::Sin, phi));
    const int lz  = g.unary(k::KOp::Sqrt, g.binary(k::KOp::Max, sub(cf(1.0), u1), cf(0.0)));
    // world-space direction = lx·T + ly·B + lz·N.
    const int dx = add(add(mul(lx, tx), mul(ly, bx)), mul(lz, nx));
    const int dy = add(add(mul(lx, ty), mul(ly, by)), mul(lz, ny));
    const int dz = add(add(mul(lx, tz), mul(ly, bz)), mul(lz, nz));
    const int t  = g.trace_ray_closest(as, ox, oy, oz, dx, dy, dz, cf(1.0e-3), cf(static_cast<double>(cfg.radius)));
    const int occ = g.select(g.binary(k::KOp::CmpLt, t, cf(static_cast<double>(cfg.radius))), cf(1.0), cf(0.0));
    g.stmt_buffer_store(out, tid, add(g.buffer_load(out, tid), occ)); // accumulate occlusion
    g.stmt_for_end(floop);

    // ambient visibility = 1 − occludedFraction.
    g.stmt_buffer_store(out, tid, sub(cf(1.0), mul(g.buffer_load(out, tid), cf(1.0 / static_cast<double>(cfg.samples)))));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

struct RtReflectConfig
{
    float    view[3]   = {0.0F, -1.0F, 0.0F};  // incident view direction (camera → surface)
    float    light[3]  = {0.577F, 0.577F, 0.577F}; // directional light (unit)
    float    albedo[3] = {0.8F, 0.5F, 0.3F};   // reflected-surface albedo
    crd::u32 ntri      = 1U;                    // triangle count (clamps the primId geometry fetch)
    crd::u32 local_size = 64U;
};

// RT REFLECTIONS — the RICH-HIT effect (the last primitive the path tracer needs). Per surface point: reflect the view ray
// about the normal, trace the reflected ray (`trace_ray_hit` → distance + PRIMITIVE INDEX), then SHADE the hit — fetch the hit
// triangle's flat normal (indexed by primId) and Lambert-shade it — or sample an environment gradient on a miss. This is the
// reflected-ray → hit → fetch-geometry → shade chain that generalises to path-tracing bounces + ReSTIR GI. Scalar per channel
// (the compute-kernel emitter is scalar-only). Buffers: TLAS (b0), positions (b1, 3f), normals (b2, 3f), per-triangle flat
// normals (b3, 3f each), reflection colour out (b4, 3f).
[[nodiscard]] inline crd::kir::KEntry build_rt_reflection_kernel(crd::kir::KGraph& g, const RtReflectConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     mx  = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };
    const auto     mn  = [&](int a, int b) { return g.binary(k::KOp::Min, a, b); };
    const auto     sat = [&](int x) { return mn(mx(x, cf(0.0)), cf(1.0)); };
    const auto     mix = [&](int a, int b, int t) { return add(a, mul(sub(b, a), t)); };

    const int as   = g.accel_struct_decl(0, 0);
    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int tn   = g.buffer_decl(k::DType::F32, 0, 3, false); // per-triangle flat normals
    const int out  = g.buffer_decl(k::DType::F32, 0, 4, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = add(mul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = mul(tid, cu(3U));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, add(base, cu(c))); };
    const int  px = lp(pos, b3, 0U), py = lp(pos, b3, 1U), pz = lp(pos, b3, 2U);
    const int  nx = lp(nrm, b3, 0U), ny = lp(nrm, b3, 1U), nz = lp(nrm, b3, 2U);

    // reflect the (constant) view direction about N: R = V − 2(V·N)N.
    const int vx = cf(static_cast<double>(cfg.view[0])), vy = cf(static_cast<double>(cfg.view[1])), vz = cf(static_cast<double>(cfg.view[2]));
    const int vdn = add(add(mul(vx, nx), mul(vy, ny)), mul(vz, nz));
    const int rx  = sub(vx, mul(mul(cf(2.0), vdn), nx));
    const int ry  = sub(vy, mul(mul(cf(2.0), vdn), ny));
    const int rz  = sub(vz, mul(mul(cf(2.0), vdn), nz));
    const int ox = add(px, mul(cf(1.0e-3), nx)), oy = add(py, mul(cf(1.0e-3), ny)), oz = add(pz, mul(cf(1.0e-3), nz));

    const k::KGraph::RtHit hit = g.trace_ray_hit(as, ox, oy, oz, rx, ry, rz, cf(1.0e-3), cf(1.0e30));
    const int miss = g.binary(k::KOp::CmpEq, hit.prim, cu(0xFFFFFFFFU));

    // environment gradient on a miss (sky by the reflected-ray elevation R.y).
    const int sy   = sat(add(mul(ry, cf(0.5)), cf(0.5)));
    const int skyR = mix(cf(0.55), cf(0.15), sy);
    const int skyG = mix(cf(0.70), cf(0.35), sy);
    const int skyB = mix(cf(0.95), cf(0.80), sy);

    // hit shading: fetch the hit triangle's flat normal (clamp the index so a miss's fetch is in-bounds), Lambert-shade it.
    const int cprim = mn(hit.prim, cu(cfg.ntri > 0U ? cfg.ntri - 1U : 0U));
    const int tb    = mul(cprim, cu(3U));
    const int tnx = lp(tn, tb, 0U), tny = lp(tn, tb, 1U), tnz = lp(tn, tb, 2U);
    const int ndl = mx(add(add(mul(tnx, cf(static_cast<double>(cfg.light[0]))), mul(tny, cf(static_cast<double>(cfg.light[1])))), mul(tnz, cf(static_cast<double>(cfg.light[2])))), cf(0.0));
    const int shade = add(cf(0.2), mul(cf(0.8), ndl)); // ambient + diffuse
    const int hitR = mul(cf(static_cast<double>(cfg.albedo[0])), shade);
    const int hitG = mul(cf(static_cast<double>(cfg.albedo[1])), shade);
    const int hitB = mul(cf(static_cast<double>(cfg.albedo[2])), shade);

    g.stmt_buffer_store(out, add(b3, cu(0U)), g.select(miss, skyR, hitR));
    g.stmt_buffer_store(out, add(b3, cu(1U)), g.select(miss, skyG, hitG));
    g.stmt_buffer_store(out, add(b3, cu(2U)), g.select(miss, skyB, hitB));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

struct PathTraceConfig
{
    crd::u32 samples    = 16U; // paths per point (== TAA frames when 1/frame)
    crd::u32 bounces    = 3U;  // diffuse bounce depth (unrolled)
    float    albedo[3]  = {0.7F, 0.7F, 0.7F}; // uniform diffuse albedo of the scene surfaces
    crd::u32 ntri       = 1U;  // triangle count (clamps the primId geometry fetch)
    crd::u32 local_size = 64U;
};

// PATH-TRACING MEGAKERNEL — the full light-transport integrator, composed from the RT primitives. Per surface point: shoot
// `samples` cosine-weighted paths; each path bounces diffusely (`trace_ray_hit` → hit → fetch normal → cosine-scatter,
// throughput ×= albedo) until it ESCAPES to the sky (the light) or reaches the bounce cap, accumulating radiance. The SAMPLE
// loop is a runtime `For` (radiance via buffer RMW); the BOUNCE loop is UNROLLED (CKIR's For carries no registers, so
// origin/dir/throughput chain as SSA nodes across bounces). Deterministic triple32 sampling ⇒ GPU==oracle. This is diffuse
// global illumination — and the frame ReSTIR PT / NEE / MIS slot into next. Buffers: TLAS (b0), positions (b1, 3f), normals
// (b2, 3f), per-triangle flat normals (b3, 3f each), radiance out (b4, 3f).
[[nodiscard]] inline crd::kir::KEntry build_pathtrace_kernel(crd::kir::KGraph& g, const PathTraceConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     mx  = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };
    const auto     mn  = [&](int a, int b) { return g.binary(k::KOp::Min, a, b); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     uxor = [&](int a, int b) { return g.binary(k::KOp::BitXor, a, b); };
    const auto     sat  = [&](int x) { return mn(mx(x, cf(0.0)), cf(1.0)); };
    const auto     mix  = [&](int a, int b, int t) { return add(a, mul(sub(b, a), t)); };

    const int as   = g.accel_struct_decl(0, 0);
    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int tn   = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int out  = g.buffer_decl(k::DType::F32, 0, 4, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = umul(tid, cu(3U));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, uadd(base, cu(c))); };
    const int  pnx = lp(nrm, b3, 0U), pny = lp(nrm, b3, 1U), pnz = lp(nrm, b3, 2U); // surface normal
    const int  ppx = lp(pos, b3, 0U), ppy = lp(pos, b3, 1U), ppz = lp(pos, b3, 2U); // surface point

    // orthonormal tangent frame around a normal (Duff 2017) → fills t[3], b[3].
    const auto frame = [&](int nx, int ny, int nz, int t[3], int b[3]) {
        const int sg = g.select(g.binary(k::KOp::CmpGe, nz, cf(0.0)), cf(1.0), cf(-1.0));
        const int fa = g.binary(k::KOp::Div, cf(-1.0), add(sg, nz));
        const int fb = mul(mul(nx, ny), fa);
        t[0] = add(cf(1.0), mul(sg, mul(mul(nx, nx), fa))); t[1] = mul(sg, fb); t[2] = mul(g.unary(k::KOp::Neg, sg), nx);
        b[0] = fb; b[1] = add(sg, mul(mul(ny, ny), fa)); b[2] = g.unary(k::KOp::Neg, ny);
    };
    // cosine-weighted hemisphere direction (local z = N) → fills d[3].
    const auto cosine_dir = [&](int u1, int u2, int nx, int ny, int nz, const int t[3], const int b[3], int d[3]) {
        const int rr  = g.unary(k::KOp::Sqrt, u1);
        const int phi = mul(cf(2.0 * 3.14159265358979323846), u2);
        const int lx  = mul(rr, g.unary(k::KOp::Cos, phi));
        const int ly  = mul(rr, g.unary(k::KOp::Sin, phi));
        const int lz  = g.unary(k::KOp::Sqrt, mx(sub(cf(1.0), u1), cf(0.0)));
        d[0] = add(add(mul(lx, t[0]), mul(ly, b[0])), mul(lz, nx));
        d[1] = add(add(mul(lx, t[1]), mul(ly, b[1])), mul(lz, ny));
        d[2] = add(add(mul(lx, t[2]), mul(ly, b[2])), mul(lz, nz));
    };
    // sky radiance by ray elevation (the light source): bright, blue-tinted upward.
    const auto sky = [&](int diry, int c[3]) {
        const int sy = sat(add(mul(diry, cf(0.5)), cf(0.5)));
        c[0] = mix(cf(0.90), cf(0.55), sy); c[1] = mix(cf(0.92), cf(0.70), sy); c[2] = mix(cf(0.95), cf(1.00), sy);
    };
    const auto hash2 = [&](int s, crd::u32 bnc, int& u1, int& u2) {
        const int base = uadd(umul(s, cu(0x9E3779B9U)), cu(bnc * 0x2545F491U + 1U));
        u1 = detail::rt_hash01(g, uxor(umul(tid, cu(0x632BE5ABU)), base));
        u2 = detail::rt_hash01(g, uxor(umul(tid, cu(0x85157AF5U)), umul(base, cu(0xC2B2AE35U))));
    };

    g.stmt_buffer_store(out, uadd(b3, cu(0U)), cf(0.0)); // radiance accumulators ← 0
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), cf(0.0));

    const int floop = g.stmt_for_begin(cu(cfg.samples));
    const int s     = g.kernel_loop_var(floop);

    // the first scatter direction: a cosine sample from the SURFACE point (gather indirect light).
    int surfT[3];
    int surfB[3];
    frame(pnx, pny, pnz, surfT, surfB);
    int u1 = -1;
    int u2 = -1;
    hash2(s, 0U, u1, u2);
    int dir[3];
    cosine_dir(u1, u2, pnx, pny, pnz, surfT, surfB, dir);
    int ox = add(ppx, mul(cf(1.0e-3), pnx)), oy = add(ppy, mul(cf(1.0e-3), pny)), oz = add(ppz, mul(cf(1.0e-3), pnz));
    int dx = dir[0], dy = dir[1], dz = dir[2];
    int trR = cf(1.0), trG = cf(1.0), trB = cf(1.0); // throughput
    int radR = cf(0.0), radG = cf(0.0), radB = cf(0.0);

    for (crd::u32 bnc = 0; bnc < cfg.bounces; ++bnc)
    {
        const k::KGraph::RtHit hit = g.trace_ray_hit(as, ox, oy, oz, dx, dy, dz, cf(1.0e-3), cf(1.0e30));
        const int              miss = g.binary(k::KOp::CmpEq, hit.prim, cu(0xFFFFFFFFU));
        int                    sc[3];
        sky(dy, sc);
        radR = add(radR, g.select(miss, mul(trR, sc[0]), cf(0.0))); // escaped ⇒ gather sky
        radG = add(radG, g.select(miss, mul(trG, sc[1]), cf(0.0)));
        radB = add(radB, g.select(miss, mul(trB, sc[2]), cf(0.0)));
        // hit: fetch the hit triangle's flat normal + point, cosine-scatter, attenuate by albedo.
        const int cprim = mn(hit.prim, cu(cfg.ntri > 0U ? cfg.ntri - 1U : 0U));
        const int tb    = umul(cprim, cu(3U));
        const int hnx = lp(tn, tb, 0U), hny = lp(tn, tb, 1U), hnz = lp(tn, tb, 2U);
        const int hpx = add(ox, mul(hit.t, dx)), hpy = add(oy, mul(hit.t, dy)), hpz = add(oz, mul(hit.t, dz));
        trR = g.select(miss, cf(0.0), mul(trR, cf(static_cast<double>(cfg.albedo[0])))); // dead on miss, else ×albedo
        trG = g.select(miss, cf(0.0), mul(trG, cf(static_cast<double>(cfg.albedo[1]))));
        trB = g.select(miss, cf(0.0), mul(trB, cf(static_cast<double>(cfg.albedo[2]))));
        if (bnc + 1U < cfg.bounces) // resample the next bounce direction (skip after the last)
        {
            int ht[3];
            int hb[3];
            frame(hnx, hny, hnz, ht, hb);
            hash2(s, bnc + 1U, u1, u2);
            cosine_dir(u1, u2, hnx, hny, hnz, ht, hb, dir);
            ox = add(hpx, mul(cf(1.0e-3), hnx)); oy = add(hpy, mul(cf(1.0e-3), hny)); oz = add(hpz, mul(cf(1.0e-3), hnz));
            dx = dir[0]; dy = dir[1]; dz = dir[2];
        }
    }

    g.stmt_buffer_store(out, uadd(b3, cu(0U)), add(g.buffer_load(out, uadd(b3, cu(0U))), radR)); // accumulate this path
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), add(g.buffer_load(out, uadd(b3, cu(1U))), radG));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), add(g.buffer_load(out, uadd(b3, cu(2U))), radB));
    g.stmt_for_end(floop);

    const int inv = cf(1.0 / static_cast<double>(cfg.samples));
    g.stmt_buffer_store(out, uadd(b3, cu(0U)), mul(g.buffer_load(out, uadd(b3, cu(0U))), inv)); // mean radiance
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), mul(g.buffer_load(out, uadd(b3, cu(1U))), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), mul(g.buffer_load(out, uadd(b3, cu(2U))), inv));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// The light-sampling strategy the NEE path tracer emits. MIS is the shipping mode; Nee/Bsdf are the single-strategy references
// that let a test prove MIS is UNBIASED — all three must converge to the same mean (Veach's classic MIS validation).
enum class PtStrategy : crd::u32
{
    Mis  = 0U, // multiple importance sampling (power heuristic) — low variance, the gold-standard estimator
    Nee  = 1U, // light sampling only (next-event estimation) — great for small/bright lights, bad for near-specular
    Bsdf = 2U  // BSDF sampling only — great for large/dim lights + glossy, bad for small lights
};

struct PathTraceNeeConfig
{
    crd::u32 samples   = 64U;
    crd::u32 bounces   = 2U;                    // indirect bounce depth (unrolled); direct light is added at every vertex
    float    albedo[3] = {0.6F, 0.6F, 0.6F};    // uniform diffuse albedo of the scene surfaces
    // Rectangular AREA LIGHT, sampled analytically for NEE AND present in the AS (last `light_ntri` tris) so BSDF rays hit it.
    float    light_p0[3] = {-1.0F, 5.0F, -1.0F}; // a corner of the quad
    float    light_eu[3] = {2.0F, 0.0F, 0.0F};   // edge vector u  (Q = p0 + u1·eu + u2·ev, u1,u2∈[0,1))
    float    light_ev[3] = {0.0F, 0.0F, 2.0F};   // edge vector v
    float    light_nl[3] = {0.0F, -1.0F, 0.0F};  // emitting-face normal (unit)
    float    light_le[3] = {12.0F, 12.0F, 12.0F};// radiance emitted (W·sr⁻¹·m⁻²)
    crd::u32 ntri        = 4U;                    // total scene triangle count (clamps the primId flat-normal fetch)
    crd::u32 light_prim0 = 2U;                    // primId of the first light triangle (light tris = [prim0, prim0+ntri))
    crd::u32 light_ntri  = 2U;                    // number of light triangles (a quad = 2)
    PtStrategy strategy  = PtStrategy::Mis;
    crd::u32   local_size = 64U;
};

// NEE + MIS PATH TRACER — direct lighting done RIGHT. The diffuse megakernel gathered light only when a random bounce happened
// to escape to the sky; that is hopeless for a compact area light. This adds NEXT-EVENT ESTIMATION (sample a point on the light,
// cast a shadow ray, add its analytic contribution at EVERY path vertex) combined with the BSDF-sampled hit via MULTIPLE
// IMPORTANCE SAMPLING (Veach power heuristic) — the estimator that is low-variance for BOTH small-bright and large-dim lights and
// is the substrate ReSTIR DI resamples over. Emits one of three strategies (`cfg.strategy`) so a test can prove MIS is unbiased.
// Deterministic triple32 sampling (separate NEE / BSDF streams) ⇒ GPU==oracle to ULP. Buffers: TLAS (b0), positions (b1, 3f),
// normals (b2, 3f), per-triangle flat normals (b3, 3f each), radiance out (b4, 3f).
[[nodiscard]] inline crd::kir::KEntry build_pathtrace_nee_kernel(crd::kir::KGraph& g, const PathTraceNeeConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     dvv = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };
    const auto     mx  = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };
    const auto     mn  = [&](int a, int b) { return g.binary(k::KOp::Min, a, b); };
    const auto     neg = [&](int a) { return g.unary(k::KOp::Neg, a); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     uxor = [&](int a, int b) { return g.binary(k::KOp::BitXor, a, b); };
    const auto     gt0  = [&](int x) { return g.select(g.binary(k::KOp::CmpGt, x, cf(0.0)), cf(1.0), cf(0.0)); };
    const auto     dot3 = [&](int ax, int ay, int az, int bx, int by, int bz) { return add(add(mul(ax, bx), mul(ay, by)), mul(az, bz)); };
    const auto     pw2  = [&](int x) { return mul(x, x); };
    const auto     misw = [&](int pa, int pb) { return dvv(pw2(pa), mx(add(pw2(pa), pw2(pb)), cf(1.0e-20))); }; // power heuristic β=2

    constexpr double kPi    = 3.14159265358979323846;
    constexpr double kInvPi = 1.0 / kPi;
    const double p0x = cfg.light_p0[0], p0y = cfg.light_p0[1], p0z = cfg.light_p0[2];
    const double eux = cfg.light_eu[0], euy = cfg.light_eu[1], euz = cfg.light_eu[2];
    const double evx = cfg.light_ev[0], evy = cfg.light_ev[1], evz = cfg.light_ev[2];
    const double nlx = cfg.light_nl[0], nly = cfg.light_nl[1], nlz = cfg.light_nl[2];
    const double crx = euy * evz - euz * evy, cry = euz * evx - eux * evz, crz = eux * evy - euy * evx;
    const double area = crd::math::sqrt(crx * crx + cry * cry + crz * crz); // |eu × ev|
    const double aLe0 = static_cast<double>(cfg.albedo[0]) * static_cast<double>(cfg.light_le[0]) * kInvPi; // Lambert BRDF ×Le, folded
    const double aLe1 = static_cast<double>(cfg.albedo[1]) * static_cast<double>(cfg.light_le[1]) * kInvPi;
    const double aLe2 = static_cast<double>(cfg.albedo[2]) * static_cast<double>(cfg.light_le[2]) * kInvPi;

    const int as  = g.accel_struct_decl(0, 0);
    const int pos = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int tn  = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int out = g.buffer_decl(k::DType::F32, 0, 4, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = umul(tid, cu(3U));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, uadd(base, cu(c))); };
    const int  pnx = lp(nrm, b3, 0U), pny = lp(nrm, b3, 1U), pnz = lp(nrm, b3, 2U);
    const int  ppx = lp(pos, b3, 0U), ppy = lp(pos, b3, 1U), ppz = lp(pos, b3, 2U);

    const auto frame = [&](int nx, int ny, int nz, int t[3], int b[3]) {
        const int sg = g.select(g.binary(k::KOp::CmpGe, nz, cf(0.0)), cf(1.0), cf(-1.0));
        const int fa = dvv(cf(-1.0), add(sg, nz));
        const int fb = mul(mul(nx, ny), fa);
        t[0] = add(cf(1.0), mul(sg, mul(mul(nx, nx), fa))); t[1] = mul(sg, fb); t[2] = mul(neg(sg), nx);
        b[0] = fb; b[1] = add(sg, mul(mul(ny, ny), fa)); b[2] = neg(ny);
    };
    // cosine-weighted hemisphere direction (local z = N) → fills d[3]; also returns the sampled cosine (= pdf·π) for MIS.
    const auto cosine_dir = [&](int u1, int u2, int nx, int ny, int nz, const int t[3], const int b[3], int d[3]) {
        const int rr  = g.unary(k::KOp::Sqrt, u1);
        const int phi = mul(cf(2.0 * kPi), u2);
        const int lx  = mul(rr, g.unary(k::KOp::Cos, phi));
        const int ly  = mul(rr, g.unary(k::KOp::Sin, phi));
        const int lz  = g.unary(k::KOp::Sqrt, mx(sub(cf(1.0), u1), cf(0.0)));
        d[0] = add(add(mul(lx, t[0]), mul(ly, b[0])), mul(lz, nx));
        d[1] = add(add(mul(lx, t[1]), mul(ly, b[1])), mul(lz, ny));
        d[2] = add(add(mul(lx, t[2]), mul(ly, b[2])), mul(lz, nz));
        return lz; // cosθ of the sample w.r.t. N ⇒ pdf = lz/π
    };
    const auto hashsalt = [&](int s, crd::u32 salt, int& u1, int& u2) {
        const int base = uadd(umul(s, cu(0x9E3779B9U)), cu(salt * 0x2545F491U + 1U));
        u1 = detail::rt_hash01(g, uxor(umul(tid, cu(0x632BE5ABU)), base));
        u2 = detail::rt_hash01(g, uxor(umul(tid, cu(0x85157AF5U)), umul(base, cu(0xC2B2AE35U))));
    };

    g.stmt_buffer_store(out, uadd(b3, cu(0U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), cf(0.0));

    const int floop = g.stmt_for_begin(cu(cfg.samples));
    const int s     = g.kernel_loop_var(floop);
    int radR = cf(0.0), radG = cf(0.0), radB = cf(0.0);

    // NEXT-EVENT ESTIMATION at a shading vertex (Px,N,throughput) — sample the area light, shadow-ray it, add the MIS-weighted
    // direct contribution into radR/G/B. Called with throughput 0 for masked (miss/light) hits, so it self-nullifies.
    const auto nee = [&](int Px, int Py, int Pz, int Nx, int Ny, int Nz, int tR, int tG, int tB, crd::u32 salt) {
        int su1 = -1;
        int su2 = -1;
        hashsalt(s, salt, su1, su2);
        const int Qx = add(add(cf(p0x), mul(su1, cf(eux))), mul(su2, cf(evx))); // uniform point on the light quad
        const int Qy = add(add(cf(p0y), mul(su1, cf(euy))), mul(su2, cf(evy)));
        const int Qz = add(add(cf(p0z), mul(su1, cf(euz))), mul(su2, cf(evz)));
        const int dxl = sub(Qx, Px), dyl = sub(Qy, Py), dzl = sub(Qz, Pz);
        const int dist2 = mx(dot3(dxl, dyl, dzl, dxl, dyl, dzl), cf(1.0e-12));
        const int dist  = g.unary(k::KOp::Sqrt, dist2);
        const int invd  = dvv(cf(1.0), dist);
        const int wix = mul(dxl, invd), wiy = mul(dyl, invd), wiz = mul(dzl, invd);
        const int cos_s = dot3(Nx, Ny, Nz, wix, wiy, wiz);                 // surface-side cosine
        const int cos_l = neg(dot3(cf(nlx), cf(nly), cf(nlz), wix, wiy, wiz)); // light-side cosine (front-facing ⇒ >0)
        // shadow ray from P (offset off N) toward the light, stopping just short of it so the light itself is not an occluder.
        const int sox = add(Px, mul(cf(1.0e-3), Nx)), soy = add(Py, mul(cf(1.0e-3), Ny)), soz = add(Pz, mul(cf(1.0e-3), Nz));
        const int tmxs = mul(dist, cf(1.0 - 1.0e-3));
        const int st   = g.trace_ray_closest(as, sox, soy, soz, wix, wiy, wiz, cf(1.0e-3), tmxs);
        const int vis  = g.select(g.binary(k::KOp::CmpLt, st, tmxs), cf(0.0), cf(1.0));
        const int pdfL = dvv(dist2, mx(mul(cf(area), cos_l), cf(1.0e-8)));  // light pdf in solid angle
        const int pdfB = mul(cos_s, cf(kInvPi));                            // the cosine-BSDF pdf toward the light
        const int w    = (cfg.strategy == PtStrategy::Mis) ? misw(pdfL, pdfB) : cf(1.0);
        const int gate = mul(mul(gt0(cos_s), gt0(cos_l)), vis);            // both cosines positive AND unoccluded
        const int geom = dvv(mul(mul(cos_s, cos_l), cf(area)), dist2);      // cosθ_s·cosθ_l·A / d²  (light-area → solid-angle)
        const int sc   = mul(mul(w, geom), gate);
        radR = add(radR, mul(mul(tR, cf(aLe0)), sc));
        radG = add(radG, mul(mul(tG, cf(aLe1)), sc));
        radB = add(radB, mul(mul(tB, cf(aLe2)), sc));
    };

    // ── direct lighting at the primary (G-buffer) vertex ──
    if (cfg.strategy != PtStrategy::Bsdf) { nee(ppx, ppy, ppz, pnx, pny, pnz, cf(1.0), cf(1.0), cf(1.0), 1000U); }

    // ── scatter the primary ray (cosine BSDF) ──
    int surfT[3];
    int surfB[3];
    frame(pnx, pny, pnz, surfT, surfB);
    int u1 = -1;
    int u2 = -1;
    hashsalt(s, 0U, u1, u2);
    int dir[3];
    int pdfDir = mul(cosine_dir(u1, u2, pnx, pny, pnz, surfT, surfB, dir), cf(kInvPi)); // pdf of THIS ray (for BSDF-strategy MIS)
    int ox = add(ppx, mul(cf(1.0e-3), pnx)), oy = add(ppy, mul(cf(1.0e-3), pny)), oz = add(ppz, mul(cf(1.0e-3), pnz));
    int dx = dir[0], dy = dir[1], dz = dir[2];
    int trR = cf(static_cast<double>(cfg.albedo[0])), trG = cf(static_cast<double>(cfg.albedo[1])), trB = cf(static_cast<double>(cfg.albedo[2]));

    for (crd::u32 bnc = 0; bnc < cfg.bounces; ++bnc)
    {
        const k::KGraph::RtHit hit  = g.trace_ray_hit(as, ox, oy, oz, dx, dy, dz, cf(1.0e-3), cf(1.0e30));
        const int              miss = g.binary(k::KOp::CmpEq, hit.prim, cu(0xFFFFFFFFU));
        const int              tc   = mn(hit.t, cf(1.0e5)); // clamp for finite point reconstruction on a miss
        const int              nomiss = g.select(miss, cf(0.0), cf(1.0));
        const int              inrng  = mul(g.select(g.binary(k::KOp::CmpGe, hit.prim, cu(cfg.light_prim0)), cf(1.0), cf(0.0)),
                                            g.select(g.binary(k::KOp::CmpLt, hit.prim, cu(cfg.light_prim0 + cfg.light_ntri)), cf(1.0), cf(0.0)));
        const int              islight = mul(inrng, nomiss);
        const int              hitsurf = mul(nomiss, sub(cf(1.0), inrng)); // hit a non-emitter surface ⇒ continue the path

        // ── BSDF strategy: this ray landed on the light ⇒ add its (MIS-weighted) emission ──
        if (cfg.strategy != PtStrategy::Nee)
        {
            const int cosl = neg(dot3(cf(nlx), cf(nly), cf(nlz), dx, dy, dz));
            const int pdfL = dvv(mul(tc, tc), mx(mul(cf(area), cosl), cf(1.0e-8)));
            const int wb   = (cfg.strategy == PtStrategy::Mis) ? misw(pdfDir, pdfL) : cf(1.0);
            const int emit = mul(islight, gt0(cosl));
            radR = add(radR, mul(mul(mul(trR, cf(static_cast<double>(cfg.light_le[0]))), wb), emit));
            radG = add(radG, mul(mul(mul(trG, cf(static_cast<double>(cfg.light_le[1]))), wb), emit));
            radB = add(radB, mul(mul(mul(trB, cf(static_cast<double>(cfg.light_le[2]))), wb), emit));
        }

        // ── hit a diffuse surface: reconstruct the vertex, do NEE there, then scatter ──
        const int cprim = mn(hit.prim, cu(cfg.ntri > 0U ? cfg.ntri - 1U : 0U));
        const int tb    = umul(cprim, cu(3U));
        const int hnx = lp(tn, tb, 0U), hny = lp(tn, tb, 1U), hnz = lp(tn, tb, 2U);
        const int hpx = add(ox, mul(tc, dx)), hpy = add(oy, mul(tc, dy)), hpz = add(oz, mul(tc, dz));
        if (cfg.strategy != PtStrategy::Bsdf)
        {
            nee(hpx, hpy, hpz, hnx, hny, hnz, mul(trR, hitsurf), mul(trG, hitsurf), mul(trB, hitsurf), 1001U + bnc);
        }
        if (bnc + 1U < cfg.bounces)
        {
            int ht[3];
            int hb[3];
            frame(hnx, hny, hnz, ht, hb);
            hashsalt(s, bnc + 1U, u1, u2);
            pdfDir = mul(cosine_dir(u1, u2, hnx, hny, hnz, ht, hb, dir), cf(kInvPi));
            trR = mul(trR, mul(cf(static_cast<double>(cfg.albedo[0])), hitsurf)); // ×albedo, and die unless we hit a surface
            trG = mul(trG, mul(cf(static_cast<double>(cfg.albedo[1])), hitsurf));
            trB = mul(trB, mul(cf(static_cast<double>(cfg.albedo[2])), hitsurf));
            ox = add(hpx, mul(cf(1.0e-3), hnx)); oy = add(hpy, mul(cf(1.0e-3), hny)); oz = add(hpz, mul(cf(1.0e-3), hnz));
            dx = dir[0]; dy = dir[1]; dz = dir[2];
        }
    }

    g.stmt_buffer_store(out, uadd(b3, cu(0U)), add(g.buffer_load(out, uadd(b3, cu(0U))), radR));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), add(g.buffer_load(out, uadd(b3, cu(1U))), radG));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), add(g.buffer_load(out, uadd(b3, cu(2U))), radB));
    g.stmt_for_end(floop);

    const int inv = cf(1.0 / static_cast<double>(cfg.samples));
    g.stmt_buffer_store(out, uadd(b3, cu(0U)), mul(g.buffer_load(out, uadd(b3, cu(0U))), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), mul(g.buffer_load(out, uadd(b3, cu(1U))), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), mul(g.buffer_load(out, uadd(b3, cu(2U))), inv));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

struct RestirDiConfig
{
    crd::u32 frames     = 1U;   // independent ReSTIR estimates averaged (1 = the real-time 1-spp mode; high = validation vs ground truth)
    crd::u32 candidates = 16U;  // M — RIS candidate light samples streamed into each reservoir
    float    albedo[3]  = {0.6F, 0.6F, 0.6F};
    float    light_p0[3] = {-1.5F, 3.0F, -1.5F};
    float    light_eu[3] = {3.0F, 0.0F, 0.0F};
    float    light_ev[3] = {0.0F, 0.0F, 3.0F};
    float    light_nl[3] = {0.0F, -1.0F, 0.0F};
    float    light_le[3] = {8.0F, 8.0F, 8.0F};
    crd::u32 local_size  = 64U;
};

// ReSTIR DI — RESAMPLED IMPORTANCE SAMPLING with a per-pixel reservoir (Bitterli et al. 2020), the frontier real-time
// many-light estimator. NEE draws ONE light sample per pixel from a cheap source pdf; ReSTIR streams M candidates into a
// WEIGHTED-RESERVOIR-SAMPLING reservoir whose target p̂ = the UNSHADOWED contribution (f·Le·G), keeps the single best survivor,
// and pays for exactly ONE shadow ray (visibility reuse). The RIS estimator L = f(y)·Le·G(y)·V(y)·W with W = Σwᵢ/(M·p̂(y)),
// wᵢ = p̂(xᵢ)/p(xᵢ) — provably unbiased, and importance-sampling the good samples slashes variance. The reservoir is threaded as
// SSA through the UNROLLED candidate loop (CKIR `For` carries no registers); `frames` are averaged in an outer runtime `For`.
// This is the spatial-less core (RIS + WRS + visibility reuse); temporal/spatial reservoir reuse layers on top next. Buffers:
// TLAS (b0), positions (b1, 3f), normals (b2, 3f), radiance out (b3, 3f).
[[nodiscard]] inline crd::kir::KEntry build_restir_di_kernel(crd::kir::KGraph& g, const RestirDiConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     dvv = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };
    const auto     mx  = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };
    const auto     neg = [&](int a) { return g.unary(k::KOp::Neg, a); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     uxor = [&](int a, int b) { return g.binary(k::KOp::BitXor, a, b); };
    const auto     dot3 = [&](int ax, int ay, int az, int bx, int by, int bz) { return add(add(mul(ax, bx), mul(ay, by)), mul(az, bz)); };
    const auto     clamp0 = [&](int x) { return mx(x, cf(0.0)); };

    constexpr double kInvPi = 1.0 / 3.14159265358979323846;
    const double p0x = cfg.light_p0[0], p0y = cfg.light_p0[1], p0z = cfg.light_p0[2];
    const double eux = cfg.light_eu[0], euy = cfg.light_eu[1], euz = cfg.light_eu[2];
    const double evx = cfg.light_ev[0], evy = cfg.light_ev[1], evz = cfg.light_ev[2];
    const double nlx = cfg.light_nl[0], nly = cfg.light_nl[1], nlz = cfg.light_nl[2];
    const double crx = euy * evz - euz * evy, cry = euz * evx - eux * evz, crz = eux * evy - euy * evx;
    const double area = crd::math::sqrt(crx * crx + cry * cry + crz * crz);
    const double a0Le0 = static_cast<double>(cfg.albedo[0]) * static_cast<double>(cfg.light_le[0]) * kInvPi; // per-channel integrand coeff
    const double a1Le1 = static_cast<double>(cfg.albedo[1]) * static_cast<double>(cfg.light_le[1]) * kInvPi;
    const double a2Le2 = static_cast<double>(cfg.albedo[2]) * static_cast<double>(cfg.light_le[2]) * kInvPi;
    const double clum  = 0.2126 * a0Le0 + 0.7152 * a1Le1 + 0.0722 * a2Le2;                                   // luminance coeff for p̂

    const int as  = g.accel_struct_decl(0, 0);
    const int pos = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int out = g.buffer_decl(k::DType::F32, 0, 3, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = umul(tid, cu(3U));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, uadd(base, cu(c))); };
    const int  Nx = lp(nrm, b3, 0U), Ny = lp(nrm, b3, 1U), Nz = lp(nrm, b3, 2U);
    const int  Px = lp(pos, b3, 0U), Py = lp(pos, b3, 1U), Pz = lp(pos, b3, 2U);

    g.stmt_buffer_store(out, uadd(b3, cu(0U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), cf(0.0));

    const int floop = g.stmt_for_begin(cu(cfg.frames));
    const int fr    = g.kernel_loop_var(floop);

    // ── RIS reservoir over M candidate light samples (streaming WRS, reservoir threaded as SSA) ──
    int wsum = cf(0.0);
    int cQx  = cf(p0x), cQy = cf(p0y), cQz = cf(p0z); // chosen sample point on the light
    int cPh  = cf(0.0);                               // chosen sample's target p̂
    for (crd::u32 i = 0; i < cfg.candidates; ++i)
    {
        const int seed = uadd(umul(fr, cu(0x9E3779B9U)), cu(i * 0x2545F491U + 1U));
        const int u1   = detail::rt_hash01(g, uxor(umul(tid, cu(0x632BE5ABU)), seed));
        const int u2   = detail::rt_hash01(g, uxor(umul(tid, cu(0x85157AF5U)), umul(seed, cu(0xC2B2AE35U))));
        const int Qx = add(add(cf(p0x), mul(u1, cf(eux))), mul(u2, cf(evx)));
        const int Qy = add(add(cf(p0y), mul(u1, cf(euy))), mul(u2, cf(evy)));
        const int Qz = add(add(cf(p0z), mul(u1, cf(euz))), mul(u2, cf(evz)));
        const int dxl = sub(Qx, Px), dyl = sub(Qy, Py), dzl = sub(Qz, Pz);
        const int d2  = mx(dot3(dxl, dyl, dzl, dxl, dyl, dzl), cf(1.0e-12));
        const int invd = dvv(cf(1.0), g.unary(k::KOp::Sqrt, d2));
        const int cs  = clamp0(mul(dot3(Nx, Ny, Nz, dxl, dyl, dzl), invd));           // cosθ_s (wi = d/|d|)
        const int cl  = clamp0(neg(mul(dot3(cf(nlx), cf(nly), cf(nlz), dxl, dyl, dzl), invd))); // cosθ_l
        const int G   = dvv(mul(cs, cl), d2);                                          // area-measure geometry
        const int ph  = mul(cf(clum), G);                                              // target p̂ = lum(f·Le)·G
        const int wi  = mul(ph, cf(area));                                             // RIS weight = p̂ / (1/area)
        wsum = add(wsum, wi);
        const int xi   = detail::rt_hash01(g, uxor(umul(tid, cu(0x27D4EB2FU)), umul(seed, cu(0x165667B1U))));
        const int repl = g.select(g.binary(k::KOp::CmpLt, mul(xi, wsum), wi), cf(1.0), cf(0.0)); // xi < wi/wsum
        cQx = add(mul(repl, Qx), mul(sub(cf(1.0), repl), cQx));                         // WRS replace (mask-blend, all finite)
        cQy = add(mul(repl, Qy), mul(sub(cf(1.0), repl), cQy));
        cQz = add(mul(repl, Qz), mul(sub(cf(1.0), repl), cQz));
        cPh = add(mul(repl, ph), mul(sub(cf(1.0), repl), cPh));
    }

    // ── survivor: W, one visibility ray, shade ──
    const int W = dvv(wsum, mul(cf(static_cast<double>(cfg.candidates)), mx(cPh, cf(1.0e-12)))); // unbiased contribution weight
    const int dcx = sub(cQx, Px), dcy = sub(cQy, Py), dcz = sub(cQz, Pz);
    const int dc2 = mx(dot3(dcx, dcy, dcz, dcx, dcy, dcz), cf(1.0e-12));
    const int dist = g.unary(k::KOp::Sqrt, dc2);
    const int invc = dvv(cf(1.0), dist);
    const int wix = mul(dcx, invc), wiy = mul(dcy, invc), wiz = mul(dcz, invc);
    const int cs2 = clamp0(dot3(Nx, Ny, Nz, wix, wiy, wiz));
    const int cl2 = clamp0(neg(dot3(cf(nlx), cf(nly), cf(nlz), wix, wiy, wiz)));
    const int Gc  = dvv(mul(cs2, cl2), dc2);
    const int sox = add(Px, mul(cf(1.0e-3), Nx)), soy = add(Py, mul(cf(1.0e-3), Ny)), soz = add(Pz, mul(cf(1.0e-3), Nz));
    const int tmxs = mul(dist, cf(1.0 - 1.0e-3));
    const int st   = g.trace_ray_closest(as, sox, soy, soz, wix, wiy, wiz, cf(1.0e-3), tmxs);
    const int V    = g.select(g.binary(k::KOp::CmpLt, st, tmxs), cf(0.0), cf(1.0)); // occluded ⇒ 0
    const int shade = mul(mul(Gc, V), W);                                           // integrand-geometry × visibility × RIS weight
    g.stmt_buffer_store(out, uadd(b3, cu(0U)), add(g.buffer_load(out, uadd(b3, cu(0U))), mul(cf(a0Le0), shade)));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), add(g.buffer_load(out, uadd(b3, cu(1U))), mul(cf(a1Le1), shade)));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), add(g.buffer_load(out, uadd(b3, cu(2U))), mul(cf(a2Le2), shade)));
    g.stmt_for_end(floop);

    const int inv = cf(1.0 / static_cast<double>(cfg.frames));
    g.stmt_buffer_store(out, uadd(b3, cu(0U)), mul(g.buffer_load(out, uadd(b3, cu(0U))), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), mul(g.buffer_load(out, uadd(b3, cu(1U))), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), mul(g.buffer_load(out, uadd(b3, cu(2U))), inv));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

struct ManyLightConfig
{
    crd::u32 samples    = 64U; // light samples per pixel
    crd::u32 nlights    = 4U;  // area lights (their geometry lives in a runtime buffer, NOT baked constants)
    float    albedo[3]  = {0.6F, 0.6F, 0.6F};
    bool     power_sampling = false; // POWER-proportional light selection (CDF over luminance(Le)·area) vs uniform ⌊u·N⌋
    crd::u32 local_size = 64U;
};

// MANY-LIGHTS direct lighting — the integrator-breadth capability RIS/ReSTIR exist for. Instead of one baked area light, the N
// lights live in a runtime buffer (15 floats each: p0.xyz, eu.xyz, ev.xyz, nl.xyz, Le.xyz), so a scene can carry hundreds. Per
// sample: pick a light UNIFORMLY (l = ⌊u·N⌋), sample a point on it, shadow-ray it, add f·Le·G·V / pdf with pdf = (1/N)·(1/areaₗ)
// in area measure — provably unbiased (its mean is Σₗ ∫ f·Leₗ·V·Gₗ, the sum over all lights). This is the substrate a light-BVH
// / power sampling and multi-light ReSTIR resample over. Buffers: TLAS (b0), positions (b1, 3f), normals (b2, 3f), lights
// (b3, 15f each), radiance out (b4, 3f).
[[nodiscard]] inline crd::kir::KEntry build_manylight_nee_kernel(crd::kir::KGraph& g, const ManyLightConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     dvv = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };
    const auto     mx  = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };
    const auto     mn  = [&](int a, int b) { return g.binary(k::KOp::Min, a, b); };
    const auto     neg = [&](int a) { return g.unary(k::KOp::Neg, a); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     uxor = [&](int a, int b) { return g.binary(k::KOp::BitXor, a, b); };
    const auto     clamp0 = [&](int x) { return mx(x, cf(0.0)); };
    const auto     dot3 = [&](int ax, int ay, int az, int bx, int by, int bz) { return add(add(mul(ax, bx), mul(ay, by)), mul(az, bz)); };

    constexpr double kInvPi = 1.0 / 3.14159265358979323846;
    const double a0 = static_cast<double>(cfg.albedo[0]) * kInvPi, a1 = static_cast<double>(cfg.albedo[1]) * kInvPi, a2 = static_cast<double>(cfg.albedo[2]) * kInvPi;

    const int as   = g.accel_struct_decl(0, 0);
    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int lts  = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int out  = g.buffer_decl(k::DType::F32, 0, 4, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = umul(tid, cu(3U));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, uadd(base, cu(c))); };
    const int  Nx = lp(nrm, b3, 0U), Ny = lp(nrm, b3, 1U), Nz = lp(nrm, b3, 2U);
    const int  Px = lp(pos, b3, 0U), Py = lp(pos, b3, 1U), Pz = lp(pos, b3, 2U);

    g.stmt_buffer_store(out, uadd(b3, cu(0U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), cf(0.0));

    const int floop = g.stmt_for_begin(cu(cfg.samples));
    const int s     = g.kernel_loop_var(floop);
    const int seed  = uadd(umul(s, cu(0x9E3779B9U)), cu(1U));
    const int ul  = detail::rt_hash01(g, uxor(umul(tid, cu(0x2C1B3C6DU)), seed));
    // per-light POWER = luminance(Le)·area — the importance weight for power sampling.
    const auto light_power = [&](crd::u32 i) {
        const int base = cu(i * 15U);
        const int le0 = lp(lts, base, 12U), le1 = lp(lts, base, 13U), le2 = lp(lts, base, 14U);
        const int eu0 = lp(lts, base, 3U), eu1 = lp(lts, base, 4U), eu2 = lp(lts, base, 5U);
        const int ev0 = lp(lts, base, 6U), ev1 = lp(lts, base, 7U), ev2 = lp(lts, base, 8U);
        const int cx = sub(mul(eu1, ev2), mul(eu2, ev1)), cy = sub(mul(eu2, ev0), mul(eu0, ev2)), cz = sub(mul(eu0, ev1), mul(eu1, ev0));
        const int ar = g.unary(k::KOp::Sqrt, mx(dot3(cx, cy, cz, cx, cy, cz), cf(1.0e-20)));
        return mul(add(add(mul(cf(0.2126), le0), mul(cf(0.7152), le1)), mul(cf(0.0722), le2)), ar);
    };
    int total = cf(1.0); // sum of light powers (only meaningful when power_sampling)
    int li    = cu(0U);
    if (cfg.power_sampling)
    {
        total = cf(0.0);
        for (crd::u32 i = 0; i < cfg.nlights; ++i) { total = add(total, light_power(i)); }
        const int u = mul(ul, total); // scan the CDF: pick the first light whose cumulative power crosses u
        int acc = cf(0.0), selF = cf(0.0), found = cf(0.0);
        for (crd::u32 i = 0; i < cfg.nlights; ++i)
        {
            const int nacc = add(acc, light_power(i));
            const int cond = mul(g.select(g.binary(k::KOp::CmpLt, u, nacc), cf(1.0), cf(0.0)), sub(cf(1.0), found));
            selF  = add(mul(cond, cf(static_cast<double>(i))), mul(sub(cf(1.0), cond), selF));
            found = mx(found, cond);
            acc   = nacc;
        }
        li = mn(g.cast(selF, k::DType::U32), cu(cfg.nlights > 0U ? cfg.nlights - 1U : 0U));
    }
    else // uniform: l = ⌊u·N⌋
    {
        li = mn(g.cast(g.unary(k::KOp::Floor, mul(ul, cf(static_cast<double>(cfg.nlights)))), k::DType::U32), cu(cfg.nlights > 0U ? cfg.nlights - 1U : 0U));
    }
    const int lb  = umul(li, cu(15U));
    const int p0x = lp(lts, lb, 0U), p0y = lp(lts, lb, 1U), p0z = lp(lts, lb, 2U);
    const int eux = lp(lts, lb, 3U), euy = lp(lts, lb, 4U), euz = lp(lts, lb, 5U);
    const int evx = lp(lts, lb, 6U), evy = lp(lts, lb, 7U), evz = lp(lts, lb, 8U);
    const int nlx = lp(lts, lb, 9U), nly = lp(lts, lb, 10U), nlz = lp(lts, lb, 11U);
    const int lex = lp(lts, lb, 12U), ley = lp(lts, lb, 13U), lez = lp(lts, lb, 14U);
    // area = |eu × ev|.
    const int crx = sub(mul(euy, evz), mul(euz, evy)), cry = sub(mul(euz, evx), mul(eux, evz)), crz = sub(mul(eux, evy), mul(euy, evx));
    const int area = g.unary(k::KOp::Sqrt, mx(dot3(crx, cry, crz, crx, cry, crz), cf(1.0e-20)));
    // sample a point on the light.
    const int u1 = detail::rt_hash01(g, uxor(umul(tid, cu(0x632BE5ABU)), umul(seed, cu(0x9E3779B1U))));
    const int u2 = detail::rt_hash01(g, uxor(umul(tid, cu(0x85157AF5U)), umul(seed, cu(0xC2B2AE35U))));
    const int Qx = add(add(p0x, mul(u1, eux)), mul(u2, evx));
    const int Qy = add(add(p0y, mul(u1, euy)), mul(u2, evy));
    const int Qz = add(add(p0z, mul(u1, euz)), mul(u2, evz));
    const int dx = sub(Qx, Px), dy = sub(Qy, Py), dz = sub(Qz, Pz);
    const int d2 = mx(dot3(dx, dy, dz, dx, dy, dz), cf(1.0e-12));
    const int dist = g.unary(k::KOp::Sqrt, d2);
    const int invd = dvv(cf(1.0), dist);
    const int wix = mul(dx, invd), wiy = mul(dy, invd), wiz = mul(dz, invd);
    const int cs = clamp0(dot3(Nx, Ny, Nz, wix, wiy, wiz));
    const int cl = clamp0(neg(dot3(nlx, nly, nlz, wix, wiy, wiz)));
    // shadow ray (offset off N, stop just short of the light).
    const int sox = add(Px, mul(cf(1.0e-3), Nx)), soy = add(Py, mul(cf(1.0e-3), Ny)), soz = add(Pz, mul(cf(1.0e-3), Nz));
    const int tmxs = mul(dist, cf(1.0 - 1.0e-3));
    const int st   = g.trace_ray_closest(as, sox, soy, soz, wix, wiy, wiz, cf(1.0e-3), tmxs);
    const int V    = g.select(g.binary(k::KOp::CmpLt, st, tmxs), cf(0.0), cf(1.0));
    // estimator: f·Le·G·V / pdf, G = cos_s·cos_l/d². UNIFORM pdf=(1/N)(1/area) ⇒ ×N·area; POWER pdf=(power_l/total)(1/area) ⇒
    // ×total·area/power_l = ×total/lum(Le_l) (the area cancels).
    const int lumSel = add(add(mul(cf(0.2126), lex), mul(cf(0.7152), ley)), mul(cf(0.0722), lez));
    const int wscale = cfg.power_sampling ? dvv(total, mx(lumSel, cf(1.0e-8))) : mul(cf(static_cast<double>(cfg.nlights)), area);
    const int wgt = mul(mul(dvv(mul(cs, cl), d2), V), wscale);
    g.stmt_buffer_store(out, uadd(b3, cu(0U)), add(lp(out, b3, 0U), mul(mul(cf(a0), lex), wgt)));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), add(lp(out, b3, 1U), mul(mul(cf(a1), ley), wgt)));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), add(lp(out, b3, 2U), mul(mul(cf(a2), lez), wgt)));
    g.stmt_for_end(floop);

    const int inv = cf(1.0 / static_cast<double>(cfg.samples));
    g.stmt_buffer_store(out, uadd(b3, cu(0U)), mul(lp(out, b3, 0U), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), mul(lp(out, b3, 1U), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), mul(lp(out, b3, 2U), inv));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

struct PathTraceFullConfig
{
    crd::u32 samples   = 32U;
    crd::u32 bounces   = 4U;   // max path depth (unrolled)
    crd::u32 rr_start  = 2U;   // Russian roulette begins at this bounce (unbiased path termination)
    float    albedo[3] = {0.6F, 0.6F, 0.6F};
    crd::u32 ntri       = 6U;  // total scene triangles (clamps the primId flat-normal fetch)
    crd::u32 nlights    = 2U;  // area lights (their quads are the LAST 2·nlights triangles; params in the light buffer)
    crd::u32 light_prim0 = 2U; // first light-triangle primId; lights occupy [prim0, prim0 + 2·nlights), 2 tris each
    crd::u32 local_size = 64U;
};

// FULL PRODUCTION PATH TRACER — the culmination of integrator breadth: MANY-LIGHTS next-event estimation with MULTIPLE
// IMPORTANCE SAMPLING at every path vertex, EMISSIVE-triangle hits (a BSDF ray that lands on a light adds its Le, MIS-weighted),
// RUSSIAN-ROULETTE termination past `rr_start` (unbiased — survivors are boosted by 1/p), and multi-bounce diffuse GLOBAL
// ILLUMINATION. Lights live in a runtime buffer (15f each); the light index of an emissive hit comes from its primId. Buffers:
// TLAS (b0), positions (b1, 3f), normals (b2, 3f), per-triangle flat normals (b3, 3f each), lights (b4, 15f each), radiance out
// (b5, 3f). Deterministic triple32 sampling ⇒ GPU==oracle; RR keeps the mean identical to the no-RR estimator (verified unbiased).
[[nodiscard]] inline crd::kir::KEntry build_pathtrace_full_kernel(crd::kir::KGraph& g, const PathTraceFullConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     dvv = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };
    const auto     mx  = [&](int a, int b) { return g.binary(k::KOp::Max, a, b); };
    const auto     mn  = [&](int a, int b) { return g.binary(k::KOp::Min, a, b); };
    const auto     neg = [&](int a) { return g.unary(k::KOp::Neg, a); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     uxor = [&](int a, int b) { return g.binary(k::KOp::BitXor, a, b); };
    const auto     gt0  = [&](int x) { return g.select(g.binary(k::KOp::CmpGt, x, cf(0.0)), cf(1.0), cf(0.0)); };
    const auto     clamp0 = [&](int x) { return mx(x, cf(0.0)); };
    const auto     dot3 = [&](int ax, int ay, int az, int bx, int by, int bz) { return add(add(mul(ax, bx), mul(ay, by)), mul(az, bz)); };
    const auto     pw2  = [&](int x) { return mul(x, x); };
    const auto     misw = [&](int pa, int pb) { return dvv(pw2(pa), mx(add(pw2(pa), pw2(pb)), cf(1.0e-20))); };

    constexpr double kPi    = 3.14159265358979323846;
    constexpr double kInvPi = 1.0 / kPi;
    const double invN = 1.0 / static_cast<double>(cfg.nlights > 0U ? cfg.nlights : 1U);
    const double a0 = static_cast<double>(cfg.albedo[0]), a1 = static_cast<double>(cfg.albedo[1]), a2 = static_cast<double>(cfg.albedo[2]);

    const int as   = g.accel_struct_decl(0, 0);
    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int tn   = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int lts  = g.buffer_decl(k::DType::F32, 0, 4, false);
    const int out  = g.buffer_decl(k::DType::F32, 0, 5, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = umul(tid, cu(3U));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, uadd(base, cu(c))); };
    const int  pnx = lp(nrm, b3, 0U), pny = lp(nrm, b3, 1U), pnz = lp(nrm, b3, 2U);
    const int  ppx = lp(pos, b3, 0U), ppy = lp(pos, b3, 1U), ppz = lp(pos, b3, 2U);

    const auto frame = [&](int nx, int ny, int nz, int t[3], int b[3]) {
        const int sg = g.select(g.binary(k::KOp::CmpGe, nz, cf(0.0)), cf(1.0), cf(-1.0));
        const int fa = dvv(cf(-1.0), add(sg, nz));
        const int fb = mul(mul(nx, ny), fa);
        t[0] = add(cf(1.0), mul(sg, mul(mul(nx, nx), fa))); t[1] = mul(sg, fb); t[2] = mul(neg(sg), nx);
        b[0] = fb; b[1] = add(sg, mul(mul(ny, ny), fa)); b[2] = neg(ny);
    };
    const auto cosine_dir = [&](int u1, int u2, int nx, int ny, int nz, const int t[3], const int b[3], int d[3]) {
        const int rr  = g.unary(k::KOp::Sqrt, u1);
        const int phi = mul(cf(2.0 * kPi), u2);
        const int lx  = mul(rr, g.unary(k::KOp::Cos, phi));
        const int ly  = mul(rr, g.unary(k::KOp::Sin, phi));
        const int lz  = g.unary(k::KOp::Sqrt, mx(sub(cf(1.0), u1), cf(0.0)));
        d[0] = add(add(mul(lx, t[0]), mul(ly, b[0])), mul(lz, nx));
        d[1] = add(add(mul(lx, t[1]), mul(ly, b[1])), mul(lz, ny));
        d[2] = add(add(mul(lx, t[2]), mul(ly, b[2])), mul(lz, nz));
        return lz;
    };
    const auto hashsalt = [&](int s, crd::u32 salt, int& u1, int& u2) {
        const int base = uadd(umul(s, cu(0x9E3779B9U)), cu(salt * 0x2545F491U + 1U));
        u1 = detail::rt_hash01(g, uxor(umul(tid, cu(0x632BE5ABU)), base));
        u2 = detail::rt_hash01(g, uxor(umul(tid, cu(0x85157AF5U)), umul(base, cu(0xC2B2AE35U))));
    };
    const auto hash1 = [&](int s, crd::u32 salt) {
        return detail::rt_hash01(g, uxor(umul(tid, cu(0x27D4EB2FU)), uadd(umul(s, cu(0x9E3779B1U)), cu(salt * 0x85EBCA77U + 3U))));
    };

    // radiance accumulators reused across samples via out-buffer RMW.
    g.stmt_buffer_store(out, uadd(b3, cu(0U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), cf(0.0));

    const int floop = g.stmt_for_begin(cu(cfg.samples));
    const int s     = g.kernel_loop_var(floop);
    int radR = cf(0.0), radG = cf(0.0), radB = cf(0.0);

    // MANY-LIGHTS NEE with MIS at a shading vertex — pick a light uniformly, sample it, shadow-ray it, add the MIS-weighted direct.
    const auto nee = [&](int Px, int Py, int Pz, int Nx, int Ny, int Nz, int tR, int tG, int tB, crd::u32 salt) {
        const int ul = hash1(s, salt);
        const int li = mn(g.cast(g.unary(k::KOp::Floor, mul(ul, cf(static_cast<double>(cfg.nlights)))), k::DType::U32), cu(cfg.nlights > 0U ? cfg.nlights - 1U : 0U));
        const int lb = umul(li, cu(15U));
        const int p0x = lp(lts, lb, 0U), p0y = lp(lts, lb, 1U), p0z = lp(lts, lb, 2U);
        const int eux = lp(lts, lb, 3U), euy = lp(lts, lb, 4U), euz = lp(lts, lb, 5U);
        const int evx = lp(lts, lb, 6U), evy = lp(lts, lb, 7U), evz = lp(lts, lb, 8U);
        const int nlx = lp(lts, lb, 9U), nly = lp(lts, lb, 10U), nlz = lp(lts, lb, 11U);
        const int lex = lp(lts, lb, 12U), ley = lp(lts, lb, 13U), lez = lp(lts, lb, 14U);
        const int crx = sub(mul(euy, evz), mul(euz, evy)), cry = sub(mul(euz, evx), mul(eux, evz)), crz = sub(mul(eux, evy), mul(euy, evx));
        const int area = g.unary(k::KOp::Sqrt, mx(dot3(crx, cry, crz, crx, cry, crz), cf(1.0e-20)));
        int su1 = -1, su2 = -1; hashsalt(s, salt + 777U, su1, su2);
        const int Qx = add(add(p0x, mul(su1, eux)), mul(su2, evx));
        const int Qy = add(add(p0y, mul(su1, euy)), mul(su2, evy));
        const int Qz = add(add(p0z, mul(su1, euz)), mul(su2, evz));
        const int dx = sub(Qx, Px), dy = sub(Qy, Py), dz = sub(Qz, Pz);
        const int d2 = mx(dot3(dx, dy, dz, dx, dy, dz), cf(1.0e-12));
        const int dist = g.unary(k::KOp::Sqrt, d2);
        const int invd = dvv(cf(1.0), dist);
        const int wix = mul(dx, invd), wiy = mul(dy, invd), wiz = mul(dz, invd);
        const int cs = dot3(Nx, Ny, Nz, wix, wiy, wiz);
        const int cl = neg(dot3(nlx, nly, nlz, wix, wiy, wiz));
        const int sox = add(Px, mul(cf(1.0e-3), Nx)), soy = add(Py, mul(cf(1.0e-3), Ny)), soz = add(Pz, mul(cf(1.0e-3), Nz));
        const int tmxs = mul(dist, cf(1.0 - 1.0e-3));
        const int st   = g.trace_ray_closest(as, sox, soy, soz, wix, wiy, wiz, cf(1.0e-3), tmxs);
        const int vis  = g.select(g.binary(k::KOp::CmpLt, st, tmxs), cf(0.0), cf(1.0));
        const int pdfL = mul(cf(invN), dvv(d2, mx(mul(area, cl), cf(1.0e-8)))); // (1/N)·d²/(area·cosθ_l)
        const int pdfB = mul(cs, cf(kInvPi));
        const int w    = misw(pdfL, pdfB);
        const int gate = mul(mul(gt0(cs), gt0(cl)), vis);
        const int sc   = mul(mul(mul(w, dvv(mul(cs, cl), d2)), mul(area, cf(static_cast<double>(cfg.nlights)))), mul(gate, cf(kInvPi)));
        radR = add(radR, mul(mul(mul(tR, cf(a0)), lex), sc));
        radG = add(radG, mul(mul(mul(tG, cf(a1)), ley), sc));
        radB = add(radB, mul(mul(mul(tB, cf(a2)), lez), sc));
    };

    // ── direct lighting at the primary vertex + set up the primary BSDF ray ──
    nee(ppx, ppy, ppz, pnx, pny, pnz, cf(1.0), cf(1.0), cf(1.0), 100U);
    int surfT[3], surfB[3];
    frame(pnx, pny, pnz, surfT, surfB);
    int u1 = -1, u2 = -1; hashsalt(s, 0U, u1, u2);
    int dir[3];
    int pdfDir = mul(cosine_dir(u1, u2, pnx, pny, pnz, surfT, surfB, dir), cf(kInvPi));
    int ox = add(ppx, mul(cf(1.0e-3), pnx)), oy = add(ppy, mul(cf(1.0e-3), pny)), oz = add(ppz, mul(cf(1.0e-3), pnz));
    int dx = dir[0], dy = dir[1], dz = dir[2];
    int trR = cf(a0), trG = cf(a1), trB = cf(a2);

    for (crd::u32 bnc = 0; bnc < cfg.bounces; ++bnc)
    {
        const k::KGraph::RtHit hit  = g.trace_ray_hit(as, ox, oy, oz, dx, dy, dz, cf(1.0e-3), cf(1.0e30));
        const int              miss = g.binary(k::KOp::CmpEq, hit.prim, cu(0xFFFFFFFFU));
        const int              tc   = mn(hit.t, cf(1.0e5));
        const int              nomiss = g.select(miss, cf(0.0), cf(1.0));
        const int              inrng  = mul(g.select(g.binary(k::KOp::CmpGe, hit.prim, cu(cfg.light_prim0)), cf(1.0), cf(0.0)),
                                            g.select(g.binary(k::KOp::CmpLt, hit.prim, cu(cfg.light_prim0 + 2U * cfg.nlights)), cf(1.0), cf(0.0)));
        const int              islight = mul(inrng, nomiss);
        const int              hitsurf = mul(nomiss, sub(cf(1.0), inrng));

        // ── EMISSIVE HIT (MIS): the BSDF ray landed on light lidx ⇒ add its Le weighted against the light-sampling pdf ──
        const int lidx = g.binary(k::KOp::Div, sub(mn(hit.prim, cu(cfg.light_prim0 + 2U * cfg.nlights - 1U)), cu(cfg.light_prim0)), cu(2U));
        const int elb  = umul(lidx, cu(15U));
        const int elex = lp(lts, elb, 12U), eley = lp(lts, elb, 13U), elez = lp(lts, elb, 14U);
        const int enlx = lp(lts, elb, 9U), enly = lp(lts, elb, 10U), enlz = lp(lts, elb, 11U);
        const int eeux = lp(lts, elb, 3U), eeuy = lp(lts, elb, 4U), eeuz = lp(lts, elb, 5U);
        const int eevx = lp(lts, elb, 6U), eevy = lp(lts, elb, 7U), eevz = lp(lts, elb, 8U);
        const int ecrx = sub(mul(eeuy, eevz), mul(eeuz, eevy)), ecry = sub(mul(eeuz, eevx), mul(eeux, eevz)), ecrz = sub(mul(eeux, eevy), mul(eeuy, eevx));
        const int earea = g.unary(k::KOp::Sqrt, mx(dot3(ecrx, ecry, ecrz, ecrx, ecry, ecrz), cf(1.0e-20)));
        const int ecl   = neg(dot3(enlx, enly, enlz, dx, dy, dz)); // cosθ_l along the ray
        const int epdfL = mul(cf(invN), dvv(mul(tc, tc), mx(mul(earea, ecl), cf(1.0e-8))));
        const int ewb   = misw(pdfDir, epdfL);
        const int eemit = mul(islight, gt0(ecl));
        radR = add(radR, mul(mul(mul(trR, elex), ewb), eemit));
        radG = add(radG, mul(mul(mul(trG, eley), ewb), eemit));
        radB = add(radB, mul(mul(mul(trB, elez), ewb), eemit));

        // ── hit a diffuse surface: NEE there, then scatter (with Russian roulette past rr_start) ──
        const int cprim = mn(hit.prim, cu(cfg.ntri > 0U ? cfg.ntri - 1U : 0U));
        const int tb    = umul(cprim, cu(3U));
        const int hnx = lp(tn, tb, 0U), hny = lp(tn, tb, 1U), hnz = lp(tn, tb, 2U);
        const int hpx = add(ox, mul(tc, dx)), hpy = add(oy, mul(tc, dy)), hpz = add(oz, mul(tc, dz));
        nee(hpx, hpy, hpz, hnx, hny, hnz, mul(trR, hitsurf), mul(trG, hitsurf), mul(trB, hitsurf), 200U + bnc);

        if (bnc + 1U < cfg.bounces)
        {
            int ht[3], hb[3];
            frame(hnx, hny, hnz, ht, hb);
            hashsalt(s, bnc + 1U, u1, u2);
            pdfDir = mul(cosine_dir(u1, u2, hnx, hny, hnz, ht, hb, dir), cf(kInvPi));
            trR = mul(trR, mul(cf(a0), hitsurf)); // ×albedo, die unless we hit a surface
            trG = mul(trG, mul(cf(a1), hitsurf));
            trB = mul(trB, mul(cf(a2), hitsurf));
            if (bnc + 1U >= cfg.rr_start) // RUSSIAN ROULETTE: survive with prob p=clamp(max throughput,·); survivors ÷p (unbiased)
            {
                const int plum = mn(mx(mx(mx(trR, trG), trB), cf(0.05)), cf(1.0));
                const int surv = g.select(g.binary(k::KOp::CmpLt, hash1(s, 900U + bnc), plum), cf(1.0), cf(0.0));
                const int boost = mul(surv, dvv(cf(1.0), plum)); // survive → 1/p, else → 0
                trR = mul(trR, boost); trG = mul(trG, boost); trB = mul(trB, boost);
            }
            ox = add(hpx, mul(cf(1.0e-3), hnx)); oy = add(hpy, mul(cf(1.0e-3), hny)); oz = add(hpz, mul(cf(1.0e-3), hnz));
            dx = dir[0]; dy = dir[1]; dz = dir[2];
        }
    }

    g.stmt_buffer_store(out, uadd(b3, cu(0U)), add(lp(out, b3, 0U), radR));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), add(lp(out, b3, 1U), radG));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), add(lp(out, b3, 2U), radB));
    g.stmt_for_end(floop);

    const int inv = cf(1.0 / static_cast<double>(cfg.samples));
    g.stmt_buffer_store(out, uadd(b3, cu(0U)), mul(lp(out, b3, 0U), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(1U)), mul(lp(out, b3, 1U), inv));
    g.stmt_buffer_store(out, uadd(b3, cu(2U)), mul(lp(out, b3, 2U), inv));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── ReSTIR DI SPATIOTEMPORAL reuse (Bitterli 2020, full algorithm) ────────────────────────────────────────────────────────
// The RIS core (build_restir_di_kernel) reuses nothing between pixels or frames. The full algorithm keeps a PERSISTENT reservoir
// per pixel and, each frame, merges it with (a) its own reservoir from the PREVIOUS frame (TEMPORAL reuse — accumulates effective
// samples across time) and (b) a few spatial NEIGHBOURS' reservoirs (SPATIAL reuse — shares good samples across the image). A
// reservoir merges another via the generalised combine: the other's sample is re-weighted under THIS pixel's target p̂, so the
// same op serves temporal (same p̂) and spatial (different p̂) reuse. Pipeline = 3 passes (temporal → spatial → shade), each a
// separate dispatch (spatial needs every pixel's temporal reservoir finalised first — a cross-thread barrier). The reservoir is
// 6 floats/pixel: [Qx,Qy,Qz, W, M, reserved]. Static scene ⇒ temporal reprojection is the identity (same pixel index).
constexpr crd::u32 kRestirReservoirStride = 6U;

struct RestirStConfig
{
    crd::u32 width  = 16U;      // pixel grid (width·height MUST be a multiple of local_size — no bounds guard in the scalar path)
    crd::u32 height = 16U;
    crd::u32 m_initial      = 4U;    // fresh RIS candidates generated per pixel per frame (reuse supplies the rest)
    crd::u32 spatial_k      = 4U;    // spatial neighbours merged per pixel
    float    spatial_radius = 3.0F;  // neighbour gather radius (pixels)
    float    temporal_m_cap = 20.0F; // clamp the previous reservoir's M to cap·m_initial (bounds temporal staleness bias)
    float    albedo[3]  = {0.6F, 0.6F, 0.6F};
    float    light_p0[3] = {-1.5F, 3.0F, -1.5F};
    float    light_eu[3] = {3.0F, 0.0F, 0.0F};
    float    light_ev[3] = {0.0F, 0.0F, 3.0F};
    float    light_nl[3] = {0.0F, -1.0F, 0.0F};
    float    light_le[3] = {8.0F, 8.0F, 8.0F};
    crd::u32 local_size = 64U;
};

namespace detail
{
// Shared scalar helpers for the ReSTIR reservoir kernels — bundles the ubiquitous KGraph builders + the light/target math so the
// three passes stay consistent. p̂(Q at P,N) = luminance(f·Le)·G, G = clamp(cosθ_s)·clamp(cosθ_l)/d² (unshadowed target).
struct RestirMath
{
    crd::kir::KGraph& g;
    double clum, nlx, nly, nlz;
    crd::kir::Shape sh1 = crd::kir::make_shape({1});
    int cf(double v) { return g.constant(v, sh1, crd::kir::DType::F32); }
    int cu(crd::u32 v) { return g.constant(static_cast<double>(v), sh1, crd::kir::DType::U32); }
    int add(int a, int b) { return g.binary(crd::kir::KOp::Add, a, b); }
    int sub(int a, int b) { return g.binary(crd::kir::KOp::Sub, a, b); }
    int mul(int a, int b) { return g.binary(crd::kir::KOp::Mul, a, b); }
    int dvv(int a, int b) { return g.binary(crd::kir::KOp::Div, a, b); }
    int mx(int a, int b) { return g.binary(crd::kir::KOp::Max, a, b); }
    int mn(int a, int b) { return g.binary(crd::kir::KOp::Min, a, b); }
    int neg(int a) { return g.unary(crd::kir::KOp::Neg, a); }
    int clamp0(int x) { return mx(x, cf(0.0)); }
    int gt0(int x) { return g.select(g.binary(crd::kir::KOp::CmpGt, x, cf(0.0)), cf(1.0), cf(0.0)); }
    int dot3(int ax, int ay, int az, int bx, int by, int bz) { return add(add(mul(ax, bx), mul(ay, by)), mul(az, bz)); }
    int blend(int m, int a, int b) { return add(mul(m, a), mul(sub(cf(1.0), m), b)); } // m∈{0,1}: m?a:b (finite-safe)
    int castf(int x) { return g.cast(x, crd::kir::DType::F32); }
    int castu(int x) { return g.cast(x, crd::kir::DType::U32); }
    int cosv(int x) { return g.unary(crd::kir::KOp::Cos, x); }
    int sinv(int x) { return g.unary(crd::kir::KOp::Sin, x); }
    int sqrtv(int x) { return g.unary(crd::kir::KOp::Sqrt, x); }
    int clampf(int x, double lo, double hi) { return mn(mx(x, cf(lo)), cf(hi)); }
    // Duff 2017 branchless orthonormal tangent frame around a normal.
    void frameb(int nx, int ny, int nz, int t[3], int b[3])
    {
        const int sg = g.select(g.binary(crd::kir::KOp::CmpGe, nz, cf(0.0)), cf(1.0), cf(-1.0));
        const int fa = dvv(cf(-1.0), add(sg, nz));
        const int fb = mul(mul(nx, ny), fa);
        t[0] = add(cf(1.0), mul(sg, mul(mul(nx, nx), fa))); t[1] = mul(sg, fb); t[2] = mul(neg(sg), nx);
        b[0] = fb; b[1] = add(sg, mul(mul(ny, ny), fa)); b[2] = neg(ny);
    }
    // cosine-weighted hemisphere direction (local z = N) → d[3]; returns cosθ (= sampled lz = pdf·π).
    int cosine_dirb(int u1, int u2, int nx, int ny, int nz, const int t[3], const int b[3], int d[3])
    {
        const int rr  = g.unary(crd::kir::KOp::Sqrt, u1);
        const int phi = mul(cf(2.0 * 3.14159265358979323846), u2);
        const int lx  = mul(rr, g.unary(crd::kir::KOp::Cos, phi));
        const int ly  = mul(rr, g.unary(crd::kir::KOp::Sin, phi));
        const int lz  = g.unary(crd::kir::KOp::Sqrt, mx(sub(cf(1.0), u1), cf(0.0)));
        d[0] = add(add(mul(lx, t[0]), mul(ly, b[0])), mul(lz, nx));
        d[1] = add(add(mul(lx, t[1]), mul(ly, b[1])), mul(lz, ny));
        d[2] = add(add(mul(lx, t[2]), mul(ly, b[2])), mul(lz, nz));
        return lz;
    }
    // p̂(Q) at shading point (Px,Py,Pz) with normal (Nx,Ny,Nz).
    int phat(int Px, int Py, int Pz, int Nx, int Ny, int Nz, int Qx, int Qy, int Qz)
    {
        const int dx = sub(Qx, Px), dy = sub(Qy, Py), dz = sub(Qz, Pz);
        const int d2 = mx(dot3(dx, dy, dz, dx, dy, dz), cf(1.0e-12));
        const int invd = dvv(cf(1.0), g.unary(crd::kir::KOp::Sqrt, d2));
        const int cs = clamp0(mul(dot3(Nx, Ny, Nz, dx, dy, dz), invd));
        const int cl = clamp0(neg(mul(dot3(cf(nlx), cf(nly), cf(nlz), dx, dy, dz), invd)));
        return mul(cf(clum), dvv(mul(cs, cl), d2));
    }
};
} // namespace detail

// PASS 1/3 — INITIAL RIS + TEMPORAL reuse. Per pixel: stream m_initial fresh light candidates into a reservoir (RIS), then merge
// the previous frame's reservoir at the same pixel (its M clamped to bound staleness). Buffers: pos (b1, 3f), nrm (b2, 3f),
// reservoir_prev (b3, 6f), reservoir_out (b4, 6f), frame (b5, 1×u32). No AS — RIS defers visibility to the shade pass.
[[nodiscard]] inline crd::kir::KEntry build_restir_temporal_kernel(crd::kir::KGraph& g, const RestirStConfig& cfg)
{
    namespace k = crd::kir;
    constexpr double kInvPi = 1.0 / 3.14159265358979323846;
    const double p0x = cfg.light_p0[0], p0y = cfg.light_p0[1], p0z = cfg.light_p0[2];
    const double eux = cfg.light_eu[0], euy = cfg.light_eu[1], euz = cfg.light_eu[2];
    const double evx = cfg.light_ev[0], evy = cfg.light_ev[1], evz = cfg.light_ev[2];
    const double crx = euy * evz - euz * evy, cry = euz * evx - eux * evz, crz = eux * evy - euy * evx;
    const double area = crd::math::sqrt(crx * crx + cry * cry + crz * crz);
    const double clum = kInvPi * (0.2126 * static_cast<double>(cfg.albedo[0]) * static_cast<double>(cfg.light_le[0])
                                + 0.7152 * static_cast<double>(cfg.albedo[1]) * static_cast<double>(cfg.light_le[1])
                                + 0.0722 * static_cast<double>(cfg.albedo[2]) * static_cast<double>(cfg.light_le[2]));
    detail::RestirMath m{g, clum, cfg.light_nl[0], cfg.light_nl[1], cfg.light_nl[2]};

    const int pos   = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm   = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int rprev = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int rout  = g.buffer_decl(k::DType::F32, 0, 4, true);
    const int fbuf  = g.buffer_decl(k::DType::U32, 0, 5, false);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = m.add(m.mul(g.builtin(k::KBuiltin::WorkgroupIndex), m.cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = m.mul(tid, m.cu(3U));
    const int  rb   = m.mul(tid, m.cu(kRestirReservoirStride));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, m.add(base, m.cu(c))); };
    const int  Px = lp(pos, b3, 0U), Py = lp(pos, b3, 1U), Pz = lp(pos, b3, 2U);
    const int  Nx = lp(nrm, b3, 0U), Ny = lp(nrm, b3, 1U), Nz = lp(nrm, b3, 2U);
    const int  frame = g.buffer_load(fbuf, m.cu(0U));

    // ── fresh RIS reservoir over m_initial candidates ──
    int wsum = m.cf(0.0);
    int cQx = m.cf(p0x), cQy = m.cf(p0y), cQz = m.cf(p0z), cPh = m.cf(0.0);
    for (crd::u32 i = 0; i < cfg.m_initial; ++i)
    {
        const int seed = m.add(m.mul(frame, m.cu(0x9E3779B9U)), m.add(m.mul(tid, m.cu(0x85EBCA77U)), m.cu(i * 0x2545F491U + 1U)));
        const int u1   = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x632BE5ABU)), seed));
        const int u2   = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x85157AF5U)), m.mul(seed, m.cu(0xC2B2AE35U))));
        const int Qx = m.add(m.add(m.cf(p0x), m.mul(u1, m.cf(eux))), m.mul(u2, m.cf(evx)));
        const int Qy = m.add(m.add(m.cf(p0y), m.mul(u1, m.cf(euy))), m.mul(u2, m.cf(evy)));
        const int Qz = m.add(m.add(m.cf(p0z), m.mul(u1, m.cf(euz))), m.mul(u2, m.cf(evz)));
        const int ph = m.phat(Px, Py, Pz, Nx, Ny, Nz, Qx, Qy, Qz);
        const int w  = m.mul(ph, m.cf(area)); // p̂ / (1/area)
        wsum = m.add(wsum, w);
        const int xi   = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x27D4EB2FU)), m.mul(seed, m.cu(0x165667B1U))));
        const int repl = g.select(g.binary(k::KOp::CmpLt, m.mul(xi, wsum), w), m.cf(1.0), m.cf(0.0));
        cQx = m.blend(repl, Qx, cQx); cQy = m.blend(repl, Qy, cQy); cQz = m.blend(repl, Qz, cQz);
        cPh = m.blend(repl, ph, cPh);
    }
    const int Wnew = m.dvv(wsum, m.mul(m.cf(static_cast<double>(cfg.m_initial)), m.mx(cPh, m.cf(1.0e-12))));
    const int Mnew = m.cf(static_cast<double>(cfg.m_initial));

    // ── temporal combine with the previous frame's reservoir at this pixel ──
    const int pQx = lp(rprev, rb, 0U), pQy = lp(rprev, rb, 1U), pQz = lp(rprev, rb, 2U);
    const int pW  = lp(rprev, rb, 3U);
    const int pM  = m.mn(lp(rprev, rb, 4U), m.cf(cfg.temporal_m_cap * static_cast<double>(cfg.m_initial))); // staleness cap
    const int phP = m.phat(Px, Py, Pz, Nx, Ny, Nz, pQx, pQy, pQz);                                          // prev sample's p̂ here
    const int wA  = m.mul(m.mul(cPh, Wnew), Mnew);   // = wsum (this pixel's fresh contribution)
    const int wB  = m.mul(m.mul(phP, pW), pM);       // the reprojected temporal contribution
    const int ws2 = m.add(wA, wB);
    const int M2  = m.add(Mnew, pM);
    const int xi2 = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x9E3779B1U)), m.mul(frame, m.cu(0x7FEB352DU))));
    const int pick = g.select(g.binary(k::KOp::CmpLt, m.mul(xi2, ws2), wB), m.cf(1.0), m.cf(0.0)); // keep prev with prob wB/ws2
    const int oQx = m.blend(pick, pQx, cQx), oQy = m.blend(pick, pQy, cQy), oQz = m.blend(pick, pQz, cQz);
    const int phO = m.blend(pick, phP, cPh);
    const int Wout = m.dvv(ws2, m.mul(M2, m.mx(phO, m.cf(1.0e-12)))); // same-pixel merge ⇒ both domains valid ⇒ unbiased = biased

    g.stmt_buffer_store(rout, m.add(rb, m.cu(0U)), oQx);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(1U)), oQy);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(2U)), oQz);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(3U)), Wout);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(4U)), M2);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(5U)), m.cf(0.0));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// PASS 2/3 — SPATIAL reuse. Per pixel: merge k spatial NEIGHBOURS' reservoirs (read from the temporal-pass output) into this
// pixel's reservoir. A neighbour's sample is re-weighted under THIS pixel's target p̂ (different shading point ⇒ different p̂), so
// the merge is the same generalised combine as temporal. Because neighbours have different domains, the final weight uses the
// UNBIASED normalisation W = Σwᵢ/(Z·p̂(y)) where Z = Σ Mᵢ over the reservoirs whose domain actually contains the chosen sample y
// (Bitterli 2020 §"unbiased") — a second neighbour pass recomputes each domain's validity. Buffers: pos (b1, 3f), nrm (b2, 3f),
// reservoir_in (b3, 6f = temporal output), reservoir_out (b4, 6f), frame (b5, 1×u32).
[[nodiscard]] inline crd::kir::KEntry build_restir_spatial_kernel(crd::kir::KGraph& g, const RestirStConfig& cfg)
{
    namespace k = crd::kir;
    constexpr double kInvPi = 1.0 / 3.14159265358979323846;
    const double clum = kInvPi * (0.2126 * static_cast<double>(cfg.albedo[0]) * static_cast<double>(cfg.light_le[0])
                                + 0.7152 * static_cast<double>(cfg.albedo[1]) * static_cast<double>(cfg.light_le[1])
                                + 0.0722 * static_cast<double>(cfg.albedo[2]) * static_cast<double>(cfg.light_le[2]));
    detail::RestirMath m{g, clum, cfg.light_nl[0], cfg.light_nl[1], cfg.light_nl[2]};

    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int rin  = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int rout = g.buffer_decl(k::DType::F32, 0, 4, true);
    const int fbuf = g.buffer_decl(k::DType::U32, 0, 5, false);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = m.add(m.mul(g.builtin(k::KBuiltin::WorkgroupIndex), m.cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = m.mul(tid, m.cu(3U));
    const int  rb   = m.mul(tid, m.cu(kRestirReservoirStride));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, m.add(base, m.cu(c))); };
    const int  Px = lp(pos, b3, 0U), Py = lp(pos, b3, 1U), Pz = lp(pos, b3, 2U);
    const int  Nx = lp(nrm, b3, 0U), Ny = lp(nrm, b3, 1U), Nz = lp(nrm, b3, 2U);
    const int  frame = g.buffer_load(fbuf, m.cu(0U));
    const int  sx = m.g.binary(k::KOp::Mod, tid, m.cu(cfg.width)); // pixel column
    const int  sy = m.g.binary(k::KOp::Div, tid, m.cu(cfg.width)); // pixel row
    const int  sxf = m.castf(sx), syf = m.castf(sy);

    // self reservoir
    const int sQx = lp(rin, rb, 0U), sQy = lp(rin, rb, 1U), sQz = lp(rin, rb, 2U);
    const int sW  = lp(rin, rb, 3U), sM = lp(rin, rb, 4U);
    int wsum = m.mul(m.mul(m.phat(Px, Py, Pz, Nx, Ny, Nz, sQx, sQy, sQz), sW), sM);
    int M    = sM;
    int oQx = sQx, oQy = sQy, oQz = sQz;

    // neighbour offset (disk sample) reproduced identically in both passes from the same seed.
    const auto nbr_index = [&](crd::u32 j) {
        const int seed = m.add(m.mul(frame, m.cu(0x9E3779B9U)), m.add(m.mul(tid, m.cu(0x68E31DA4U)), m.cu(j * 0xB5297A4DU + 3U)));
        const int ra = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x1B56C4E9U)), seed));
        const int rr = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x9E3779B1U)), m.mul(seed, m.cu(0x85EBCA6BU))));
        const int th = m.mul(m.cf(2.0 * 3.14159265358979323846), ra);
        const int rd = m.mul(m.cf(static_cast<double>(cfg.spatial_radius)), m.sqrtv(rr));
        const int nxf = m.clampf(m.add(sxf, m.mul(rd, m.cosv(th))), 0.0, static_cast<double>(cfg.width - 1U));
        const int nyf = m.clampf(m.add(syf, m.mul(rd, m.sinv(th))), 0.0, static_cast<double>(cfg.height - 1U));
        return m.add(m.mul(m.castu(nyf), m.cu(cfg.width)), m.castu(nxf)); // clamped ≥0 ⇒ trunc = floor
    };

    // ── pass A: WRS-merge each neighbour's reservoir under self's p̂ ──
    for (crd::u32 j = 0; j < cfg.spatial_k; ++j)
    {
        const int nbr = nbr_index(j);
        const int nrb = m.mul(nbr, m.cu(kRestirReservoirStride));
        const int bQx = lp(rin, nrb, 0U), bQy = lp(rin, nrb, 1U), bQz = lp(rin, nrb, 2U);
        const int bW  = lp(rin, nrb, 3U), bM = lp(rin, nrb, 4U);
        const int phB = m.phat(Px, Py, Pz, Nx, Ny, Nz, bQx, bQy, bQz); // neighbour's sample at SELF
        const int wB  = m.mul(m.mul(phB, bW), bM);
        wsum = m.add(wsum, wB);
        M    = m.add(M, bM);
        const int xi   = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x27D4EB2FU)), m.mul(m.add(frame, m.cu(j * 101U + 1U)), m.cu(0x165667B1U))));
        const int pick = g.select(g.binary(k::KOp::CmpLt, m.mul(xi, wsum), wB), m.cf(1.0), m.cf(0.0));
        oQx = m.blend(pick, bQx, oQx); oQy = m.blend(pick, bQy, oQy); oQz = m.blend(pick, bQz, oQz);
    }

    // ── pass B: UNBIASED normalisation — Z = ΣMᵢ over reservoirs whose domain contains the chosen sample oQ ──
    const int phOself = m.phat(Px, Py, Pz, Nx, Ny, Nz, oQx, oQy, oQz);
    int Z = m.mul(m.gt0(phOself), sM); // self contributes its M iff oQ is in the self domain
    for (crd::u32 j = 0; j < cfg.spatial_k; ++j)
    {
        const int nbr = nbr_index(j);            // same seed ⇒ same neighbour as pass A
        const int nrb = m.mul(nbr, m.cu(kRestirReservoirStride));
        const int nb3 = m.mul(nbr, m.cu(3U));
        const int nPx = lp(pos, nb3, 0U), nPy = lp(pos, nb3, 1U), nPz = lp(pos, nb3, 2U);
        const int nNx = lp(nrm, nb3, 0U), nNy = lp(nrm, nb3, 1U), nNz = lp(nrm, nb3, 2U);
        const int bM  = lp(rin, nrb, 4U);
        const int phNb = m.phat(nPx, nPy, nPz, nNx, nNy, nNz, oQx, oQy, oQz); // is oQ in neighbour j's domain?
        Z = m.add(Z, m.mul(m.gt0(phNb), bM));
    }
    const int Wout = m.dvv(wsum, m.mul(m.mx(Z, m.cf(1.0e-6)), m.mx(phOself, m.cf(1.0e-12))));

    g.stmt_buffer_store(rout, m.add(rb, m.cu(0U)), oQx);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(1U)), oQy);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(2U)), oQz);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(3U)), Wout);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(4U)), M);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(5U)), m.cf(0.0));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// PASS 3/3 — SHADE. Per pixel: take the final reservoir sample, trace ONE visibility ray, output L = f·Le·G(y)·V·W. Buffers:
// TLAS (b0), pos (b1, 3f), nrm (b2, 3f), reservoir_in (b3, 6f), radiance out (b4, 3f).
[[nodiscard]] inline crd::kir::KEntry build_restir_shade_kernel(crd::kir::KGraph& g, const RestirStConfig& cfg)
{
    namespace k = crd::kir;
    constexpr double kInvPi = 1.0 / 3.14159265358979323846;
    const double a0Le0 = static_cast<double>(cfg.albedo[0]) * static_cast<double>(cfg.light_le[0]) * kInvPi;
    const double a1Le1 = static_cast<double>(cfg.albedo[1]) * static_cast<double>(cfg.light_le[1]) * kInvPi;
    const double a2Le2 = static_cast<double>(cfg.albedo[2]) * static_cast<double>(cfg.light_le[2]) * kInvPi;
    detail::RestirMath m{g, 0.0, cfg.light_nl[0], cfg.light_nl[1], cfg.light_nl[2]};

    const int as   = g.accel_struct_decl(0, 0);
    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int rin  = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int out  = g.buffer_decl(k::DType::F32, 0, 4, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = m.add(m.mul(g.builtin(k::KBuiltin::WorkgroupIndex), m.cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = m.mul(tid, m.cu(3U));
    const int  rb   = m.mul(tid, m.cu(kRestirReservoirStride));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, m.add(base, m.cu(c))); };
    const int  Px = lp(pos, b3, 0U), Py = lp(pos, b3, 1U), Pz = lp(pos, b3, 2U);
    const int  Nx = lp(nrm, b3, 0U), Ny = lp(nrm, b3, 1U), Nz = lp(nrm, b3, 2U);
    const int  Qx = lp(rin, rb, 0U), Qy = lp(rin, rb, 1U), Qz = lp(rin, rb, 2U);
    const int  W  = lp(rin, rb, 3U);

    const int dcx = m.sub(Qx, Px), dcy = m.sub(Qy, Py), dcz = m.sub(Qz, Pz);
    const int dc2 = m.mx(m.dot3(dcx, dcy, dcz, dcx, dcy, dcz), m.cf(1.0e-12));
    const int dist = g.unary(k::KOp::Sqrt, dc2);
    const int invc = m.dvv(m.cf(1.0), dist);
    const int wix = m.mul(dcx, invc), wiy = m.mul(dcy, invc), wiz = m.mul(dcz, invc);
    const int cs  = m.clamp0(m.dot3(Nx, Ny, Nz, wix, wiy, wiz));
    const int cl  = m.clamp0(m.neg(m.dot3(m.cf(cfg.light_nl[0]), m.cf(cfg.light_nl[1]), m.cf(cfg.light_nl[2]), wix, wiy, wiz)));
    const int Gc  = m.dvv(m.mul(cs, cl), dc2);
    const int sox = m.add(Px, m.mul(m.cf(1.0e-3), Nx)), soy = m.add(Py, m.mul(m.cf(1.0e-3), Ny)), soz = m.add(Pz, m.mul(m.cf(1.0e-3), Nz));
    const int tmxs = m.mul(dist, m.cf(1.0 - 1.0e-3));
    const int st   = g.trace_ray_closest(as, sox, soy, soz, wix, wiy, wiz, m.cf(1.0e-3), tmxs);
    const int V    = g.select(g.binary(k::KOp::CmpLt, st, tmxs), m.cf(0.0), m.cf(1.0));
    const int shade = m.mul(m.mul(Gc, V), W);
    g.stmt_buffer_store(out, m.add(b3, m.cu(0U)), m.mul(m.cf(a0Le0), shade));
    g.stmt_buffer_store(out, m.add(b3, m.cu(1U)), m.mul(m.cf(a1Le1), shade));
    g.stmt_buffer_store(out, m.add(b3, m.cu(2U)), m.mul(m.cf(a2Le2), shade));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── ReSTIR GI (Ouyang et al. 2021 — path resampling for real-time indirect) ───────────────────────────────────────────────
// ReSTIR DI resamples LIGHT samples; ReSTIR GI resamples SAMPLE POINTS — the secondary vertex xs of a 1-bounce indirect path,
// carrying the outgoing radiance Lo(xs) toward the visible point xv. A pixel reuses a neighbour's sample point by RECONNECTING it
// to its own xv, which changes the solid-angle measure — hence the JACOBIAN J = (cosθ_s^new/d_new²)·(d_old²/cosθ_s^old). The
// reservoir is 12 floats/pixel: [xs.xyz(0-2), ns.xyz(3-5), Lo.rgb(6-8), W(9), M(10), pad(11)]. 3 passes (sample+temporal →
// spatial → shade), the GI twin of the DI spatiotemporal pipeline. Target p̂ = luminance(Lo)·cosθ_v (the varying part of the
// integrand; the constant BRDF f=albedo/π folds into the shade). Reuse denoises the noisy 1-sample Lo estimate.
constexpr crd::u32 kRestirGiStride = 12U;

struct RestirGiConfig
{
    crd::u32 width  = 16U;
    crd::u32 height = 16U;
    crd::u32 spatial_k      = 4U;
    float    spatial_radius = 3.0F;
    float    temporal_m_cap = 20.0F;
    float    albedo[3]  = {0.6F, 0.6F, 0.6F};
    crd::u32 ntri        = 4U;  // scene tris (clamps the hit flat-normal fetch)
    crd::u32 nlights     = 1U;  // area lights (for the Lo-at-xs direct lighting)
    crd::u32 local_size  = 64U;
};

namespace detail
{
// GI helpers on top of RestirMath: the light-NEE that estimates Lo at a sample point, and the reservoir field accessors.
struct GiMath : RestirMath
{
    // one-light-sample DIRECT lighting at (Px,N) using the light buffer `lts` and shadow-ray tracing against `as`. Returns the
    // outgoing (diffuse) radiance per channel into Lo[3]. albedo/π · Le · G · V · N · area, uniform light pick.
    void nee_direct(int lts, int as, int Px, int Py, int Pz, int Nx, int Ny, int Nz, int u_pick, int su1, int su2,
                    crd::u32 nlights, double a0, double a1, double a2, int Lo[3])
    {
        const int li = mn(g.cast(g.unary(crd::kir::KOp::Floor, mul(u_pick, cf(static_cast<double>(nlights)))), crd::kir::DType::U32), cu(nlights > 0U ? nlights - 1U : 0U));
        const int lb = mul(li, cu(15U));
        const auto ld = [&](crd::u32 c) { return g.buffer_load(lts, add(lb, cu(c))); };
        const int p0x = ld(0U), p0y = ld(1U), p0z = ld(2U), eux = ld(3U), euy = ld(4U), euz = ld(5U);
        const int evx = ld(6U), evy = ld(7U), evz = ld(8U), lnx = ld(9U), lny = ld(10U), lnz = ld(11U);
        const int lex = ld(12U), ley = ld(13U), lez = ld(14U);
        const int crx = sub(mul(euy, evz), mul(euz, evy)), cry = sub(mul(euz, evx), mul(eux, evz)), crz = sub(mul(eux, evy), mul(euy, evx));
        const int area = g.unary(crd::kir::KOp::Sqrt, mx(dot3(crx, cry, crz, crx, cry, crz), cf(1.0e-20)));
        const int Qx = add(add(p0x, mul(su1, eux)), mul(su2, evx));
        const int Qy = add(add(p0y, mul(su1, euy)), mul(su2, evy));
        const int Qz = add(add(p0z, mul(su1, euz)), mul(su2, evz));
        const int dx = sub(Qx, Px), dy = sub(Qy, Py), dz = sub(Qz, Pz);
        const int d2 = mx(dot3(dx, dy, dz, dx, dy, dz), cf(1.0e-12));
        const int dist = g.unary(crd::kir::KOp::Sqrt, d2);
        const int invd = dvv(cf(1.0), dist);
        const int wix = mul(dx, invd), wiy = mul(dy, invd), wiz = mul(dz, invd);
        const int cs = clamp0(dot3(Nx, Ny, Nz, wix, wiy, wiz));
        const int cl = clamp0(neg(dot3(lnx, lny, lnz, wix, wiy, wiz)));
        const int sox = add(Px, mul(cf(1.0e-3), Nx)), soy = add(Py, mul(cf(1.0e-3), Ny)), soz = add(Pz, mul(cf(1.0e-3), Nz));
        const int tmxs = mul(dist, cf(1.0 - 1.0e-3));
        const int st   = g.trace_ray_closest(as, sox, soy, soz, wix, wiy, wiz, cf(1.0e-3), tmxs);
        const int vis  = g.select(g.binary(crd::kir::KOp::CmpLt, st, tmxs), cf(0.0), cf(1.0));
        // Lo = (albedo/π)·Le·(cos_s·cos_l/d²)·V·N·area  (uniform light pick ⇒ ×N; area measure ⇒ ×area)
        const int base = mul(mul(mul(dvv(mul(cs, cl), d2), vis), mul(area, cf(static_cast<double>(nlights)))), cf(1.0 / 3.14159265358979323846));
        Lo[0] = mul(mul(cf(a0), lex), base);
        Lo[1] = mul(mul(cf(a1), ley), base);
        Lo[2] = mul(mul(cf(a2), lez), base);
    }
};
} // namespace detail

// PASS 1/3 — initial GI sample + temporal reuse. Per pixel: cosine-sample a direction from xv, trace to the sample point xs,
// estimate Lo(xs) via one-light NEE, form the reservoir (W=1/pdf=π/cosθ_v, M=1), then temporal-merge the previous frame's
// reservoir at the same pixel (same xv ⇒ no Jacobian). Buffers: TLAS (b0), pos (b1,3f), nrm (b2,3f), tn (b3,3f/tri),
// lights (b4,15f), reservoir_prev (b5,12f), reservoir_out (b6,12f), frame (b7,u32).
[[nodiscard]] inline crd::kir::KEntry build_restir_gi_temporal_kernel(crd::kir::KGraph& g, const RestirGiConfig& cfg)
{
    namespace k = crd::kir;
    const double a0 = static_cast<double>(cfg.albedo[0]), a1 = static_cast<double>(cfg.albedo[1]), a2 = static_cast<double>(cfg.albedo[2]);
    const double clum0 = 0.2126, clum1 = 0.7152, clum2 = 0.0722;
    detail::GiMath m{g, 0.0, 0.0, 0.0, 0.0};

    const int as   = g.accel_struct_decl(0, 0);
    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int tn   = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int lts  = g.buffer_decl(k::DType::F32, 0, 4, false);
    const int rprev = g.buffer_decl(k::DType::F32, 0, 5, false);
    const int rout  = g.buffer_decl(k::DType::F32, 0, 6, true);
    const int fbuf  = g.buffer_decl(k::DType::U32, 0, 7, false);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = m.add(m.mul(g.builtin(k::KBuiltin::WorkgroupIndex), m.cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = m.mul(tid, m.cu(3U));
    const int  rb   = m.mul(tid, m.cu(kRestirGiStride));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, m.add(base, m.cu(c))); };
    const int  Nx = lp(nrm, b3, 0U), Ny = lp(nrm, b3, 1U), Nz = lp(nrm, b3, 2U);
    const int  Px = lp(pos, b3, 0U), Py = lp(pos, b3, 1U), Pz = lp(pos, b3, 2U);
    const int  frame = g.buffer_load(fbuf, m.cu(0U));

    // cosine sample a bounce direction from (xv,N).
    int T[3], Bt[3]; m.frameb(Nx, Ny, Nz, T, Bt);
    const int seed = m.add(m.mul(frame, m.cu(0x9E3779B9U)), m.mul(tid, m.cu(0x85EBCA77U)));
    const int u1 = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x632BE5ABU)), seed));
    const int u2 = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x85157AF5U)), m.mul(seed, m.cu(0xC2B2AE35U))));
    int dir[3]; const int cosv = m.cosine_dirb(u1, u2, Nx, Ny, Nz, T, Bt, dir);
    const int ox = m.add(Px, m.mul(m.cf(1.0e-3), Nx)), oy = m.add(Py, m.mul(m.cf(1.0e-3), Ny)), oz = m.add(Pz, m.mul(m.cf(1.0e-3), Nz));
    const k::KGraph::RtHit hit = g.trace_ray_hit(as, ox, oy, oz, dir[0], dir[1], dir[2], m.cf(1.0e-3), m.cf(1.0e30));
    const int miss = g.binary(k::KOp::CmpEq, hit.prim, m.cu(0xFFFFFFFFU));
    const int hitm = g.select(miss, m.cf(0.0), m.cf(1.0));
    const int tc   = m.mn(hit.t, m.cf(1.0e5));
    const int xsx = m.add(ox, m.mul(tc, dir[0])), xsy = m.add(oy, m.mul(tc, dir[1])), xsz = m.add(oz, m.mul(tc, dir[2]));
    const int cprim = m.mn(hit.prim, m.cu(cfg.ntri > 0U ? cfg.ntri - 1U : 0U));
    const int tb = m.mul(cprim, m.cu(3U));
    const int nsx = lp(tn, tb, 0U), nsy = lp(tn, tb, 1U), nsz = lp(tn, tb, 2U);
    // Lo(xs) via one-light NEE (zeroed on a miss).
    const int up  = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x2C1B3C6DU)), m.mul(seed, m.cu(0x9E3779B1U))));
    const int lu1 = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x27D4EB2FU)), m.mul(seed, m.cu(0x165667B1U))));
    const int lu2 = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x9E3779B7U)), m.mul(seed, m.cu(0x85EBCA6BU))));
    int Lo[3]; m.nee_direct(lts, as, xsx, xsy, xsz, nsx, nsy, nsz, up, lu1, lu2, cfg.nlights, a0, a1, a2, Lo);
    Lo[0] = m.mul(Lo[0], hitm); Lo[1] = m.mul(Lo[1], hitm); Lo[2] = m.mul(Lo[2], hitm);
    // initial reservoir: target p̂ = lum(Lo)·cosθ_v; W = 1/pdf = π/cosθ_v (M=1). Guard cosθ_v.
    const int cvg   = m.mx(cosv, m.cf(1.0e-4));
    const int Winit = m.mul(m.cf(3.14159265358979323846), m.dvv(m.cf(1.0), cvg));
    const int phInit = m.mul(m.add(m.add(m.mul(m.cf(clum0), Lo[0]), m.mul(m.cf(clum1), Lo[1])), m.mul(m.cf(clum2), Lo[2])), cvg);

    // temporal combine with the previous frame's reservoir at this pixel (same xv).
    const int pxs0 = lp(rprev, rb, 0U), pxs1 = lp(rprev, rb, 1U), pxs2 = lp(rprev, rb, 2U);
    const int pns0 = lp(rprev, rb, 3U), pns1 = lp(rprev, rb, 4U), pns2 = lp(rprev, rb, 5U);
    const int pLo0 = lp(rprev, rb, 6U), pLo1 = lp(rprev, rb, 7U), pLo2 = lp(rprev, rb, 8U);
    const int pW = lp(rprev, rb, 9U);
    const int pM = m.mn(lp(rprev, rb, 10U), m.cf(cfg.temporal_m_cap));
    // prev sample's cosθ_v at this xv:
    const int pdx = m.sub(pxs0, Px), pdy = m.sub(pxs1, Py), pdz = m.sub(pxs2, Pz);
    const int pil = m.dvv(m.cf(1.0), g.unary(k::KOp::Sqrt, m.mx(m.dot3(pdx, pdy, pdz, pdx, pdy, pdz), m.cf(1.0e-12))));
    const int pcv = m.clamp0(m.mul(m.dot3(Nx, Ny, Nz, pdx, pdy, pdz), pil));
    const int phPrev = m.mul(m.add(m.add(m.mul(m.cf(clum0), pLo0), m.mul(m.cf(clum1), pLo1)), m.mul(m.cf(clum2), pLo2)), pcv);
    const int wA = m.mul(m.mul(phInit, Winit), m.cf(1.0));   // self M=1
    const int wB = m.mul(m.mul(phPrev, pW), pM);
    const int ws = m.add(wA, wB);
    const int M2 = m.add(m.cf(1.0), pM);
    const int xi = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x9E3779B1U)), m.mul(frame, m.cu(0x7FEB352DU))));
    const int pick = g.select(g.binary(k::KOp::CmpLt, m.mul(xi, ws), wB), m.cf(1.0), m.cf(0.0)); // keep prev with prob wB/ws
    const int oX0 = m.blend(pick, pxs0, xsx), oX1 = m.blend(pick, pxs1, xsy), oX2 = m.blend(pick, pxs2, xsz);
    const int oN0 = m.blend(pick, pns0, nsx), oN1 = m.blend(pick, pns1, nsy), oN2 = m.blend(pick, pns2, nsz);
    const int oL0 = m.blend(pick, pLo0, Lo[0]), oL1 = m.blend(pick, pLo1, Lo[1]), oL2 = m.blend(pick, pLo2, Lo[2]);
    const int phO = m.blend(pick, phPrev, phInit);
    const int Wout = m.dvv(ws, m.mul(M2, m.mx(phO, m.cf(1.0e-12))));

    g.stmt_buffer_store(rout, m.add(rb, m.cu(0U)), oX0);  g.stmt_buffer_store(rout, m.add(rb, m.cu(1U)), oX1);  g.stmt_buffer_store(rout, m.add(rb, m.cu(2U)), oX2);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(3U)), oN0);  g.stmt_buffer_store(rout, m.add(rb, m.cu(4U)), oN1);  g.stmt_buffer_store(rout, m.add(rb, m.cu(5U)), oN2);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(6U)), oL0);  g.stmt_buffer_store(rout, m.add(rb, m.cu(7U)), oL1);  g.stmt_buffer_store(rout, m.add(rb, m.cu(8U)), oL2);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(9U)), Wout); g.stmt_buffer_store(rout, m.add(rb, m.cu(10U)), M2); g.stmt_buffer_store(rout, m.add(rb, m.cu(11U)), m.cf(0.0));

    k::KEntry e;
    e.stage = k::KStage::Compute; e.local_size[0] = cfg.local_size; e.kernel_body_begin = mark; e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// PASS 2/3 — SPATIAL reuse with the JACOBIAN reconnection. Per pixel: merge k neighbours' GI reservoirs, reconnecting each
// neighbour's sample point xs_q to THIS pixel's xv. Reconnection changes the solid-angle measure, so the neighbour's weight is
// scaled by J = (cosθ_s^new · d_old²) / (cosθ_s^old · d_new²) — the ratio of the sample point's projected solid angle from the two
// visible points. Buffers: pos (b1,3f), nrm (b2,3f), reservoir_in (b3,12f), reservoir_out (b4,12f), frame (b5,u32).
[[nodiscard]] inline crd::kir::KEntry build_restir_gi_spatial_kernel(crd::kir::KGraph& g, const RestirGiConfig& cfg)
{
    namespace k = crd::kir;
    const double clum0 = 0.2126, clum1 = 0.7152, clum2 = 0.0722;
    detail::GiMath m{g, 0.0, 0.0, 0.0, 0.0};

    const int pos  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int rin  = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int rout = g.buffer_decl(k::DType::F32, 0, 4, true);
    const int fbuf = g.buffer_decl(k::DType::U32, 0, 5, false);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = m.add(m.mul(g.builtin(k::KBuiltin::WorkgroupIndex), m.cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = m.mul(tid, m.cu(3U));
    const int  rb   = m.mul(tid, m.cu(kRestirGiStride));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, m.add(base, m.cu(c))); };
    const int  Nx = lp(nrm, b3, 0U), Ny = lp(nrm, b3, 1U), Nz = lp(nrm, b3, 2U);
    const int  Px = lp(pos, b3, 0U), Py = lp(pos, b3, 1U), Pz = lp(pos, b3, 2U);
    const int  frame = g.buffer_load(fbuf, m.cu(0U));
    const int  sx = m.g.binary(k::KOp::Mod, tid, m.cu(cfg.width)), sy = m.g.binary(k::KOp::Div, tid, m.cu(cfg.width));
    const int  sxf = m.castf(sx), syf = m.castf(sy);

    // luminance·cosθ_v target of a stored sample (xs,Lo) seen from (Px,N); also returns cos_v, cos_s (via out params) for reuse.
    const auto phat_gi = [&](int xs0, int xs1, int xs2, int ns0, int ns1, int ns2, int Lo0, int Lo1, int Lo2, int& cvOut, int& csOut, int& d2Out) {
        const int dx = m.sub(xs0, Px), dy = m.sub(xs1, Py), dz = m.sub(xs2, Pz);
        const int d2 = m.mx(m.dot3(dx, dy, dz, dx, dy, dz), m.cf(1.0e-12));
        const int il = m.dvv(m.cf(1.0), g.unary(k::KOp::Sqrt, d2));
        cvOut = m.clamp0(m.mul(m.dot3(Nx, Ny, Nz, dx, dy, dz), il));
        csOut = m.clamp0(m.neg(m.mul(m.dot3(ns0, ns1, ns2, dx, dy, dz), il)));
        d2Out = d2;
        const int lum = m.add(m.add(m.mul(m.cf(clum0), Lo0), m.mul(m.cf(clum1), Lo1)), m.mul(m.cf(clum2), Lo2));
        return m.mul(lum, cvOut);
    };

    // self reservoir.
    const int sX0 = lp(rin, rb, 0U), sX1 = lp(rin, rb, 1U), sX2 = lp(rin, rb, 2U);
    const int sN0 = lp(rin, rb, 3U), sN1 = lp(rin, rb, 4U), sN2 = lp(rin, rb, 5U);
    const int sL0 = lp(rin, rb, 6U), sL1 = lp(rin, rb, 7U), sL2 = lp(rin, rb, 8U);
    const int sW = lp(rin, rb, 9U), sM = lp(rin, rb, 10U);
    int cvS = -1, csS = -1, d2S = -1;
    const int phS = phat_gi(sX0, sX1, sX2, sN0, sN1, sN2, sL0, sL1, sL2, cvS, csS, d2S);
    int wsum = m.mul(m.mul(phS, sW), sM);
    int M    = sM;
    int oX0 = sX0, oX1 = sX1, oX2 = sX2, oN0 = sN0, oN1 = sN1, oN2 = sN2, oL0 = sL0, oL1 = sL1, oL2 = sL2, phO = phS;

    for (crd::u32 j = 0; j < cfg.spatial_k; ++j)
    {
        const int seed = m.add(m.mul(frame, m.cu(0x9E3779B9U)), m.add(m.mul(tid, m.cu(0x68E31DA4U)), m.cu(j * 0xB5297A4DU + 3U)));
        const int ra = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x1B56C4E9U)), seed));
        const int rr = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x9E3779B1U)), m.mul(seed, m.cu(0x85EBCA6BU))));
        const int th = m.mul(m.cf(2.0 * 3.14159265358979323846), ra);
        const int rd = m.mul(m.cf(static_cast<double>(cfg.spatial_radius)), m.sqrtv(rr));
        const int nxf = m.clampf(m.add(sxf, m.mul(rd, m.cosv(th))), 0.0, static_cast<double>(cfg.width - 1U));
        const int nyf = m.clampf(m.add(syf, m.mul(rd, m.sinv(th))), 0.0, static_cast<double>(cfg.height - 1U));
        const int nbr = m.add(m.mul(m.castu(nyf), m.cu(cfg.width)), m.castu(nxf));
        const int nrb = m.mul(nbr, m.cu(kRestirGiStride));
        const int nb3 = m.mul(nbr, m.cu(3U));
        const int qX0 = lp(rin, nrb, 0U), qX1 = lp(rin, nrb, 1U), qX2 = lp(rin, nrb, 2U);
        const int qN0 = lp(rin, nrb, 3U), qN1 = lp(rin, nrb, 4U), qN2 = lp(rin, nrb, 5U);
        const int qL0 = lp(rin, nrb, 6U), qL1 = lp(rin, nrb, 7U), qL2 = lp(rin, nrb, 8U);
        const int qW = lp(rin, nrb, 9U), qM = lp(rin, nrb, 10U);
        // reconnect the neighbour's sample xs_q to THIS xv:
        int cvN = -1, csN = -1, d2N = -1;
        const int phN = phat_gi(qX0, qX1, qX2, qN0, qN1, qN2, qL0, qL1, qL2, cvN, csN, d2N);
        // neighbour's ORIGINAL geometry from its own xv_q:
        const int qvx = lp(pos, nb3, 0U), qvy = lp(pos, nb3, 1U), qvz = lp(pos, nb3, 2U);
        const int odx = m.sub(qX0, qvx), ody = m.sub(qX1, qvy), odz = m.sub(qX2, qvz);
        const int od2 = m.mx(m.dot3(odx, ody, odz, odx, ody, odz), m.cf(1.0e-12));
        const int oil = m.dvv(m.cf(1.0), g.unary(k::KOp::Sqrt, od2));
        const int csO = m.clamp0(m.neg(m.mul(m.dot3(qN0, qN1, qN2, odx, ody, odz), oil)));
        // Jacobian J = (cosθ_s^new · d_old²) / (cosθ_s^old · d_new²).
        const int J = m.dvv(m.mul(csN, od2), m.mx(m.mul(m.mx(csO, m.cf(1.0e-6)), d2N), m.cf(1.0e-12)));
        const int gate = m.mul(m.gt0(cvN), m.gt0(csN));
        const int wB = m.mul(m.mul(m.mul(m.mul(phN, qW), qM), J), gate);
        wsum = m.add(wsum, wB);
        M    = m.add(M, qM);
        const int xi   = detail::rt_hash01(g, m.g.binary(k::KOp::BitXor, m.mul(tid, m.cu(0x27D4EB2FU)), m.mul(m.add(frame, m.cu(j * 101U + 1U)), m.cu(0x165667B1U))));
        const int pick = g.select(g.binary(k::KOp::CmpLt, m.mul(xi, wsum), wB), m.cf(1.0), m.cf(0.0));
        oX0 = m.blend(pick, qX0, oX0); oX1 = m.blend(pick, qX1, oX1); oX2 = m.blend(pick, qX2, oX2);
        oN0 = m.blend(pick, qN0, oN0); oN1 = m.blend(pick, qN1, oN1); oN2 = m.blend(pick, qN2, oN2);
        oL0 = m.blend(pick, qL0, oL0); oL1 = m.blend(pick, qL1, oL1); oL2 = m.blend(pick, qL2, oL2);
        phO = m.blend(pick, phN, phO);
    }
    const int Wout = m.dvv(wsum, m.mul(M, m.mx(phO, m.cf(1.0e-12))));

    g.stmt_buffer_store(rout, m.add(rb, m.cu(0U)), oX0);  g.stmt_buffer_store(rout, m.add(rb, m.cu(1U)), oX1);  g.stmt_buffer_store(rout, m.add(rb, m.cu(2U)), oX2);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(3U)), oN0);  g.stmt_buffer_store(rout, m.add(rb, m.cu(4U)), oN1);  g.stmt_buffer_store(rout, m.add(rb, m.cu(5U)), oN2);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(6U)), oL0);  g.stmt_buffer_store(rout, m.add(rb, m.cu(7U)), oL1);  g.stmt_buffer_store(rout, m.add(rb, m.cu(8U)), oL2);
    g.stmt_buffer_store(rout, m.add(rb, m.cu(9U)), Wout); g.stmt_buffer_store(rout, m.add(rb, m.cu(10U)), M); g.stmt_buffer_store(rout, m.add(rb, m.cu(11U)), m.cf(0.0));

    k::KEntry e;
    e.stage = k::KStage::Compute; e.local_size[0] = cfg.local_size; e.kernel_body_begin = mark; e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// PASS 3/3 — SHADE the GI reservoir: L_indirect = (albedo/π)·Lo·cosθ_v·W. Buffers: pos (b1,3f), nrm (b2,3f), reservoir_in
// (b3,12f), radiance out (b4,3f).
[[nodiscard]] inline crd::kir::KEntry build_restir_gi_shade_kernel(crd::kir::KGraph& g, const RestirGiConfig& cfg)
{
    namespace k = crd::kir;
    const double a0 = static_cast<double>(cfg.albedo[0]) / 3.14159265358979323846;
    const double a1 = static_cast<double>(cfg.albedo[1]) / 3.14159265358979323846;
    const double a2 = static_cast<double>(cfg.albedo[2]) / 3.14159265358979323846;
    detail::GiMath m{g, 0.0, 0.0, 0.0, 0.0};

    const int pos = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nrm = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int rin = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int out = g.buffer_decl(k::DType::F32, 0, 4, true);

    const int  mark = g.kernel_stmt_mark();
    const int  tid  = m.add(m.mul(g.builtin(k::KBuiltin::WorkgroupIndex), m.cu(cfg.local_size)), g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int  b3   = m.mul(tid, m.cu(3U));
    const int  rb   = m.mul(tid, m.cu(kRestirGiStride));
    const auto lp   = [&](int buf, int base, crd::u32 c) { return g.buffer_load(buf, m.add(base, m.cu(c))); };
    const int  Nx = lp(nrm, b3, 0U), Ny = lp(nrm, b3, 1U), Nz = lp(nrm, b3, 2U);
    const int  Px = lp(pos, b3, 0U), Py = lp(pos, b3, 1U), Pz = lp(pos, b3, 2U);
    const int  xs0 = lp(rin, rb, 0U), xs1 = lp(rin, rb, 1U), xs2 = lp(rin, rb, 2U);
    const int  Lo0 = lp(rin, rb, 6U), Lo1 = lp(rin, rb, 7U), Lo2 = lp(rin, rb, 8U);
    const int  W   = lp(rin, rb, 9U);
    const int  dx = m.sub(xs0, Px), dy = m.sub(xs1, Py), dz = m.sub(xs2, Pz);
    const int  il = m.dvv(m.cf(1.0), g.unary(k::KOp::Sqrt, m.mx(m.dot3(dx, dy, dz, dx, dy, dz), m.cf(1.0e-12))));
    const int  cv = m.clamp0(m.mul(m.dot3(Nx, Ny, Nz, dx, dy, dz), il));
    const int  sh = m.mul(m.mul(cv, W), m.cf(1.0));
    g.stmt_buffer_store(out, m.add(b3, m.cu(0U)), m.mul(m.mul(m.cf(a0), Lo0), sh));
    g.stmt_buffer_store(out, m.add(b3, m.cu(1U)), m.mul(m.mul(m.cf(a1), Lo1), sh));
    g.stmt_buffer_store(out, m.add(b3, m.cu(2U)), m.mul(m.mul(m.cf(a2), Lo2), sh));

    k::KEntry e;
    e.stage = k::KStage::Compute; e.local_size[0] = cfg.local_size; e.kernel_body_begin = mark; e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::rt
