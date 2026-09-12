#pragma once

// ckir_nrc.hpp — D-007 B14-d: NEURAL RADIANCE CACHE (Müller et al. 2021, "Real-time Neural Radiance Caching for Path Tracing")
// — an online-trained tiny MLP that caches indirect radiance, authored in CKIR. The distinctive NRC primitive is the INPUT
// ENCODING: a MULTIRESOLUTION HASH GRID (Müller et al. 2022, "Instant Neural Graphics Primitives") that maps a 3D position to a
// compact feature vector the MLP consumes. This file builds, in CKIR (statement-tier compute, bit-exact CPU oracle + backends):
//   [1] build_nrc_hashgrid_encode  — the L-level trilinear hashed-feature encoder (the NRC-specific piece the MLP lacks),
//   [2] the MLP INFERENCE wiring (encode → the fused FP32 MLP forward in ckir_mlp.hpp → cached radiance),
//   [3] the online TRAINING step (L2 loss gradient → MLP backprop → feature/weight SGD update).
// The FP32 path is portable + bit-exact; the coopmat/cooperative-vector hardware-accelerated MLP tier rides B10 (the fused FP32
// MLP already crushes cuBLAS — see the NRC moat board). The RAY GENERATION that produces training targets is the B9 RT leaf.

#include <crd/kir/ckir.hpp>

namespace crd::kir::nrc
{

struct NrcConfig
{
    int    levels       = 4;     // L — hash-grid resolution levels (coarse→fine)
    int    features     = 2;     // F — features per level (per hash entry)
    int    table_size   = 16384; // T — hash entries per level (a power of two ⇒ index by & (T−1))
    int    base_res     = 16;    // N_min — base grid resolution
    double growth       = 2.0;   // b — per-level resolution growth (res_l = N_min · b^l)

    int    hidden       = 16;    // the NRC MLP hidden width (one ReLU hidden layer)
    int    out_dim      = 3;     // cached-radiance output channels (RGB)
    double learn_rate   = 0.01;  // SGD step for the online training update

    [[nodiscard]] int encoded_dim() const noexcept { return levels * features; } // the MLP input width (L·F)
    [[nodiscard]] bool valid() const noexcept { return levels >= 1 && features >= 1 && (table_size & (table_size - 1)) == 0 && hidden >= 1 && out_dim >= 1; }
};

namespace detail
{
constexpr crd::u32 kPi1 = 1U;          // Instant-NGP spatial-hash primes (π₁ folded as identity)
constexpr crd::u32 kPi2 = 2654435761U;
constexpr crd::u32 kPi3 = 805459861U;
} // namespace detail

// Build the MULTIRESOLUTION HASH-GRID ENCODER (Müller 2022 §3). One thread per query position. For each of L levels: scale the
// position by the level resolution, find the 8 enclosing grid corners, spatially HASH each corner into the level's T-entry
// table, and TRILINEARLY blend its F features by the fractional position. The L·F blended features (concatenated) are the MLP
// input. Buffers: 0 = positions (F32 N·3, in [0,1]³), 1 = tables (F32 L·T·F, the learnable features), 2 = out encoded
// (F32 N·(L·F)). Dispatch N/64 workgroups. Deterministic (fixed corner/level order, no atomics); bit-exact (mul/add/floor/
// bit-ops are exact, no transcendentals).
[[nodiscard]] inline KEntry build_nrc_hashgrid_encode(KGraph& g, const NrcConfig& cfg)
{
    const auto kf   = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto umul = [&](int a, crd::u32 b) { return g.binary(KOp::Mul, a, ku(b)); };
    const auto uxor = [&](int a, int b) { return g.binary(KOp::BitXor, a, b); };

    const int lvl = cfg.levels;
    const int ftr = cfg.features;
    const int tsz = cfg.table_size;
    const int lf  = lvl * ftr;

    const int pos_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int tab_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 2, true);
    const int p     = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int p3   = mul(p, ku(3));
    const int x    = g.buffer_load(pos_b, p3);
    const int y    = g.buffer_load(pos_b, add(p3, ku(1)));
    const int z    = g.buffer_load(pos_b, add(p3, ku(2)));
    const int po   = mul(p, ku(static_cast<crd::u32>(lf))); // output base for this position

