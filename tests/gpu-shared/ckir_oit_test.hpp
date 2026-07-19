#pragma once

// ckir_oit_test.hpp — SHARED, backend-neutral CKIR shaders + CPU oracle for B17 ORDER-INDEPENDENT TRANSPARENCY (D-007).
// The SAME graphs feed the Vulkan (C1) and DX12 (C4) raster paths (ADR-0101). Pure crd::kir + crd::math; no Vulkan / D3D12.
//
// B17-a WEIGHTED-BLENDED OIT (McGuire-Bavoil, JCGT 2013). N translucent full-screen quads are accumulated in ONE
// order-independent pass into two float targets — an ADDITIVE accumulation `Σ (Ci·ai·wi , ai·wi)` (RGBA16F) and a
// MULTIPLICATIVE revealage `Π(1-ai)` (R16F) — then a full-screen composite resolves `avg = accum.rgb/max(accum.a, eps)`
// and blends `avg·(1-reveal) + background·reveal`. The transparent FS emits BOTH attachments; the composite FS samples the
// two float targets (bindless index 0 = accum, 1 = revealage) which `draw_wboit` binds. Depth weight `w(d) = 0.01 + 10·(1-d)²`
// (mul/add only — the sole non-bit-exact ops are the RGBA16F/R16F store rounding + the composite divide, so WBOIT is a
// TO-ULP tier, validated by RGBA8-observable equality with a tight LSB tolerance and Vulkan == DX12).

#include <crd/kir/ckir.hpp>

#include <cmath>  // std::lround for the RGBA8 quantisation
#include <cstring> // std::memcpy for the f16-precision round

namespace crd::gputest
{
namespace kir = crd::kir;

inline constexpr crd::u32 kMaxWboitQuads = 8U;

// A WBOIT test scene: `count` translucent full-screen quads (colour · coverage · depth) composited over `background`.
struct WboitScene
{
    crd::u32 count = 0U;
    float    color[kMaxWboitQuads][3] = {};
    float    alpha[kMaxWboitQuads]    = {};
    float    depth[kMaxWboitQuads]    = {};
    float    background[3]            = {};
};

// The shared B17 translucent test scene: four full-screen quads (colour · coverage · depth) over an opaque background —
// used by the WBOIT (B17-a), A-buffer (B17-c) and MBOIT (B17-b) tests so the tiers can be cross-validated on ONE scene.
[[nodiscard]] inline WboitScene make_oit_scene()
{
    WboitScene s;
    s.count         = 4U;
    s.background[0] = 0.10F; s.background[1] = 0.10F; s.background[2] = 0.12F;
    s.color[0][0] = 0.90F; s.color[0][1] = 0.15F; s.color[0][2] = 0.10F; s.alpha[0] = 0.50F; s.depth[0] = 0.20F;
    s.color[1][0] = 0.15F; s.color[1][1] = 0.85F; s.color[1][2] = 0.20F; s.alpha[1] = 0.40F; s.depth[1] = 0.55F;
    s.color[2][0] = 0.20F; s.color[2][1] = 0.25F; s.color[2][2] = 0.90F; s.alpha[2] = 0.30F; s.depth[2] = 0.80F;
    s.color[3][0] = 0.90F; s.color[3][1] = 0.85F; s.color[3][2] = 0.10F; s.alpha[3] = 0.60F; s.depth[3] = 0.35F;
    return s;
}

// Round a float to HALF (f16) precision (round-to-nearest-even), keeping the f32 exponent — a faithful model of the GPU's
// "compute-in-f32, store-to-f16" blend for the accumulation targets, for values in half's normal range (our accum/reveal).
[[nodiscard]] inline float round_to_f16(float x)
{
    crd::u32 u = 0U;
    std::memcpy(&u, &x, sizeof(u));
    const crd::u32 low = u & 0x00001FFFU;                                  // low 13 mantissa bits dropped by f16
    crd::u32       t   = u & 0xFFFFE000U;                                  // keep sign + exponent + top 10 mantissa bits
    if (low > 0x00001000U || (low == 0x00001000U && (u & 0x00002000U) != 0U)) { t += 0x00002000U; } // RNE
    float r = 0.0F;
    std::memcpy(&r, &t, sizeof(r));
    return r;
}

// ── B17-a WBOIT shader graphs ────────────────────────────────────────────────────────────────────────────────────────

// TRANSPARENT VERTEX: `scene.count` full-screen quads (6 verts each) from VertexIndex. Every quad tiles the whole screen
// (quad = vid/6, corner = vid%6); the per-quad colour+coverage (flat, loc 0) and depth (flat, loc 1) are selected by quad
// index. Order irrelevant — the accumulation is commutative.
inline void build_wboit_transparent_vs(kir::KGraph& g, kir::KEntry& ve, const WboitScene& scene)
{
    const auto sh = kir::make_shape({1});
    const auto f  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto i  = [&](int v) { return g.constant(static_cast<double>(v), sh, kir::DType::I32); };

    const int vid    = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::I32); // 0 .. count*6-1
    const int quad   = g.binary(kir::KOp::Div, vid, i(6));                             // which translucent quad
    const int corner = g.binary(kir::KOp::Sub, vid, g.binary(kir::KOp::Mul, quad, i(6))); // 0..5 within the quad
    const auto eqc   = [&](int v) { return g.binary(kir::KOp::CmpEq, corner, i(v)); };
    const auto eqq   = [&](int v) { return g.binary(kir::KOp::CmpEq, quad, i(v)); };

