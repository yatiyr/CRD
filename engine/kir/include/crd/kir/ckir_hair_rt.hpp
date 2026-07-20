#pragma once

// ckir_hair_rt.hpp — D-007 B18-f: the PATH-TRACED HAIR SWATCH — hardware curve traversal driving the B18-a BCSDF at
// film sampling rates. This is the renderer the RT strand tier exists for.
//
// ⭐ WHY THIS EXISTS, AND WHY THE RASTER PATH CANNOT DO IT. A groom puts ~148 strands over every pixel. A deferred
//    rasteriser resolves ONE of them per pixel (or 4 under 2x2 supersampling), so neighbouring pixels commit to
//    DIFFERENT winners out of the same 148 candidates. The result is not noise, it is streaking: 3-6 px ribbons that
//    read as impasto brush strokes. No amount of filtering fixes it, because the information was discarded at
//    visibility. Film shoots hundreds of rays per pixel and lets each one hit a real fibre; the pixel then holds the
//    AVERAGE of the fibre mass, which is what hair actually looks like. That is the whole difference, and it is only
//    available once strands are a traceable primitive — which is what B18-f landed.
//
// WHAT THIS DOES PER SAMPLE — a full multi-bounce path with next-event estimation:
//   · a jittered camera ray → TraceRayCurves against the procedural swept-sphere BLAS;
//   · at each of `bounces` hits: the fibre frame rebuilt from the hit (interpolated tangent; h from the radial
//     offset in the segment's own frame), the Chiang BCSDF evaluated PER COLOUR CHANNEL (σₐ is spectral — that is
//     where hair colour comes from, not a tint);
//   · NEXT-EVENT ESTIMATION per light: a shadow ray MARCHED through up to `shadow_steps` fibres, multiplying the
//     per-channel transmittance exp(−σₐ·d) at each crossing. A fibre is a FILTER, not an occluder — so the shadow
//     is COLOURED, and the light surviving deep inside pale hair comes out gold. A binary shadow blacks the interior;
//   · an INDIRECT bounce (uniform-sphere sample, weighted f·cosθ·4π) continues the path. The interior of a groom is
//     almost entirely bounces 2+, so this is what makes hair read as a translucent VOLUME rather than a silhouette.
//   · escaped rays gather a studio environment; an analytic ground plane receives the groom's real contact shadow.
//
// The sample loop is INSIDE the kernel and accumulates through an out-buffer RMW (the pattern ckir_rt.hpp's path
// tracer uses). Callers split the total sample count across several dispatches with different `seed` values and sum
// the passes host-side — a single dispatch large enough for a converged frame would trip the Windows GPU watchdog.
//
// ⛔ THIS IS A FILM / OFFLINE RENDERER as configured: ~194 ms per full-frame sample on a 4070 Ti SUPER at 1400×1000
//    with 3 bounces + 3-light 8-step transmittance shadows (docs/bench/2026-07-20-hair-rt-swatch-perf.md). Real-time
//    hair is the B18-c DOM-shadow + B14-denoiser path, not this one — this is the reference the raster tier is
//    measured against.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair.hpp>
#include <crd/kir/ckir_lss.hpp>
#include <crd/kir/ckir_rt.hpp>

#include <crd/math/cmath.hpp>

namespace crd::kir::hairrt
{

struct RtHairSwatchConfig
{
    int width  = 1280;
    int height = 960;
    int spp    = 16; // samples PER DISPATCH — total = spp × passes, summed host-side

    // camera basis, precomputed host-side (the kernel only jitters within the pixel)
    double cam_pos[3]  = {0.0, 0.5, 2.6};
    double cam_fwd[3]  = {0.0, 0.0, -1.0};
    double cam_right[3] = {1.0, 0.0, 0.0};
    double cam_up[3]   = {0.0, 1.0, 0.0};
    double tan_half_fov = 0.36397; // tan(20°)

    // three lights: direction TO the light (unit), colour, and the frame used to jitter within the source's disc
    double light_dir[3][3]   = {{-0.45, 0.55, 0.70}, {0.30, 0.25, -0.92}, {0.75, -0.20, 0.35}};
    double light_col[3][3]   = {{1.00, 0.94, 0.86}, {1.10, 0.86, 0.62}, {0.22, 0.26, 0.34}};
    double light_t[3][3]     = {{1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}};
    double light_b[3][3]     = {{0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}};
    double light_radius[3]   = {0.09, 0.05, 0.30}; // angular radius — larger = softer shadow
    int    nlights           = 3;

    // fibre optics. σₐ is PER CHANNEL: hair colour is absorption, so the BCSDF runs three times.
    double sigma_a[3] = {0.06, 0.16, 0.42};
    double eta        = 1.55;
    double beta_m     = 0.24;
    double beta_n     = 0.28;
    double alpha_deg  = 2.8;