    double res = static_cast<double>(cfg.base_res);
    for (int l = 0; l < lvl; ++l)
    {
        // scale into the level grid; corner-0 = floor, local = fractional offset in the cell.
        const int sx  = mul(x, kf(res));
        const int sy  = mul(y, kf(res));
        const int sz  = mul(z, kf(res));
        const int fx0 = g.unary(KOp::Floor, sx);
        const int fy0 = g.unary(KOp::Floor, sy);
        const int fz0 = g.unary(KOp::Floor, sz);
        const int lx  = sub(sx, fx0);
        const int ly  = sub(sy, fy0);
        const int lz  = sub(sz, fz0);
        const int cx0 = g.cast(fx0, DType::U32);
        const int cy0 = g.cast(fy0, DType::U32);
        const int cz0 = g.cast(fz0, DType::U32);
        const int lbase = ku(static_cast<crd::u32>(l * tsz)); // this level's table offset (in ENTRIES)

        int feat[8]; // up to F features accumulated (F ≤ 8 practical)
        for (int f = 0; f < ftr; ++f) { feat[f] = kf(0.0); }
        for (int corner = 0; corner < 8; ++corner)
        {
            const int dx = corner & 1;
            const int dy = (corner >> 1) & 1;
            const int dz = (corner >> 2) & 1;
            const int cx = (dx != 0) ? add(cx0, ku(1)) : cx0;
            const int cy = (dy != 0) ? add(cy0, ku(1)) : cy0;
            const int cz = (dz != 0) ? add(cz0, ku(1)) : cz0;
            // spatial hash → table entry (T is a power of two ⇒ & (T−1)).
            const int h  = g.binary(KOp::BitAnd, uxor(uxor(umul(cx, detail::kPi1), umul(cy, detail::kPi2)), umul(cz, detail::kPi3)), ku(static_cast<crd::u32>(tsz - 1)));
            // trilinear weight = ∏ (d ? local : 1−local).
            const int wx = (dx != 0) ? lx : sub(kf(1.0), lx);
            const int wy = (dy != 0) ? ly : sub(kf(1.0), ly);
            const int wz = (dz != 0) ? lz : sub(kf(1.0), lz);
            const int w  = mul(mul(wx, wy), wz);
            const int ent = mul(add(lbase, h), ku(static_cast<crd::u32>(ftr))); // element offset of this entry's F features
            for (int f = 0; f < ftr; ++f)
            {
                feat[f] = add(feat[f], mul(w, g.buffer_load(tab_b, add(ent, ku(static_cast<crd::u32>(f))))));
            }
        }
        for (int f = 0; f < ftr; ++f)
        {
            g.stmt_buffer_store(out_b, add(po, ku(static_cast<crd::u32>(l * ftr + f))), feat[f]);
        }
        res *= cfg.growth;
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Build the NRC MLP INFERENCE kernel — the cache query: encoded features → one ReLU hidden layer → linear output = cached
// radiance. One thread per sample (the small per-sample MLP is the FUNCTIONAL core; the fused tiled/coopmat MLP in ckir_mlp.hpp
// is the PERFORMANCE tier that rides B10). h_j = ReLU(Σ_d W1[j,d]·enc_d + b1_j); out_o = Σ_j W2[o,j]·h_j + b2_o. Buffers:
// 0 = encoded (F32 N·D, D = L·F), 1 = W1 (F32 H·D), 2 = b1 (F32 H), 3 = W2 (F32 O·H), 4 = b2 (F32 O), 5 = out (F32 N·O).
[[nodiscard]] inline KEntry build_nrc_infer(KGraph& g, const NrcConfig& cfg)
{
    const auto kf   = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int dd = cfg.encoded_dim();
    const int hh = cfg.hidden;
    const int oo = cfg.out_dim;

    const int enc_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int w1_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int b1_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int w2_b  = g.buffer_decl(DType::F32, 0, 3, false);
    const int b2_b  = g.buffer_decl(DType::F32, 0, 4, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 5, true);
    const int p     = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int eb   = mul(p, ku(static_cast<crd::u32>(dd))); // this sample's encoded-feature base

    int hact[64]; // hidden activations (H ≤ 64)
    for (int j = 0; j < hh; ++j)
    {
        int acc = g.buffer_load(b1_b, ku(static_cast<crd::u32>(j)));
        for (int d = 0; d < dd; ++d)
        {
            acc = add(acc, mul(g.buffer_load(w1_b, ku(static_cast<crd::u32>(j * dd + d))), g.buffer_load(enc_b, add(eb, ku(static_cast<crd::u32>(d))))));
        }
        hact[j] = fmax(acc, kf(0.0)); // ReLU
    }
    const int ob = mul(p, ku(static_cast<crd::u32>(oo)));
    for (int o = 0; o < oo; ++o)
    {
        int acc = g.buffer_load(b2_b, ku(static_cast<crd::u32>(o)));
        for (int j = 0; j < hh; ++j)
        {
            acc = add(acc, mul(g.buffer_load(w2_b, ku(static_cast<crd::u32>(o * hh + j))), hact[j]));
        }
        g.stmt_buffer_store(out_b, add(ob, ku(static_cast<crd::u32>(o))), acc);
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Build the NRC TRAINING-GRADIENT kernel — one online step's BACKPROP: forward, the L2 loss L = Σ_o (out_o − target_o)², then
// the analytic gradient of L w.r.t. both weight matrices (the hard part; the SGD weight update W −= lr·Σ_batch grad is trivial
// arithmetic on top, and batch accumulation is an atomic-add / reduction the caller drives). One thread per sample writes its
// OWN gradient contribution (no atomics ⇒ portable + trivially verifiable by finite differences). Chain:
//   dout_o = 2(out_o − tgt_o);  gW2[o,j] = dout_o·h_j;  dh_j = Σ_o dout_o·W2[o,j];  dpre_j = (pre_j>0)·dh_j;  gW1[j,d] = dpre_j·enc_d.
// Buffers: 0 = enc (N·D), 1 = W1 (H·D), 2 = b1 (H), 3 = W2 (O·H), 4 = b2 (O), 5 = target (N·O), 6 = gW1 (N·H·D), 7 = gW2 (N·O·H).
[[nodiscard]] inline KEntry build_nrc_train_grad(KGraph& g, const NrcConfig& cfg)
{
    const auto kf   = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int dd = cfg.encoded_dim();
    const int hh = cfg.hidden;
    const int oo = cfg.out_dim;

    const int enc_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int w1_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int b1_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int w2_b  = g.buffer_decl(DType::F32, 0, 3, false);
    const int b2_b  = g.buffer_decl(DType::F32, 0, 4, false);
    const int tgt_b = g.buffer_decl(DType::F32, 0, 5, false);
    const int gw1_b = g.buffer_decl(DType::F32, 0, 6, true);
    const int gw2_b = g.buffer_decl(DType::F32, 0, 7, true);
    const int p     = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int eb   = mul(p, ku(static_cast<crd::u32>(dd)));

    // FORWARD — keep pre-activations (for the ReLU mask), hidden activations, and outputs.
    int hact[64];
    int hmask[64]; // 1.0 where pre_j > 0 (the ReLU derivative), else 0.0
    for (int j = 0; j < hh; ++j)
    {
        int acc = g.buffer_load(b1_b, ku(static_cast<crd::u32>(j)));
        for (int d = 0; d < dd; ++d)
        {
            acc = add(acc, mul(g.buffer_load(w1_b, ku(static_cast<crd::u32>(j * dd + d))), g.buffer_load(enc_b, add(eb, ku(static_cast<crd::u32>(d))))));
        }
        hact[j]  = fmax(acc, kf(0.0));
        hmask[j] = g.select(g.binary(KOp::CmpGt, acc, kf(0.0)), kf(1.0), kf(0.0));
    }
    int dout[8];
    const int tb = mul(p, ku(static_cast<crd::u32>(oo)));
    for (int o = 0; o < oo; ++o)
    {
        int acc = g.buffer_load(b2_b, ku(static_cast<crd::u32>(o)));
        for (int j = 0; j < hh; ++j) { acc = add(acc, mul(g.buffer_load(w2_b, ku(static_cast<crd::u32>(o * hh + j))), hact[j])); }
        dout[o] = mul(kf(2.0), sub(acc, g.buffer_load(tgt_b, add(tb, ku(static_cast<crd::u32>(o)))))); // ∂L/∂out_o
    }

    // BACKWARD — gW2[o,j] = dout_o·h_j ; dh_j = Σ_o dout_o·W2[o,j] ; dpre_j = mask_j·dh_j ; gW1[j,d] = dpre_j·enc_d.
    const int g2base = mul(p, ku(static_cast<crd::u32>(oo * hh)));
    for (int o = 0; o < oo; ++o)
    {
        for (int j = 0; j < hh; ++j)
        {
            g.stmt_buffer_store(gw2_b, add(g2base, ku(static_cast<crd::u32>(o * hh + j))), mul(dout[o], hact[j]));
        }
    }
    const int g1base = mul(p, ku(static_cast<crd::u32>(hh * dd)));
    for (int j = 0; j < hh; ++j)
    {
        int dh = kf(0.0);
        for (int o = 0; o < oo; ++o) { dh = add(dh, mul(dout[o], g.buffer_load(w2_b, ku(static_cast<crd::u32>(o * hh + j))))); }
        const int dpre = mul(hmask[j], dh);
        for (int d = 0; d < dd; ++d)
        {
            g.stmt_buffer_store(gw1_b, add(g1base, ku(static_cast<crd::u32>(j * dd + d))), mul(dpre, g.buffer_load(enc_b, add(eb, ku(static_cast<crd::u32>(d))))));
        }
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::nrc
