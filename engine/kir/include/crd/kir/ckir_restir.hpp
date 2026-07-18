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
    int    num_candidates    = 32;   // M — light candidates streamed into each pixel's reservoir per frame (the RT leaf)
    double m_cap             = 20.0; // temporal history clamp: prev M capped to m_cap·M_current (bounds temporal bias/lag)
    int    spatial_neighbors = 4;    // K — reservoirs pulled from screen neighbours in the SPATIAL reuse pass

    [[nodiscard]] bool valid() const noexcept { return num_candidates >= 1 && spatial_neighbors >= 1; }
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
    // A tight RUNTIME LOOP over the M candidates, NOT a compile-time unroll — the unrolled straight-line code carries huge
    // register pressure and tanks GPU occupancy (measured 1.56× slower than a hand-written loop). The loop-carried reservoir
    // (Σw, chosen f, chosen p̂) lives in per-thread SHARED slots [tid]; measured at register-loop parity (see [.crush-bench]).
    const int sw   = g.shared_decl(DType::F32, 64); // Σw per thread
    const int scf  = g.shared_decl(DType::F32, 64); // chosen f(y)
    const int scph = g.shared_decl(DType::F32, 64); // chosen p̂(y)
    const int tid  = g.builtin(KBuiltin::LocalInvocationIndex);
    const int p    = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), tid);
    const int f0   = kf(0.0);
    const int eps  = kf(detail::kEps);

    const int mark = g.kernel_stmt_mark();
    g.stmt_materialize(p); // hoist p to the OUTER scope: it indexes candidates INSIDE the loop AND the output store OUTSIDE it,
                           // and the emitter scopes a first-inside-loop temp to the loop body (would be undefined at the store)
    g.stmt_shared_store(sw, tid, f0);
    g.stmt_shared_store(scf, tid, f0);
    g.stmt_shared_store(scph, tid, f0);
    const int fr   = g.stmt_for_begin(ku(static_cast<crd::u32>(m)));
    const int j    = g.kernel_loop_var(fr);
    const int base = mul(add(mul(p, ku(static_cast<crd::u32>(m))), j), ku(3));
    const int fi   = g.buffer_load(cand_b, base);
    const int phi  = g.buffer_load(cand_b, add(base, ku(1)));
    const int xii  = g.buffer_load(cand_b, add(base, ku(2)));
    const int wsum = add(g.shared_load(sw, tid), phi);
    g.stmt_materialize(wsum); // FREEZE Σw before storing it: else the later `repl` re-reads sw AFTER the store ⇒ phi double-counts
    g.stmt_shared_store(sw, tid, wsum);
    const int repl = g.binary(KOp::CmpLt, xii, divv(phi, fmax(wsum, eps))); // keep i w.p. w_i/Σw
    g.stmt_shared_store(scf, tid, g.select(repl, fi, g.shared_load(scf, tid)));
    g.stmt_shared_store(scph, tid, g.select(repl, phi, g.shared_load(scph, tid)));
    g.stmt_for_end(fr);

    const int cphf = g.shared_load(scph, tid);
    const int w    = divv(g.shared_load(sw, tid), mul(kf(static_cast<double>(m)), fmax(cphf, eps))); // W = Σw/(M·p̂(y))
    const int op4  = mul(p, ku(4));
    g.stmt_buffer_store(out_b, op4, g.shared_load(scf, tid));
    g.stmt_buffer_store(out_b, add(op4, ku(1)), cphf);
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

// Build the SPATIAL REUSE pass — merge each pixel's reservoir with K screen-space NEIGHBOUR reservoirs (Bitterli §5, the other
// half of spatiotemporal reuse). Generalises the two-reservoir temporal merge to K+1: streaming Weighted Reservoir Sampling
// keeps the center first (weight = its Σw = p̂·W·M), then each neighbour k with probability (its Σw)/(running Σw) — one fresh
// random per neighbour. The merged W = (Σ all Σw)/(Σ all M · p̂(selected)); the estimator stays UNBIASED and the effective
// sample count grows across the neighbourhood ⇒ variance drops further. (The neighbour SELECTION + the geometry/normal
// similarity reject that avoids merging across edges is the renderer/RT leaf — here the indices are supplied.) Buffers:
// 0 = res_in (F32 N·4), 1 = nbr (F32 N·K — the K neighbour pixel indices, integer-valued), 2 = xi (F32 N·K — the WRS randoms),
// 3 = out_res (F32 N·4). One thread per pixel.
[[nodiscard]] inline KEntry build_restir_spatial(KGraph& g, const RestirConfig& cfg)
{
    const auto kf   = [&](crd::f64 v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int kn    = cfg.spatial_neighbors;
    const int res_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int nbr_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int xi_b  = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 3, true);
    const int p     = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    const int c4    = mul(p, ku(4));
    const int eps   = kf(detail::kEps);
    const int pk    = mul(p, ku(static_cast<crd::u32>(kn)));

    const int mark = g.kernel_stmt_mark();
    const int cf   = g.buffer_load(res_b, c4);
    const int cph  = g.buffer_load(res_b, add(c4, ku(1)));
    const int cw   = g.buffer_load(res_b, add(c4, ku(2)));
    const int cm   = g.buffer_load(res_b, add(c4, ku(3)));
    int       wsum = mul(mul(cph, cw), cm); // the center reservoir's Σw = p̂·W·M
    int       mtot = cm;
    int       of   = cf;  // running selected sample
    int       oph  = cph;
    for (int j = 0; j < kn; ++j)
    {
        const int ni  = g.cast(g.buffer_load(nbr_b, add(pk, ku(static_cast<crd::u32>(j)))), DType::U32); // neighbour pixel index
        const int n4  = mul(ni, ku(4));
        const int nf  = g.buffer_load(res_b, n4);
        const int nph = g.buffer_load(res_b, add(n4, ku(1)));
        const int nw  = g.buffer_load(res_b, add(n4, ku(2)));
        const int nm  = g.buffer_load(res_b, add(n4, ku(3)));
        const int nsw = mul(mul(nph, nw), nm); // this neighbour's Σw
        wsum          = add(wsum, nsw);
        mtot          = add(mtot, nm);
        const int xi  = g.buffer_load(xi_b, add(pk, ku(static_cast<crd::u32>(j))));
        const int rep = g.binary(KOp::CmpLt, xi, divv(nsw, fmax(wsum, eps))); // keep this neighbour w.p. its Σw/running Σw
        of            = g.select(rep, nf, of);
        oph           = g.select(rep, nph, oph);
    }
    const int ow = divv(wsum, mul(mtot, fmax(oph, eps)));
    g.stmt_buffer_store(out_b, c4, of);
    g.stmt_buffer_store(out_b, add(c4, ku(1)), oph);
    g.stmt_buffer_store(out_b, add(c4, ku(2)), ow);
    g.stmt_buffer_store(out_b, add(c4, ku(3)), mtot);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::restir