    // Full-screen quad: x=+1 for corners {1,2,4}, y=+1 for {2,4,5} (SROA-safe select chain, no bool ops).
    const int x = g.select(eqc(1), f(1.0), g.select(eqc(2), f(1.0), g.select(eqc(4), f(1.0), f(-1.0))));
    const int y = g.select(eqc(2), f(1.0), g.select(eqc(4), f(1.0), g.select(eqc(5), f(1.0), f(-1.0))));

    // Per-quad data via select-on-quad chains (the last quad is the chain tail).
    const auto sel = [&](const auto& pick) {
        const crd::u32 n   = scene.count;
        int            acc = f(pick(n - 1U));
        for (crd::u32 q = n - 1U; q-- > 0U;) { acc = g.select(eqq(static_cast<int>(q)), f(pick(q)), acc); }
        return acc;
    };
    const int cr    = sel([&](crd::u32 q) { return scene.color[q][0]; });
    const int cg    = sel([&](crd::u32 q) { return scene.color[q][1]; });
    const int cb    = sel([&](crd::u32 q) { return scene.color[q][2]; });
    const int ca    = sel([&](crd::u32 q) { return scene.alpha[q]; });
    const int depth = sel([&](crd::u32 q) { return scene.depth[q]; });

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(x, y, f(0.5), f(1.0));
    ve.n_out    = 2;
    ve.out[0]   = {g.vec4(cr, cg, cb, ca), 0, kir::Interp::Flat}; // (colour.rgb, coverage)
    ve.out[1]   = {depth, 1, kir::Interp::Flat};                  // depth for the weight
}

// TRANSPARENT FRAGMENT: emit the two WBOIT attachments. loc 0 = `vec4(Ci·ai·wi, ai·wi)` (accum, additive); loc 1 = `ai`
// (revealage source, blended as `reveal *= 1-ai`). Weight `wi = 0.01 + 10·(1-d)²`.
inline void build_wboit_transparent_fs(kir::KGraph& g, kir::KEntry& fe)
{
    const auto sh = kir::make_shape({1});
    const auto f  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };

    const int ca    = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 0, kir::Interp::Flat); // (colour.rgb, coverage)
    const int depth = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 1, kir::Interp::Flat);
    const int color = g.swizzle(ca, 0, 1, 2);
    const int alpha = g.swizzle(ca, 3);

    const int om = g.binary(kir::KOp::Sub, f(1.0), depth);                              // 1 - d
    const int w  = g.binary(kir::KOp::Add, f(0.01), g.binary(kir::KOp::Mul, f(10.0), g.binary(kir::KOp::Mul, om, om)));
    const int premult   = g.binary(kir::KOp::Mul, color, g.splat(alpha, 3));            // Ci·ai
    const int premult_w = g.binary(kir::KOp::Mul, premult, g.splat(w, 3));              // Ci·ai·wi
    const int aw        = g.binary(kir::KOp::Mul, alpha, w);                            // ai·wi
    const int accum     = g.vec_concat(premult_w, aw);                                  // vec4

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 2;
    fe.out[0] = {accum, 0};
    fe.out[1] = {alpha, 1};
}

// COMPOSITE VERTEX: one full-screen triangle (3 verts) — clip {(-1,-1),(3,-1),(-1,3)}; uv = clip·0.5 + 0.5 (loc 0).
inline void build_wboit_composite_vs(kir::KGraph& g, kir::KEntry& ve)
{
    const auto sh  = kir::make_shape({1});
    const auto f   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  vid = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::I32); // 0..2
    const auto eqi = [&](int v) { return g.binary(kir::KOp::CmpEq, vid, g.constant(static_cast<double>(v), sh, kir::DType::I32)); };
    const int  x   = g.select(eqi(1), f(3.0), f(-1.0));
    const int  y   = g.select(eqi(2), f(3.0), f(-1.0));
    const int  ux  = g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, x, f(1.0)), f(0.5));
    const int  uy  = g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, y, f(1.0)), f(0.5));

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(x, y, f(0.0), f(1.0));
    ve.n_out    = 1;
    ve.out[0]   = {g.vec2(ux, uy), 0, kir::Interp::Smooth};
}