    double bg[3]      = {0.020, 0.024, 0.032};
    double exposure   = 1.0;
    double shadow_tmin = 0.006; // must clear the fibre the ray started on (a few strand radii)
    // How many fibres a shadow ray steps THROUGH before giving up. Past this depth the residual light is negligible
    // for dark hair; the remaining error is a slight over-darkening in the deepest interior of pale hair.
    int    shadow_steps          = 8;
    int    shadow_steps_indirect = 3;  // an indirect bounce does not need the direct path's shadow precision
    int    bounces               = 3;  // 1 = direct only; the interior of a groom is almost entirely bounces 2+
    double ray_tmin              = 1.0e-5;
    double throughput_clamp      = 1.5; // a firefly guard: the R lobe is sharp enough to spike a uniform-sphere weight
    // the studio ENVIRONMENT an escaped ray gathers. With none, indirect rays collect nothing and GI is exactly zero.
    double env_lo[3] = {0.018, 0.020, 0.024};
    double env_hi[3] = {0.105, 0.112, 0.130};
    double shadow_normal_offset = 4.0; // in fibre radii — must clear the strand's own overlapping end caps
    // the GROUND: a gray plane the swatch stands on, so the groom has a contact shadow and a sense of scale
    bool   ground              = true;
    double plane_y             = 0.0;
    double ground_albedo[3]    = {0.34, 0.34, 0.35};
    int    ground_shadow_steps = 5;
    double fibre_depth  = 2.0; // optical path of one crossing, in sigma_a units (about a diameter)
    int    segments    = 1;     // for the miss-index clamp
    int    local_size  = 64;
    // ── DIAGNOSTIC AOV. 0 = beauty. 1 = h probe: R accumulates |h| ONLY on hits, G counts the hits, B is unused, and
    //    the plane + environment are suppressed entirely. The host then reads R/G as mean|h| CONDITIONED ON A HIT and
    //    G/spp as coverage. Compositing a background into an AOV makes a partially-covered fibre indistinguishable
    //    from a high h, which is exactly how an earlier version of this probe produced a confident wrong answer.
    int    debug_aov   = 0;
};

// Buffers: b0 AS · b1 segments (F32, 8/seg) · b2 seed (F32, 1 — the pass index) · b3 out (F32, 3/pixel, SUMMED)
[[nodiscard]] inline KEntry build_rt_hair_swatch_kernel(KGraph& g, const RtHairSwatchConfig& cfg)
{
    using namespace crd::kir::lss::detail;
    namespace hd = crd::kir::hair::detail;

    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const auto  mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto  uadd = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  umul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int as    = g.accel_struct_decl(0, 0);
    const int seg_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int sed_b = g.buffer_decl(DType::F32, 0, 2, false);
    const int tan_b = g.buffer_decl(DType::F32, 0, 4, false); // 6 floats/segment: the endpoint tangents
    const int out_b = g.buffer_decl(DType::F32, 0, 3, true);

    const int mark = g.kernel_stmt_mark();
    const int tid  = uadd(umul(g.builtin(KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(cfg.local_size))),
                         g.builtin(KBuiltin::LocalInvocationIndex));
    const int seed = g.buffer_load(sed_b, cu(0));
    g.stmt_materialize(seed);

    const int w    = cu(static_cast<crd::u32>(cfg.width));
    const int px   = g.binary(KOp::Mod, tid, w);
    const int py   = g.binary(KOp::Div, tid, w);
    const int pxf  = g.cast(px, DType::F32);
    const int pyf  = g.cast(py, DType::F32);
    const int b3   = umul(tid, cu(3U));

    // the same integer hash ckir_rt.hpp uses for its path samples — one decorrelated stream per use site
    const auto hash01 = [&](int x) { return crd::kir::rt::detail::rt_hash01(g, x); };

    g.stmt_buffer_store(out_b, uadd(b3, cu(0U)), ks(0.0));
    g.stmt_buffer_store(out_b, uadd(b3, cu(1U)), ks(0.0));
    g.stmt_buffer_store(out_b, uadd(b3, cu(2U)), ks(0.0));

    const int floop = g.stmt_for_begin(cu(static_cast<crd::u32>(cfg.spp)));
    const int sidx  = g.kernel_loop_var(floop);
    const int sbase = uadd(umul(g.cast(seed, DType::U32), cu(0x9E3779B9U)), umul(sidx, cu(0x85157AF5U)));

    // ── the jittered camera ray ────────────────────────────────────────────────────────────────────────────────────
    const int jx = hash01(g.binary(KOp::BitXor, umul(tid, cu(0x632BE5ABU)), sbase));
    const int jy = hash01(g.binary(KOp::BitXor, umul(tid, cu(0xC2B2AE35U)), uadd(sbase, cu(0x9E3779B9U))));
    const double aspect = static_cast<double>(cfg.width) / static_cast<double>(cfg.height);
    const int ndx = hd::mul(g, hd::sub(g, hd::dv(g, hd::add(g, pxf, jx), ks(0.5 * cfg.width)), ks(1.0)),
                            ks(aspect * cfg.tan_half_fov));
    const int ndy = hd::mul(g, hd::sub(g, ks(1.0), hd::dv(g, hd::add(g, pyf, jy), ks(0.5 * cfg.height))),
                            ks(cfg.tan_half_fov));
    const auto axis3 = [&](const double v[3], int s) {
        return V3{hd::mul(g, ks(v[0]), s), hd::mul(g, ks(v[1]), s), hd::mul(g, ks(v[2]), s)};
    };
    const V3 rr = axis3(cfg.cam_right, ndx);
    const V3 uu = axis3(cfg.cam_up, ndy);
    const V3 rd_raw{hd::add(g, hd::add(g, ks(cfg.cam_fwd[0]), rr.x), uu.x),
                    hd::add(g, hd::add(g, ks(cfg.cam_fwd[1]), rr.y), uu.y),
                    hd::add(g, hd::add(g, ks(cfg.cam_fwd[2]), rr.z), uu.z)};
    const int rdl  = mx(hd::safe_sqrt(g, dot3(g, rd_raw, rd_raw)), ks(1.0e-20));
    const int rdi  = hd::dv(g, ks(1.0), rdl);
    const V3  rd{hd::mul(g, rd_raw.x, rdi), hd::mul(g, rd_raw.y, rdi), hd::mul(g, rd_raw.z, rdi)};
    const V3  ro{ks(cfg.cam_pos[0]), ks(cfg.cam_pos[1]), ks(cfg.cam_pos[2])};
    // ── THE PATH ─────────────────────────────────────────────────────────────────────────────────────────────────
    // NEXT-EVENT ESTIMATION for the sharp direct lighting, plus an INDIRECT bounce for everything else. The split is
    // the whole reason this converges: the BCSDF's R lobe is only a couple of degrees wide, so sampling it blindly
    // would almost never find a light — NEE handles that exactly. The indirect ray does not need that precision, so it
    // samples the sphere UNIFORMLY and pays for it in the weight (f·cosθ·4π). Unbiased, trivial to evaluate, and its
    // variance is the kind extra samples actually fix.
    //
    // ⭐ THIS IS WHAT MAKES HAIR LOOK LIKE HAIR. Direct lighting alone renders a groom as a silhouette: inside the mass
    //   every point is shadowed from every lamp, so the interior goes black (dark hair) or flat (pale hair). In reality
    //   most of the light inside hair has bounced between fibres several times, and because each bounce is filtered by
    //   σₐ that light is deeply SATURATED — the warm glow inside blonde hair is light that has passed through a dozen
    //   fibres. The throughput here carries exactly that: it is multiplied by the fibre's own coloured response at
    //   every bounce, so pale hair keeps bouncing and dark hair dies after one, which is precisely the difference
    //   between the two in life.
    constexpr double kFar = 1.0e4;
    constexpr double kTau = 6.28318530717958647692;

    V3  po = ro;
    V3  pd = rd;
    int thr[3] = {ks(1.0), ks(1.0), ks(1.0)};
    int radc[3] = {ks(0.0), ks(0.0), ks(0.0)};
    int miss0   = -1;
    int dbg_h   = -1;
    int dbg_hit = -1;

    for (int bounce = 0; bounce < cfg.bounces; ++bounce)
    {
        const int    nshadow = bounce == 0 ? cfg.shadow_steps : cfg.shadow_steps_indirect;
        const crd::u32 bseed = static_cast<crd::u32>(bounce) * 0x9E3779B1U + 0x7F4A7C15U;

        const auto hit = g.trace_ray_curves(as, seg_b, po.x, po.y, po.z, pd.x, pd.y, pd.z, ks(cfg.ray_tmin), ks(kFar));
        const int  miss = g.binary(KOp::CmpGe, hit.t, ks(kFar - 1.0));
        if (bounce == 0) { miss0 = miss; }

        // ── the ENVIRONMENT the escaped ray gathers. Without one, indirect rays leave and collect nothing, and GI
        //    contributes exactly zero — a studio has walls and a bright ceiling, and hair picks that up everywhere its
        //    own lamps do not reach.
        if (cfg.debug_aov == 0)
        {
            const int up = hd::mul(g, hd::add(g, pd.y, ks(1.0)), ks(0.5));
            for (int c = 0; c < 3; ++c)
            {
                const int e = hd::add(g, ks(cfg.env_lo[c]), hd::mul(g, ks(cfg.env_hi[c] - cfg.env_lo[c]), up));
                radc[c] = hd::add(g, radc[c], g.select(miss, hd::mul(g, thr[c], e), ks(0.0)));
            }
        }

        // the hit segment (index clamped on a miss — an out-of-range load is silent corruption on the device)
        const int prim_c = g.binary(KOp::Min, hit.prim, cu(static_cast<crd::u32>(cfg.segments > 0 ? cfg.segments - 1 : 0)));
        const int sidx8  = g.select(miss, cu(0), umul(prim_c, cu(8U)));
        const auto sl = [&](int k) {
            const int v = g.buffer_load(seg_b, uadd(sidx8, cu(static_cast<crd::u32>(k))));
            g.stmt_materialize(v);
            return v;
        };
        const V3  pa{sl(0), sl(1), sl(2)};
        const int ra = sl(3);
        const V3  pb{sl(4), sl(5), sl(6)};
        const int rb = sl(7);

        // ⛔⛔ THE TANGENT MUST BE INTERPOLATED, NOT TAKEN FROM THE SEGMENT. normalize(pb - pa) is FLAT shading: the
        //     tangent steps at every segment boundary, and because the R lobe is razor-sharp in θ (β_m ≈ 0.22) each
        //     step BREAKS the specular — the highlight comes out chopped into bright dashes one segment long instead
        //     of travelling down the fibre. Same problem and same fix as vertex normals on a mesh.
        const int tb_i = umul(g.select(miss, cu(0), prim_c), cu(6U));
        const auto tl  = [&](int k) {
            const int v = g.buffer_load(tan_b, uadd(tb_i, cu(static_cast<crd::u32>(k))));
            g.stmt_materialize(v);
            return v;
        };
        const V3 t0{tl(0), tl(1), tl(2)};
        const V3 t1{tl(3), tl(4), tl(5)};
        const V3 traw{hd::add(g, t0.x, hd::mul(g, hd::sub(g, t1.x, t0.x), hit.u)),
                      hd::add(g, t0.y, hd::mul(g, hd::sub(g, t1.y, t0.y), hit.u)),
                      hd::add(g, t0.z, hd::mul(g, hd::sub(g, t1.z, t0.z), hit.u))};
        const int tlen = mx(hd::safe_sqrt(g, dot3(g, traw, traw)), ks(1.0e-20));
        const int tinv = hd::dv(g, ks(1.0), tlen);
        const V3  tang{hd::mul(g, traw.x, tinv), hd::mul(g, traw.y, tinv), hd::mul(g, traw.z, tinv)};
        const int rad  = mx(hd::add(g, ra, hd::mul(g, hd::sub(g, rb, ra), hit.u)), ks(1.0e-20));

        const V3 hp{hd::add(g, po.x, hd::mul(g, hit.t, pd.x)), hd::add(g, po.y, hd::mul(g, hit.t, pd.y)),
                    hd::add(g, po.z, hd::mul(g, hit.t, pd.z))};
        const V3 wo{g.unary(KOp::Neg, pd.x), g.unary(KOp::Neg, pd.y), g.unary(KOp::Neg, pd.z)};

        // h = signed perpendicular miss distance / radius, along normalize(T × ωo) — exact by construction, where an
        // h recovered from the surface normal degenerates at grazing angles.
        const V3  cr0{hd::sub(g, hd::mul(g, tang.y, wo.z), hd::mul(g, tang.z, wo.y)),
                     hd::sub(g, hd::mul(g, tang.z, wo.x), hd::mul(g, tang.x, wo.z)),
                     hd::sub(g, hd::mul(g, tang.x, wo.y), hd::mul(g, tang.y, wo.x))};
        const int cl  = mx(hd::safe_sqrt(g, dot3(g, cr0, cr0)), ks(1.0e-20));
        const int ci  = hd::dv(g, ks(1.0), cl);
        const V3  bn{hd::mul(g, cr0.x, ci), hd::mul(g, cr0.y, ci), hd::mul(g, cr0.z, ci)};
        // ⛔⛔ h MUST BE DECOMPOSED IN THE FRAME THE INTERSECTION ACTUALLY USED. h is the ray's perpendicular offset
        //     from the fibre axis in radius units, and it is what γo = asin(h) — the axis the whole BCSDF turns on —
        //     is built from. The radial offset of a hit is radial with respect to the SEGMENT's axis, because that is
        //     the cylinder the intersector solved against. Projecting it onto a basis built from the INTERPOLATED
        //     tangent mixes two different axes: the leftover axial component leaks into the projection, and dividing
        //     by a 68 µm radius turns a sub-micron inconsistency into a value that saturates the clamp.
        //
        //     Measured: |h| should be roughly UNIFORM over [0,1] across a fibre's width; with the mismatched frames it
        //     piled up at 1 (65k pixels in 0.9-1.0 against ~5k in 0.5-0.6, nothing at all below 0.5). Because f varies
        //     steeply with h near the grazing limit, that pinned every fibre into a narrow, wildly-varying slice of the
        //     lobe — which is what printed as beads along the strands.
        //
        //     So: the geometric frame for h, the smooth tangent for the θ angles. The smooth tangent is right where
        //     continuity matters and the sensitivity is mild; h needs consistency with the intersector instead.
        const V3  seg_v = sub3(g, pb, pa);
        const int sgl   = mx(hd::safe_sqrt(g, dot3(g, seg_v, seg_v)), ks(1.0e-20));
        const int sgi_  = hd::dv(g, ks(1.0), sgl);
        const V3  sdir{hd::mul(g, seg_v.x, sgi_), hd::mul(g, seg_v.y, sgi_), hd::mul(g, seg_v.z, sgi_)};
        const V3  gcr{hd::sub(g, hd::mul(g, sdir.y, wo.z), hd::mul(g, sdir.z, wo.y)),
                     hd::sub(g, hd::mul(g, sdir.z, wo.x), hd::mul(g, sdir.x, wo.z)),
                     hd::sub(g, hd::mul(g, sdir.x, wo.y), hd::mul(g, sdir.y, wo.x))};
        const int gcl = mx(hd::safe_sqrt(g, dot3(g, gcr, gcr)), ks(1.0e-20));
        const int gci = hd::dv(g, ks(1.0), gcl);
        const V3  gbn{hd::mul(g, gcr.x, gci), hd::mul(g, gcr.y, gci), hd::mul(g, gcr.z, gci)};
        const V3  axp{hd::add(g, pa.x, hd::mul(g, seg_v.x, hit.u)), hd::add(g, pa.y, hd::mul(g, seg_v.y, hit.u)),
                     hd::add(g, pa.z, hd::mul(g, seg_v.z, hit.u))};
        const V3  roff = sub3(g, hp, axp);
        const int hh   = mx(g.binary(KOp::Min, hd::dv(g, dot3(g, roff, gbn), rad), ks(1.0)), ks(-1.0));

        // DIAGNOSTIC AOV (kept: it found the f32-cancellation bug and is the validated instrument for the class).
        // R accumulates |h| ONLY on hits, G counts hits, so the host reads R/G as mean|h| CONDITIONED ON A HIT — a
        // uniform cylinder must give 0.5. Suppressing the plane + environment (below) keeps a partially-covered pixel
        // from masquerading as a high h; that blend is exactly how an earlier version of this probe produced a
        // confident wrong answer, so the instrument stays honest by construction.
        if (cfg.debug_aov == 1 && bounce == 0)
        {
            dbg_h   = g.select(miss, ks(0.0), g.unary(KOp::Abs, hh));
            dbg_hit = g.select(miss, ks(0.0), ks(1.0));
        }

        // fibre frame: x = T, y = ωo's axis-perpendicular part (so φo ≡ 0), z = T × y
        const int wot = dot3(g, wo, tang);
        const V3  wop{hd::sub(g, wo.x, hd::mul(g, tang.x, wot)), hd::sub(g, wo.y, hd::mul(g, tang.y, wot)),
                     hd::sub(g, wo.z, hd::mul(g, tang.z, wot))};
        const int pl  = mx(hd::safe_sqrt(g, dot3(g, wop, wop)), ks(1.0e-20));
        const int pi_ = hd::dv(g, ks(1.0), pl);
        const V3  fy{hd::mul(g, wop.x, pi_), hd::mul(g, wop.y, pi_), hd::mul(g, wop.z, pi_)};
        const V3  fz{hd::sub(g, hd::mul(g, tang.y, fy.z), hd::mul(g, tang.z, fy.y)),
                    hd::sub(g, hd::mul(g, tang.z, fy.x), hd::mul(g, tang.x, fy.z)),
                    hd::sub(g, hd::mul(g, tang.x, fy.y), hd::mul(g, tang.y, fy.x))};

        // the shadow/continuation origin, pushed off the surface ALONG THE NORMAL. A tmin cannot do this job:
        // consecutive segments share an endpoint and their cap spheres overlap, so a ray leaving the surface can
        // re-enter the strand it just left — and one leaving nearly ALONG the fibre grazes its neighbours for an
        // unbounded distance, which no epsilon covers.
        const int dvt = dot3(g, roff, tang);
        const V3  nrw{hd::sub(g, roff.x, hd::mul(g, tang.x, dvt)), hd::sub(g, roff.y, hd::mul(g, tang.y, dvt)),
                     hd::sub(g, roff.z, hd::mul(g, tang.z, dvt))};
        // ⛔⛔ THE SURFACE NORMAL DEGENERATES AT AN END CAP, AND THE GUARD MUST BE A FALLBACK, NOT AN EPSILON. A cap
        //     hit lands on a HEMISPHERE, so the radial offset points largely ALONG the tangent; stripping the tangent
        //     component leaves a near-zero vector. Clamping its length to 1e-20 and dividing does not rescue it — it
        //     AMPLIFIES the remaining numerical dust into a unit vector pointing in an essentially random direction.
        //     The shadow ray then starts four radii away in that random direction, sometimes straight back inside the
        //     strand it just left. Caps sit at every joint, so an isolated fibre picks up a dark BEAD at every joint —
        //     a dashed hair. `bn` is a genuine substitute: it is unit, and perpendicular to the tangent by
        //     construction, so pushing along it always clears the tube.
        const int nl2 = dot3(g, nrw, nrw);
        const int deg = g.binary(KOp::CmpLt, nl2, hd::mul(g, hd::sq(g, rad), ks(1.0e-4)));
        const int nl  = mx(hd::safe_sqrt(g, nl2), ks(1.0e-20));
        const int ni  = hd::dv(g, ks(1.0), nl);
        const V3  nrm{g.select(deg, gbn.x, hd::mul(g, nrw.x, ni)), g.select(deg, gbn.y, hd::mul(g, nrw.y, ni)),
                     g.select(deg, gbn.z, hd::mul(g, nrw.z, ni))};
        const int off = hd::mul(g, rad, ks(cfg.shadow_normal_offset));
        const V3  sorg{hd::add(g, hp.x, hd::mul(g, nrm.x, off)), hd::add(g, hp.y, hd::mul(g, nrm.y, off)),
                       hd::add(g, hp.z, hd::mul(g, nrm.z, off))};

        // ── NEXT-EVENT ESTIMATION ────────────────────────────────────────────────────────────────────────────────
        for (int L = 0; L < cfg.nlights; ++L)
        {
            const int lu1 = hash01(g.binary(KOp::BitXor, umul(tid, cu(0x27D4EB2FU + static_cast<crd::u32>(L) * 0x1000193U)),
                                            uadd(sbase, cu(0x165667B1U + bseed))));
            const int lu2 = hash01(g.binary(KOp::BitXor, umul(tid, cu(0x9E3779B1U + static_cast<crd::u32>(L) * 0x85EBCA6BU)),
                                            uadd(sbase, cu(0x27220A95U + bseed))));
            const int lrr = hd::mul(g, ks(cfg.light_radius[L]), hd::safe_sqrt(g, lu1));
            const int lph = hd::mul(g, ks(kTau), lu2);
            const int loc = hd::mul(g, lrr, g.unary(KOp::Cos, lph));
            const int los = hd::mul(g, lrr, g.unary(KOp::Sin, lph));
            const V3  lraw{hd::add(g, hd::add(g, ks(cfg.light_dir[L][0]), hd::mul(g, ks(cfg.light_t[L][0]), loc)), hd::mul(g, ks(cfg.light_b[L][0]), los)),
                           hd::add(g, hd::add(g, ks(cfg.light_dir[L][1]), hd::mul(g, ks(cfg.light_t[L][1]), loc)), hd::mul(g, ks(cfg.light_b[L][1]), los)),
                           hd::add(g, hd::add(g, ks(cfg.light_dir[L][2]), hd::mul(g, ks(cfg.light_t[L][2]), loc)), hd::mul(g, ks(cfg.light_b[L][2]), los))};
            const int ll  = mx(hd::safe_sqrt(g, dot3(g, lraw, lraw)), ks(1.0e-20));
            const int li  = hd::dv(g, ks(1.0), ll);
            const V3  wi{hd::mul(g, lraw.x, li), hd::mul(g, lraw.y, li), hd::mul(g, lraw.z, li)};

            // march the shadow ray THROUGH fibres, accumulating per-channel transmittance. A binary shadow is wrong
            // for hair: a fibre is a filter, not an occluder, and because σₐ is spectral the surviving light is
            // COLOURED — which is why the light deep inside blonde hair comes out gold.
            int trc[3] = {ks(1.0), ks(1.0), ks(1.0)};
            V3  so = sorg;
            for (int step = 0; step < nshadow; ++step)
            {
                const auto sh  = g.trace_ray_curves(as, seg_b, so.x, so.y, so.z, wi.x, wi.y, wi.z, ks(cfg.shadow_tmin), ks(kFar));
                const int  occ = g.binary(KOp::CmpLt, sh.t, ks(kFar - 1.0));
                for (int c = 0; c < 3; ++c)
                {
                    const double tfib = crd::math::exp(-cfg.sigma_a[c] * cfg.fibre_depth);
                    trc[c] = hd::mul(g, trc[c], g.select(occ, ks(tfib), ks(1.0)));
                }
                if (step + 1 < nshadow)
                {
                    const int adv = g.select(occ, hd::add(g, sh.t, ks(cfg.shadow_tmin)), ks(0.0));
                    so = V3{hd::add(g, so.x, hd::mul(g, wi.x, adv)), hd::add(g, so.y, hd::mul(g, wi.y, adv)),
                            hd::add(g, so.z, hd::mul(g, wi.z, adv))};
                }
            }

            const int sin_ti = dot3(g, wi, tang);
            const int cos_ti = hd::safe_sqrt(g, hd::sub(g, ks(1.0), hd::sq(g, sin_ti)));
            const int phi_i  = g.binary(KOp::Atan2, dot3(g, wi, fz), dot3(g, wi, fy));
            for (int c = 0; c < 3; ++c)
            {
                const int f = hair::hair_bcsdf_eval_angles(g, wot, pl, ks(0.0), sin_ti, cos_ti, phi_i, hh, ks(cfg.eta),
                                                           ks(cfg.sigma_a[c]), ks(cfg.beta_m), ks(cfg.beta_n),
                                                           ks(cfg.alpha_deg));
                // L_o = ∫ f·L_i·cosθi dω — the cosine is part of the fibre's measure, not decoration.
                const int d = hd::mul(g, hd::mul(g, hd::mul(g, f, cos_ti), ks(cfg.light_col[L][c])), trc[c]);
                radc[c] = hd::add(g, radc[c], g.select(miss, ks(0.0), hd::mul(g, thr[c], d)));
            }
        }

        // ── THE INDIRECT BOUNCE ──────────────────────────────────────────────────────────────────────────────────
        if (bounce + 1 < cfg.bounces)
        {
            const int bu1 = hash01(g.binary(KOp::BitXor, umul(tid, cu(0x45D9F3BU)), uadd(sbase, cu(0x51ED2701U + bseed))));
            const int bu2 = hash01(g.binary(KOp::BitXor, umul(tid, cu(0x119DE1F3U)), uadd(sbase, cu(0x9E3779B9U + bseed))));
            // uniform on the sphere: z = 1 − 2u₁, and the weight below carries the 4π that makes it unbiased.
            const int bz  = hd::sub(g, ks(1.0), hd::mul(g, ks(2.0), bu1));
            const int brr = hd::safe_sqrt(g, hd::sub(g, ks(1.0), hd::sq(g, bz)));
            const int bph = hd::mul(g, ks(kTau), bu2);
            const V3  nd{hd::mul(g, brr, g.unary(KOp::Cos, bph)), hd::mul(g, brr, g.unary(KOp::Sin, bph)), bz};

            const int nsin = dot3(g, nd, tang);
            const int ncos = hd::safe_sqrt(g, hd::sub(g, ks(1.0), hd::sq(g, nsin)));
            const int nphi = g.binary(KOp::Atan2, dot3(g, nd, fz), dot3(g, nd, fy));
            for (int c = 0; c < 3; ++c)
            {
                const int f = hair::hair_bcsdf_eval_angles(g, wot, pl, ks(0.0), nsin, ncos, nphi, hh, ks(cfg.eta),
                                                           ks(cfg.sigma_a[c]), ks(cfg.beta_m), ks(cfg.beta_n),
                                                           ks(cfg.alpha_deg));
                // weight = f·cosθ / pdf, pdf = 1/4π. A miss retires the path by zeroing the throughput — the
                // branchless equivalent of breaking out of the loop.
                const int wgt = hd::mul(g, hd::mul(g, f, ncos), ks(2.0 * kTau));
                thr[c] = g.select(miss, ks(0.0), hd::mul(g, thr[c], g.binary(KOp::Min, wgt, ks(cfg.throughput_clamp))));
                g.stmt_materialize(thr[c]);
            }
            const int eoff = hd::mul(g, rad, ks(cfg.shadow_normal_offset));
            po = V3{hd::add(g, hp.x, hd::mul(g, nd.x, eoff)), hd::add(g, hp.y, hd::mul(g, nd.y, eoff)),
                    hd::add(g, hp.z, hd::mul(g, nd.z, eoff))};
            pd = nd;
            g.stmt_materialize(po.x); g.stmt_materialize(po.y); g.stmt_materialize(po.z);
            g.stmt_materialize(pd.x); g.stmt_materialize(pd.y); g.stmt_materialize(pd.z);
        }
        for (int c = 0; c < 3; ++c) { g.stmt_materialize(radc[c]); }
    }

    // ── THE GROUND PLANE (y = plane_y), analytic ──────────────────────────────────────────────────────────────────
    // Not in the acceleration structure: one ray-plane solve is cheaper and exact, and the BLAS holds curves only. It
    // earns its place by receiving the groom's CONTACT SHADOW — a swatch floating against a flat backdrop has no sense
    // of scale or of how much light the mass is actually stopping. The shadow ray runs against the same fibres, so it
    // is real occlusion rather than a projected blob.
    int planec[3] = {ks(0.0), ks(0.0), ks(0.0)};
    if (cfg.ground && cfg.debug_aov == 0)
    {
        const int dy   = g.select(g.binary(KOp::CmpLt, g.unary(KOp::Abs, rd.y), ks(1.0e-6)), ks(-1.0e-6), rd.y);
        const int pt   = hd::dv(g, hd::sub(g, ks(cfg.plane_y), ro.y), dy);
        const int phit = g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, pt, ks(1.0e-3)),
                                  g.binary(KOp::CmpLt, pt, ks(kFar - 1.0)));
        const V3  pp{hd::add(g, ro.x, hd::mul(g, pt, rd.x)), ks(cfg.plane_y + 1.0e-4),
                    hd::add(g, ro.z, hd::mul(g, pt, rd.z))};
        int acc[3] = {ks(0.0), ks(0.0), ks(0.0)};
        for (int L = 0; L < cfg.nlights; ++L)
        {
            const V3  wl{ks(cfg.light_dir[L][0]), ks(cfg.light_dir[L][1]), ks(cfg.light_dir[L][2])};
            const int ndl = g.binary(KOp::Max, wl.y, ks(0.0));
            int       ptr[3] = {ks(1.0), ks(1.0), ks(1.0)};
            V3        so = pp;
            for (int step = 0; step < cfg.ground_shadow_steps; ++step)
            {
                const auto sh  = g.trace_ray_curves(as, seg_b, so.x, so.y, so.z, wl.x, wl.y, wl.z, ks(cfg.shadow_tmin), ks(kFar));
                const int  occ = g.binary(KOp::CmpLt, sh.t, ks(kFar - 1.0));
                for (int c = 0; c < 3; ++c)
                {
                    const double tfib = crd::math::exp(-cfg.sigma_a[c] * cfg.fibre_depth);
                    ptr[c] = hd::mul(g, ptr[c], g.select(occ, ks(tfib), ks(1.0)));
                }
                if (step + 1 < cfg.ground_shadow_steps)
                {
                    const int adv = g.select(occ, hd::add(g, sh.t, ks(cfg.shadow_tmin)), ks(0.0));
                    so = V3{hd::add(g, so.x, hd::mul(g, wl.x, adv)), hd::add(g, so.y, hd::mul(g, wl.y, adv)),
                            hd::add(g, so.z, hd::mul(g, wl.z, adv))};
                }
            }
            for (int c = 0; c < 3; ++c)
            {
                acc[c] = hd::add(g, acc[c], hd::mul(g, hd::mul(g, hd::mul(g, ndl, ks(cfg.light_col[L][c])), ptr[c]),
                                                    ks(cfg.ground_albedo[c] * 0.3183098861837907)));
            }
        }
        // the plane also picks up the sky it can see
        for (int c = 0; c < 3; ++c)
        {
            acc[c] = hd::add(g, acc[c], ks(cfg.ground_albedo[c] * cfg.env_hi[c] * 0.5));
            planec[c] = g.select(phit, acc[c], ks(0.0));
        }
    }

    // The path already added the environment on its own miss; the plane replaces that where the PRIMARY ray missed
    // the hair and struck the ground instead.
    // ⛔ THE PLANE IS ONLY VISIBLE WHERE THE HAIR IS NOT. Adding it whenever the primary ray reaches the ground —
    //    ignoring whether a fibre was in front of it — stacks the full lit-plane value onto EVERY hair pixel. The
    //    groom then washes out to a flat achromatic white, because the added term carries no σₐ and so destroys
    //    exactly the colour the BCSDF worked to produce. It reads as a translucent phantom.
    {
        for (int c = 0; c < 3; ++c) { radc[c] = g.select(miss0, hd::add(g, radc[c], planec[c]), radc[c]); }
    }
    if (cfg.debug_aov == 1) { radc[0] = dbg_h; radc[1] = dbg_hit; radc[2] = ks(0.0); }

    g.stmt_buffer_store(out_b, uadd(b3, cu(0U)), hd::add(g, g.buffer_load(out_b, uadd(b3, cu(0U))), hd::mul(g, radc[0], ks(cfg.exposure))));
    g.stmt_buffer_store(out_b, uadd(b3, cu(1U)), hd::add(g, g.buffer_load(out_b, uadd(b3, cu(1U))), hd::mul(g, radc[1], ks(cfg.exposure))));
    g.stmt_buffer_store(out_b, uadd(b3, cu(2U)), hd::add(g, g.buffer_load(out_b, uadd(b3, cu(2U))), hd::mul(g, radc[2], ks(cfg.exposure))));

    g.stmt_for_end(floop);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(cfg.local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::hairrt

