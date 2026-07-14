#pragma once

// ckir_restir.hpp — D-007 B14-a: ReSTIR (Bitterli et al. 2020, "Spatiotemporal reservoir resampling for real-time ray
// tracing with dynamic direct lighting") — the dominant real-time many-light sampler, authored in CKIR. Each pixel keeps a
// RESERVOIR that resamples a stream of light-sample candidates by their contribution (RIS — Resampled Importance Sampling),
// then reuses reservoirs across FRAMES (temporal) and NEIGHBOURS (spatial) so one pixel effectively samples thousands of
// lights. The CANDIDATE GENERATION (which light, its unshadowed contribution, the shadow-ray visibility) is the ray-tracing
// leaf — deferred to B9; the RESERVOIR + RIS + reuse MATH (the whole estimator, and its unbiasedness) is built + verified
// here (statement-tier compute, CPU oracle + both backends), exactly as SVGF/DDGI were.
//
// A reservoir is 4 floats per pixel: [y_f (the selected sample's contribution f(y)), y_phat (its target p̂(y)), W (the RIS
// unbiased weight = w_sum/(M·p̂(y))), M (candidates seen)]. The single-sample estimate of the light integral is f(y)·W;
// E[f(y)·W] = ∫f exactly (RIS is unbiased). FP32 `precise` ⇒ bit-matches the oracle (the only ULP is `div`, IEEE-exact).

#include <crd/kir/ckir.hpp>

namespace crd::kir::restir
{

struct RestirConfig
{
    int    num_candidates = 32;   // M — light candidates streamed into each pixel's reservoir per frame (the RT leaf)
    double m_cap          = 20.0; // temporal history clamp: prev M capped to m_cap·M_current (bounds temporal bias/lag)

    [[nodiscard]] bool valid() const noexcept { return num_candidates >= 1; }
};

namespace detail
{
constexpr double kEps = 1.0e-8;
} // namespace detail

// Build the RIS pass — stream M candidates through each pixel's reservoir via Weighted Reservoir Sampling, output the
// reservoir. Candidate `i` = (f_i = its shaded contribution, phat_i = its target/resampling weight, xi_i ∈ [0,1) = the WRS
// random). WRS keeps candidate i with probability w_i/Σw (w_i = phat_i, the source pdf folded in by the caller). Then
// W = Σw / (M·p̂(y)). Buffers: 0=cand (F32 N·M·3), 1=out_res (F32 N·4 = f, phat, W, M). One thread per pixel.
[[nodiscard]] inline KEntry build_restir_ris(KGraph& g, const RestirConfig& cfg)
{
    const int  m   = cfg.num_candidates;
    const auto kf  = [&](crd::f64 v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int cand_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 1, true);
    const int p      = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    const int f0     = kf(0.0);

    const int mark = g.kernel_stmt_mark();
    int       wsum = f0;
    int       cf   = f0; // chosen f(y)
    int       cph  = f0; // chosen p̂(y)
    for (int i = 0; i < m; ++i)
    {
        const int base = mul(add(mul(p, ku(static_cast<crd::u32>(m))), ku(static_cast<crd::u32>(i))), ku(3));
        const int fi   = g.buffer_load(cand_b, base);
        const int phi  = g.buffer_load(cand_b, add(base, ku(1)));
        const int xii  = g.buffer_load(cand_b, add(base, ku(2)));
        wsum           = add(wsum, phi);
        const int repl = g.binary(KOp::CmpLt, xii, divv(phi, fmax(wsum, kf(detail::kEps)))); // keep i w.p. w_i/Σw
        cf             = g.select(repl, fi, cf);
        cph            = g.select(repl, phi, cph);
    }
    const int w   = divv(wsum, mul(kf(static_cast<double>(m)), fmax(cph, kf(detail::kEps)))); // W = Σw/(M·p̂(y))
    const int op4 = mul(p, ku(4));
    g.stmt_buffer_store(out_b, op4, cf);
    g.stmt_buffer_store(out_b, add(op4, ku(1)), cph);
    g.stmt_buffer_store(out_b, add(op4, ku(2)), w);
    g.stmt_buffer_store(out_b, add(op4, ku(3)), kf(static_cast<double>(m)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Build the TEMPORAL REUSE pass — merge each pixel's current reservoir with its (reprojected) previous reservoir. Each
// reservoir contributes its selected sample as one candidate weighted by its own Σw = phat·W·M; the merged reservoir picks
// one, with the previous M CLAMPED to m_cap·M_cur (bounds the temporal bias so stale samples don't dominate). The estimator
// stays unbiased and the effective sample count grows ⇒ variance drops. Buffers: 0=cur_res (F32 N·4), 1=prev_res (F32 N·4),
// 2=merge_xi (F32 N = the WRS random for the merge), 3=out_res (F32 N·4). One thread per pixel.
[[nodiscard]] inline KEntry build_restir_temporal(KGraph& g, const RestirConfig& cfg)
{
    const auto kf  = [&](crd::f64 v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto fmin = [&](int a, int b) { return g.binary(KOp::Min, a, b); };

    const int cur_b  = g.buffer_decl(DType::F32, 0, 0, false);
    const int prv_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int xi_b   = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 3, true);
    const int p      = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    const int c4     = mul(p, ku(4));
    const int eps    = kf(detail::kEps);

    const int mark = g.kernel_stmt_mark();
    const int cf   = g.buffer_load(cur_b, c4);
    const int cph  = g.buffer_load(cur_b, add(c4, ku(1)));
    const int c_w  = g.buffer_load(cur_b, add(c4, ku(2)));
    const int c_m  = g.buffer_load(cur_b, add(c4, ku(3)));
    const int pf   = g.buffer_load(prv_b, c4);
    const int pph  = g.buffer_load(prv_b, add(c4, ku(1)));
    const int p_w  = g.buffer_load(prv_b, add(c4, ku(2)));
    const int p_m  = fmin(g.buffer_load(prv_b, add(c4, ku(3))), mul(kf(cfg.m_cap), c_m)); // history clamp

    // each reservoir's contribution weight = its own Σw = p̂·W·M (invert W = Σw/(M·p̂))
    const int wc    = mul(mul(cph, c_w), c_m);
    const int wp    = mul(mul(pph, p_w), p_m);
    const int wsum  = add(wc, wp);
    const int mm    = add(c_m, p_m);
    const int xi    = g.buffer_load(xi_b, p);
    const int takep = g.binary(KOp::CmpLt, xi, divv(wp, fmax(wsum, eps))); // pick the PREV sample w.p. w_prev/Σw
    const int of    = g.select(takep, pf, cf);
    const int oph   = g.select(takep, pph, cph);
    const int o_w   = divv(wsum, mul(mm, fmax(oph, eps)));
    g.stmt_buffer_store(out_b, c4, of);
    g.stmt_buffer_store(out_b, add(c4, ku(1)), oph);
    g.stmt_buffer_store(out_b, add(c4, ku(2)), o_w);
    g.stmt_buffer_store(out_b, add(c4, ku(3)), mm);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::restir