// COMPOSITE FRAGMENT: sample accum (bindless index 0) + revealage (index 1); resolve `avg = accum.rgb/max(accum.a, 1e-4)`
// and emit `vec4(avg, reveal)` — the hardware blend then produces `avg·(1-reveal) + background·reveal`.
inline void build_wboit_composite_fs(kir::KGraph& g, kir::KEntry& fe)
{
    const auto sh   = kir::make_shape({1});
    const int  uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  tex  = g.texture(0, 3, kir::DType::F32, kir::TexDim::Tex2D, false, false, false, /*array_count=*/8);
    const int  samp = g.sampler(0, 2);
    const int  i0   = g.constant(0.0, sh, kir::DType::U32);
    const int  i1   = g.constant(1.0, sh, kir::DType::U32);

    const int accum = g.tex_sample_at(tex, samp, uv, i0);          // vec4
    const int rev   = g.swizzle(g.tex_sample_at(tex, samp, uv, i1), 0); // R16F -> .r
    const int rgb   = g.swizzle(accum, 0, 1, 2);
    const int a     = g.swizzle(accum, 3);
    const int denom = g.binary(kir::KOp::Max, a, g.constant(1.0e-4, sh, kir::DType::F32));
    const int avg   = g.binary(kir::KOp::Div, rgb, g.splat(denom, 3));

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec_concat(avg, rev), 0};
}

// ── B17-a WBOIT CPU ORACLE ───────────────────────────────────────────────────────────────────────────────────────────
//
// Replicate the GPU pipeline in f32 with per-step f16 rounding of the accumulation targets (matching "compute-f32,
// store-f16" blend) + f32 divide + the composite blend over the QUANTISED background (the target is cleared to RGBA8 then
// blended). Every quad covers every pixel (full-screen), so the result is uniform — one colour, written to all texels.
// Returns the packed little-endian RGBA8 (0xAABBGGRR, matching IRasterTarget::read_pixel).
[[nodiscard]] inline crd::u32 wboit_oracle_pixel(const WboitScene& scene)
{
    float accum[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    float reveal   = 1.0F;
    for (crd::u32 q = 0U; q < scene.count; ++q)
    {
        const float d  = scene.depth[q];
        const float om = 1.0F - d;
        const float w  = 0.01F + 10.0F * om * om;
        const float ai = scene.alpha[q];
        for (int c = 0; c < 3; ++c)
        {
            accum[c] = round_to_f16(accum[c] + scene.color[q][c] * ai * w);
        }
        accum[3] = round_to_f16(accum[3] + ai * w);
        reveal   = round_to_f16(reveal * (1.0F - ai));
    }
    const float denom = accum[3] > 1.0e-4F ? accum[3] : 1.0e-4F;
    crd::u32    out   = 0xFF000000U; // opaque
    for (int c = 0; c < 3; ++c)
    {
        const float avg  = accum[c] / denom;
        const float bg_q = static_cast<float>(std::lround(scene.background[c] * 255.0F)) / 255.0F; // dequantised clear
        float       v    = avg * (1.0F - reveal) + bg_q * reveal;
        v                = v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
        out |= static_cast<crd::u32>(std::lround(v * 255.0F)) << (8 * c);
    }
    return out;
}

// The EXACT sorted over-composite of the scene (the A-buffer ground truth), quantised to packed RGBA8 — a pure-CPU mirror
// of the `build_abuffer_resolve` kernel (validated bit-exact on both devices). Used to score WBOIT's approximation error.
[[nodiscard]] inline crd::u32 oit_exact_composite_rgba8(const WboitScene& scene)
{
    crd::u32 order[kMaxWboitQuads];
    for (crd::u32 i = 0; i < scene.count; ++i) { order[i] = i; }
    for (crd::u32 i = 0; i + 1U < scene.count; ++i) // sort ascending by depth (front-to-back)
    {
        for (crd::u32 j = 0; j + 1U + i < scene.count; ++j)
        {
            if (scene.depth[order[j]] > scene.depth[order[j + 1U]])
            {
                const crd::u32 t = order[j];
                order[j]         = order[j + 1U];
                order[j + 1U]    = t;
            }
        }
    }
    float t   = 1.0F;
    float c[3] = {0.0F, 0.0F, 0.0F};
    for (crd::u32 k = 0; k < scene.count; ++k)
    {
        const crd::u32 q  = order[k];
        const float    ta = t * scene.alpha[q];
        for (int ch = 0; ch < 3; ++ch) { c[ch] = c[ch] + ta * scene.color[q][ch]; }
        t = t * (1.0F - scene.alpha[q]);
    }
    crd::u32 out = 0xFF000000U;
    for (int ch = 0; ch < 3; ++ch)
    {
        float v = c[ch] + t * scene.background[ch];
        v       = v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
        out |= static_cast<crd::u32>(std::lround(v * 255.0F)) << (8 * ch);
    }
    return out;
}

// Per-channel absolute difference of two packed RGBA8 texels (max over R,G,B) — the LSB tolerance check.
[[nodiscard]] inline crd::u32 rgba8_max_channel_diff(crd::u32 a, crd::u32 b)
{
    crd::u32 m = 0U;
    for (int c = 0; c < 3; ++c)
    {
        const int ca = static_cast<int>((a >> (8 * c)) & 0xFFU);
        const int cb = static_cast<int>((b >> (8 * c)) & 0xFFU);
        const int dd = ca > cb ? ca - cb : cb - ca;
        if (static_cast<crd::u32>(dd) > m) { m = static_cast<crd::u32>(dd); }
    }
    return m;
}

} // namespace crd::gputest
