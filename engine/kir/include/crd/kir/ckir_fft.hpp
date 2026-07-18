#pragma once

// ckir_fft.hpp — B-cmp Phase 1: author an N-point complex FFT as a CKIR shared-memory compute kernel. This is the reusable
// FFT AUTHORING layer (the mandate: "author FFT in CKIR" — every future on-chip FFT/convolution consumer builds on it), NOT
// a one-off test kernel. Radix-2 Stockham AUTOSORT (natural-order in → natural-order out, no bit-reversal pass), out-of-place
// PING-PONG across two shared (re,im) buffers — required because CKIR re-reads shared lazily, so an in-place butterfly that
// overwrites its own input is a read-after-write hazard (see the campaign memory). Single workgroup, N/2 threads (one radix-2
// butterfly per thread per stage). The log2(N) stages are UNROLLED at authoring time (N is compile-time) so the src/dst
// buffers alternate DETERMINISTICALLY — CKIR cannot select a shared array by a runtime parity.
//
// Verified index map (radix-2 DIT self-sorting, checked by hand vs the N=4 DFT):
//   stage s (0..p-1), r = 2^s, L = 2^(s+1); thread t (0..N/2-1): g = t/r, j = t%r
//     read  in0 = g*r + j,  in1 = in0 + N/2      (the two halves of the previous pass)
//     write out0 = g*L + j, out1 = out0 + r      (contiguous natural-order groups of L)
//     twiddle w = W_L^j = W_N^(j*2^(p-1-s))  ⇒  twidx = j << (p-1-s)  into a precomputed W_N[N/2] table
//     butterfly: u = src[in0], v = w*src[in1];  dst[out0] = u + v;  dst[out1] = u - v
//
// Buffers (set 0): in_re=0, in_im=1, tw_re=2, tw_im=3, out_re=4, out_im=5. Twiddles are PRECOMPUTED + uploaded
// (W_N^k = (cos(2*pi*k/N), -sin(2*pi*k/N)), k=0..N/2-1) so the butterfly is add/sub/mul only — bit-exact once the kernel
// emitter emits `precise` temps (Phase-1 GPU determinism). `inverse` conjugates the twiddle (wi -> -wi); the 1/N scale is
// the caller's (kept out so forward+inverse share one graph and stay bit-exact).

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

struct Fft1dPlan
{
    KEntry entry;
    int    n       = 0;
    int    log2n   = 0;
    bool   inverse = false;
};

// log2 of a power-of-two n (n must be a power of two >= 2).
[[nodiscard]] inline int fft_log2(int n) noexcept
{
    int p = 0;
    while ((1 << p) < n) { ++p; }
    return p;
}

// Author the radix-2 Stockham forward (or inverse) FFT of size `n` into `g`. Returns the plan (KEntry + metadata).
[[nodiscard]] inline Fft1dPlan build_fft1d_radix2(KGraph& g, int n, bool inverse = false, bool batched = false)
{
    const int      p    = fft_log2(n);
    const int      half = n / 2;
    const Shape    sh1  = make_shape({1});
    const auto     ku   = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto     add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto     mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto     sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };

    const int in_re  = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_im  = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re  = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im  = g.buffer_decl(DType::F32, 0, 3, false);
    const int out_re = g.buffer_decl(DType::F32, 0, 4, true);
    const int out_im = g.buffer_decl(DType::F32, 0, 5, true);

    // ping-pong shared buffers: A = (a_re,a_im), B = (b_re,b_im)
    const int a_re = g.shared_decl(DType::F32, n);
    const int a_im = g.shared_decl(DType::F32, n);
    const int b_re = g.shared_decl(DType::F32, n);
    const int b_im = g.shared_decl(DType::F32, n);
    const int tid  = g.builtin(KBuiltin::LocalInvocationIndex); // butterfly index 0..half-1
    const int tid2 = add(tid, ku(static_cast<crd::u32>(half))); // the thread's second element
    // BATCHED: one workgroup does one N-point FFT; its in/out slice starts at WorkgroupIndex*N (shared indices stay local).
    const int  base = batched ? mul(g.builtin(KBuiltin::WorkgroupIndex), ku(static_cast<crd::u32>(n))) : -1;
    const auto boff = [&](int idx) { return batched ? add(base, idx) : idx; };

    const int mark = g.kernel_stmt_mark();
    // load natural-order input into A (each of the half threads loads elements tid and tid+N/2)
    g.stmt_shared_store(a_re, tid, g.buffer_load(in_re, boff(tid)));
    g.stmt_shared_store(a_im, tid, g.buffer_load(in_im, boff(tid)));
    g.stmt_shared_store(a_re, tid2, g.buffer_load(in_re, boff(tid2)));
    g.stmt_shared_store(a_im, tid2, g.buffer_load(in_im, boff(tid2)));
    g.stmt_barrier();

    for (int s = 0; s < p; ++s) // UNROLLED stages — deterministic ping-pong
    {
        const bool even   = (s % 2) == 0;
        const int  sre    = even ? a_re : b_re;
        const int  sim    = even ? a_im : b_im;
        const int  dre    = even ? b_re : a_re;
        const int  dim    = even ? b_im : a_im;
        const int  r      = 1 << s;
        const int  gidx   = g.binary(KOp::Div, tid, ku(static_cast<crd::u32>(r))); // t / r
        const int  jidx   = g.binary(KOp::Mod, tid, ku(static_cast<crd::u32>(r))); // t % r
        const int  in0    = add(mul(gidx, ku(static_cast<crd::u32>(r))), jidx);    // g*r + j
        const int  in1    = add(in0, ku(static_cast<crd::u32>(half)));             // + N/2
        const int  out0   = add(mul(gidx, ku(static_cast<crd::u32>(2 * r))), jidx); // g*L + j
        const int  out1   = add(out0, ku(static_cast<crd::u32>(r)));               // + r
        const int  twidx  = g.binary(KOp::Shl, jidx, ku(static_cast<crd::u32>(p - 1 - s))); // j << (p-1-s)
        const int  wr     = g.buffer_load(tw_re, twidx);
        int        wi     = g.buffer_load(tw_im, twidx);
        if (inverse) { wi = g.unary(KOp::Neg, wi); } // W_N^{-k} = conj(W_N^k)
        const int  x0r = g.shared_load(sre, in0);
        const int  x0i = g.shared_load(sim, in0);
        const int  x1r = g.shared_load(sre, in1);
        const int  x1i = g.shared_load(sim, in1);
        const int  vr  = sub(mul(wr, x1r), mul(wi, x1i)); // (w * x1).re
        const int  vi  = add(mul(wr, x1i), mul(wi, x1r)); // (w * x1).im
        g.stmt_shared_store(dre, out0, add(x0r, vr));
        g.stmt_shared_store(dim, out0, add(x0i, vi));
        g.stmt_shared_store(dre, out1, sub(x0r, vr));
        g.stmt_shared_store(dim, out1, sub(x0i, vi));
        g.stmt_barrier();
    }

    // after p stages the result is in B if p is odd, else A.
    const bool odd  = (p % 2) == 1;
    const int  fre  = odd ? b_re : a_re;
    const int  fim  = odd ? b_im : a_im;
    g.stmt_buffer_store(out_re, boff(tid), g.shared_load(fre, tid));
    g.stmt_buffer_store(out_im, boff(tid), g.shared_load(fim, tid));
    g.stmt_buffer_store(out_re, boff(tid2), g.shared_load(fre, tid2));
    g.stmt_buffer_store(out_im, boff(tid2), g.shared_load(fim, tid2));

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(half);
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = p;
    plan.inverse                 = inverse;
    return plan;
}

// log4 of a power-of-four n (n must be 4^k, k>=1).
[[nodiscard]] inline int fft_log4(int n) noexcept
{
    int p = 0;
    while ((1 << (2 * p)) < n) { ++p; }
    return p;
}

// Author a radix-4 Stockham FFT of size `n` (a power of FOUR) into `g` — half the shared-memory passes of radix-2 (log4 vs
// log2 stages), the CPU FFT's radix. The 4-point DFT is EXACT (adds/subs + ±i rotations, no irrational twiddles) so only the
// per-butterfly PRE-twiddles multiply — bit-exact once the emitter emits `precise` temps. N/4 threads, stages UNROLLED
// (ping-pong across two shared (re,im) pairs). Twiddle table is the FULL W_N[N] (radix-4 pre-twiddle indices reach ~3N/4):
// buffers in_re=0,in_im=1,tw_re=2 (len N),tw_im=3 (len N),out_re=4,out_im=5. `inverse` conjugates: pre-twiddle wi→-wi AND
// the 4-point DFT's ±i rotation flips (W_4 = -i forward → +i inverse). 1/N scale is the caller's.
[[nodiscard]] inline Fft1dPlan build_fft1d_radix4(KGraph& g, int n, bool inverse = false, bool batched = false)
{
    const int   p4      = fft_log4(n);
    const int   quarter = n / 4;
    const Shape sh1     = make_shape({1});
    const auto  ku      = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add     = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub     = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul     = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_re  = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_im  = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re  = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im  = g.buffer_decl(DType::F32, 0, 3, false);
    const int out_re = g.buffer_decl(DType::F32, 0, 4, true);
    const int out_im = g.buffer_decl(DType::F32, 0, 5, true);
    const int a_re   = g.shared_decl(DType::F32, n);
    const int a_im   = g.shared_decl(DType::F32, n);
    const int b_re   = g.shared_decl(DType::F32, n);
    const int b_im   = g.shared_decl(DType::F32, n);
    const int tid    = g.builtin(KBuiltin::LocalInvocationIndex); // butterfly index 0..N/4-1
    // BATCHED: one workgroup does one N-point FFT; its in/out slice starts at WorkgroupIndex*N (twiddles stay shared).
    const int base = batched ? mul(g.builtin(KBuiltin::WorkgroupIndex), ku(static_cast<crd::u32>(n))) : -1;
    const auto boff = [&](int idx) { return batched ? add(base, idx) : idx; };

    const int mark = g.kernel_stmt_mark();
    for (int m = 0; m < 4; ++m) // load the 4 natural-order elements this thread owns into A
    {
        const int idx = add(tid, ku(static_cast<crd::u32>(m * quarter)));
        g.stmt_shared_store(a_re, idx, g.buffer_load(in_re, boff(idx)));
        g.stmt_shared_store(a_im, idx, g.buffer_load(in_im, boff(idx)));
    }
    g.stmt_barrier();

    for (int s = 0; s < p4; ++s) // UNROLLED radix-4 stages
    {
        const bool even = (s % 2) == 0;
        const int  sre  = even ? a_re : b_re;
        const int  sim  = even ? a_im : b_im;
        const int  dre  = even ? b_re : a_re;
        const int  dim  = even ? b_im : a_im;
        const int  rs   = 1 << (2 * s);       // 4^s
        const int  ll   = 1 << (2 * s + 2);   // 4^(s+1)
        const int  nl   = n / ll;             // N / L
        const int  gidx = g.binary(KOp::Div, tid, ku(static_cast<crd::u32>(rs)));
        const int  jidx = g.binary(KOp::Mod, tid, ku(static_cast<crd::u32>(rs)));

        // load + pre-twiddle the 4 inputs a0..a3 (a0: W^0 = 1).
        int ar[4];
        int ai[4];
        for (int m = 0; m < 4; ++m)
        {
            const int inm = add(add(mul(gidx, ku(static_cast<crd::u32>(rs))), jidx), ku(static_cast<crd::u32>(m * quarter)));
            const int sr  = g.shared_load(sre, inm);
            const int sii = g.shared_load(sim, inm);
            if (m == 0) { ar[0] = sr; ai[0] = sii; continue; }
            const int twidx = mul(jidx, ku(static_cast<crd::u32>(m * nl))); // (m*j)*(N/L)
            const int wr    = g.buffer_load(tw_re, twidx);
            int       wi    = g.buffer_load(tw_im, twidx);
            if (inverse) { wi = g.unary(KOp::Neg, wi); }
            ar[m] = sub(mul(sr, wr), mul(sii, wi)); // (s * w).re
            ai[m] = add(mul(sr, wi), mul(sii, wr)); // (s * w).im
        }
        // 4-point DFT (exact: adds/subs + i-rotate). t0=a0+a2, t1=a0-a2, t2=a1+a3, t3=a1-a3.
        const int t0r = add(ar[0], ar[2]);
        const int t0i = add(ai[0], ai[2]);
        const int t1r = sub(ar[0], ar[2]);
        const int t1i = sub(ai[0], ai[2]);
        const int t2r = add(ar[1], ar[3]);
        const int t2i = add(ai[1], ai[3]);
        const int t3r = sub(ar[1], ar[3]);
        const int t3i = sub(ai[1], ai[3]);
        int       xr[4];
        int       xi[4];
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i); // X0 = t0 + t2
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i); // X2 = t0 - t2
        if (!inverse) // forward: X1 = t1 - i*t3 (=(t1r+t3i, t1i-t3r)), X3 = t1 + i*t3
        {
            xr[1] = add(t1r, t3i); xi[1] = sub(t1i, t3r);
            xr[3] = sub(t1r, t3i); xi[3] = add(t1i, t3r);
        }
        else // inverse: X1 = t1 + i*t3, X3 = t1 - i*t3
        {
            xr[1] = sub(t1r, t3i); xi[1] = add(t1i, t3r);
            xr[3] = add(t1r, t3i); xi[3] = sub(t1i, t3r);
        }
        for (int k = 0; k < 4; ++k) // out_k = g*L + j + k*4^s
        {
            const int outk = add(add(mul(gidx, ku(static_cast<crd::u32>(ll))), jidx), ku(static_cast<crd::u32>(k * rs)));
            g.stmt_shared_store(dre, outk, xr[k]);
            g.stmt_shared_store(dim, outk, xi[k]);
        }
        g.stmt_barrier();
    }

    const bool odd = (p4 % 2) == 1;
    const int  fre = odd ? b_re : a_re;
    const int  fim = odd ? b_im : a_im;
    for (int m = 0; m < 4; ++m)
    {
        const int idx = add(tid, ku(static_cast<crd::u32>(m * quarter)));
        g.stmt_buffer_store(out_re, boff(idx), g.shared_load(fre, idx));
        g.stmt_buffer_store(out_im, boff(idx), g.shared_load(fim, idx));
    }

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(quarter);
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = 2 * p4;
    plan.inverse                 = inverse;
    return plan;
}

// ⭐ THE CRUSH PRIMITIVE (1-D): a FUSED FFT-convolution — forward FFT → ×filter spectrum → inverse FFT → `scale`, ALL in
// ONE dispatch, entirely in shared memory. The vendor path (cuFFT/VkFFT) pays THREE global round-trips (fwd kernel → multiply
// kernel → inv kernel); we pay ONE on-chip pass. Radix-4 Stockham fwd + inv (ping-pong A↔B via a current/other flip); the
// pointwise multiply happens in-shared between them. Output = the circular convolution of the input with the filter h whose
// PRECOMPUTED spectrum FFT(h) is uploaded. `scale` (<0 ⇒ 1/N, the natural single-conv normalization) is exact in f32 when it
// is a power of two ⇒ the pipeline is bit-exact via precise. `batched_filter` (batched only): offset the FILTER per workgroup
// too (each signal gets its OWN filter slice, WorkgroupIndex*N) — needed by the 2-D convolution where each column carries a
// different filter column; default false is the bloom pattern (ONE shared filter over all batch signals).
// Buffers: in_re=0,in_im=1, tw_re=2,tw_im=3 (full W_N[N]), filt_re=4,filt_im=5 (=FFT(h)), out_re=6,out_im=7. N/4 threads.
[[nodiscard]] inline Fft1dPlan build_fft1d_convolution(KGraph& g, int n, bool batched = false, crd::f64 scale = -1.0,
                                                       bool batched_filter = false)
{
    const int      p4      = fft_log4(n);
    const int      quarter = n / 4;
    const Shape    sh1     = make_shape({1});
    const crd::f64 sc      = scale < 0.0 ? 1.0 / static_cast<crd::f64>(n) : scale;
    const auto     ku      = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto     add     = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto     sub     = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto     mul     = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto     neg     = [&](int a) { return g.unary(KOp::Neg, a); };

    const int in_re   = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_im   = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re   = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im   = g.buffer_decl(DType::F32, 0, 3, false);
    const int filt_re = g.buffer_decl(DType::F32, 0, 4, false);
    const int filt_im = g.buffer_decl(DType::F32, 0, 5, false);
    const int out_re  = g.buffer_decl(DType::F32, 0, 6, true);
    const int out_im  = g.buffer_decl(DType::F32, 0, 7, true);
    const int a_re    = g.shared_decl(DType::F32, n);
    const int a_im    = g.shared_decl(DType::F32, n);
    const int b_re    = g.shared_decl(DType::F32, n);
    const int b_im    = g.shared_decl(DType::F32, n);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    // BATCHED: one workgroup convolves one N-signal; its in/out slice starts at WorkgroupIndex*N (twiddles stay shared).
    const int  base = batched ? mul(g.builtin(KBuiltin::WorkgroupIndex), ku(static_cast<crd::u32>(n))) : -1;
    const auto boff = [&](int idx) { return batched ? add(base, idx) : idx; };
    // filter offset: shared over the batch (bloom PSF) OR per-workgroup (2-D conv — each column its own filter slice).
    const int  fbase     = (batched && batched_filter) ? mul(g.builtin(KBuiltin::WorkgroupIndex), ku(static_cast<crd::u32>(n))) : -1;
    const auto boff_filt = [&](int idx) { return (batched && batched_filter) ? add(fbase, idx) : idx; };

    int        cre = a_re; int cim = a_im; int ore = b_re; int oim = b_im; // current / other ping-pong pair
    const auto flip = [&]() { const int tr = cre; cre = ore; ore = tr; const int ti = cim; cim = oim; oim = ti; };

    // one radix-4 Stockham stage: reads the CURRENT pair, writes the OTHER, + a barrier. `inv` conjugates.
    const auto stage = [&](int s, bool inv) {
        const int rs   = 1 << (2 * s);
        const int ll   = 1 << (2 * s + 2);
        const int nl   = n / ll;
        const int gidx = g.binary(KOp::Div, tid, ku(static_cast<crd::u32>(rs)));
        const int jidx = g.binary(KOp::Mod, tid, ku(static_cast<crd::u32>(rs)));
        int       ar[4]; int ai[4];
        for (int m = 0; m < 4; ++m)
        {
            const int inm = add(add(mul(gidx, ku(static_cast<crd::u32>(rs))), jidx), ku(static_cast<crd::u32>(m * quarter)));
            const int sr  = g.shared_load(cre, inm);
            const int sii = g.shared_load(cim, inm);
            if (m == 0) { ar[0] = sr; ai[0] = sii; continue; }
            const int twidx = mul(jidx, ku(static_cast<crd::u32>(m * nl)));
            const int wr    = g.buffer_load(tw_re, twidx);
            int       wi    = g.buffer_load(tw_im, twidx);
            if (inv) { wi = neg(wi); }
            ar[m] = sub(mul(sr, wr), mul(sii, wi));
            ai[m] = add(mul(sr, wi), mul(sii, wr));
        }
        const int t0r = add(ar[0], ar[2]); const int t0i = add(ai[0], ai[2]);
        const int t1r = sub(ar[0], ar[2]); const int t1i = sub(ai[0], ai[2]);
        const int t2r = add(ar[1], ar[3]); const int t2i = add(ai[1], ai[3]);
        const int t3r = sub(ar[1], ar[3]); const int t3i = sub(ai[1], ai[3]);
        int       xr[4]; int xi[4];
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i);
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i);
        if (!inv) { xr[1] = add(t1r, t3i); xi[1] = sub(t1i, t3r); xr[3] = sub(t1r, t3i); xi[3] = add(t1i, t3r); }
        else      { xr[1] = sub(t1r, t3i); xi[1] = add(t1i, t3r); xr[3] = add(t1r, t3i); xi[3] = sub(t1i, t3r); }
        for (int k = 0; k < 4; ++k)
        {
            const int outk = add(add(mul(gidx, ku(static_cast<crd::u32>(ll))), jidx), ku(static_cast<crd::u32>(k * rs)));
            g.stmt_shared_store(ore, outk, xr[k]);
            g.stmt_shared_store(oim, outk, xi[k]);
        }
        g.stmt_barrier();
    };

    const int mark = g.kernel_stmt_mark();
    for (int m = 0; m < 4; ++m) // load x into the current buffer
    {
        const int idx = add(tid, ku(static_cast<crd::u32>(m * quarter)));
        g.stmt_shared_store(cre, idx, g.buffer_load(in_re, boff(idx)));
        g.stmt_shared_store(cim, idx, g.buffer_load(in_im, boff(idx)));
    }
    g.stmt_barrier();
    for (int s = 0; s < p4; ++s) { stage(s, false); flip(); } // FORWARD FFT (current = FFT(x))
    for (int m = 0; m < 4; ++m) // ×FILTER SPECTRUM — read CURRENT, write OTHER (disjoint ⇒ no lazy-eval RAW hazard), then flip
    {
        const int idx = add(tid, ku(static_cast<crd::u32>(m * quarter)));
        const int xr  = g.shared_load(cre, idx);
        const int xi  = g.shared_load(cim, idx);
        const int fr  = g.buffer_load(filt_re, boff_filt(idx));
        const int fi  = g.buffer_load(filt_im, boff_filt(idx));
        g.stmt_shared_store(ore, idx, sub(mul(xr, fr), mul(xi, fi)));
        g.stmt_shared_store(oim, idx, add(mul(xr, fi), mul(xi, fr)));
    }
    g.stmt_barrier();
    flip();
    for (int s = 0; s < p4; ++s) { stage(s, true); flip(); } // INVERSE FFT (current = N * conv)
    const int invn = g.constant(sc, sh1, DType::F32); // `scale` (default 1/N); exact in f32 when a power of two
    for (int m = 0; m < 4; ++m)
    {
        const int idx = add(tid, ku(static_cast<crd::u32>(m * quarter)));
        g.stmt_buffer_store(out_re, boff(idx), mul(g.shared_load(cre, idx), invn));
        g.stmt_buffer_store(out_im, boff(idx), mul(g.shared_load(cim, idx), invn));
    }

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(quarter);
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = 2 * p4;
    plan.inverse                 = false;
    return plan;
}

// log8 of a power-of-eight n (n = 8^k, k>=1).
[[nodiscard]] inline int fft_log8(int n) noexcept
{
    int p = 0;
    while ((1 << (3 * p)) < n) { ++p; }
    return p;
}

// Author a radix-8 Stockham FFT of size `n` (a power of EIGHT) — the SOTA throughput radix (⅓ the shared passes of radix-2).
// The 8-point DFT is built as TWO exact 4-point DFTs (even/odd, adds/subs + ±i) + a radix-2 combine with W_8 twiddles — only
// W_8^1=(c,-c) and W_8^3=(-c,-c) carry the c=√2/2 constant (W_8^0=1, W_8^2=(0,-1) are free). Per-butterfly PRE-twiddles from
// the full W_N[N] table. N/8 threads, stages UNROLLED (ping-pong). `inverse` conjugates (pre-twiddle wi→-wi, the 4-point i-
// rotation, and the W_8 combine all flip). 1/N scale is the caller's. Shared = 4·N floats (16·N B ≤ 48KB ⇒ N ≤ 3072 one-wg).
[[nodiscard]] inline Fft1dPlan build_fft1d_radix8(KGraph& g, int n, bool inverse = false, bool batched = false)
{
    const int   p8      = fft_log8(n);
    const int   eighth  = n / 8;
    const Shape sh1     = make_shape({1});
    const auto  ku      = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add     = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub     = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul     = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto  neg     = [&](int a) { return g.unary(KOp::Neg, a); };
    const int   cc      = g.constant(0.70710678118654752440, sh1, DType::F32); // sqrt(2)/2

    const int in_re  = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_im  = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re  = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im  = g.buffer_decl(DType::F32, 0, 3, false);
    const int out_re = g.buffer_decl(DType::F32, 0, 4, true);
    const int out_im = g.buffer_decl(DType::F32, 0, 5, true);
    const int a_re   = g.shared_decl(DType::F32, n);
    const int a_im   = g.shared_decl(DType::F32, n);
    const int b_re   = g.shared_decl(DType::F32, n);
    const int b_im   = g.shared_decl(DType::F32, n);
    const int tid    = g.builtin(KBuiltin::LocalInvocationIndex);
    const int base   = batched ? mul(g.builtin(KBuiltin::WorkgroupIndex), ku(static_cast<crd::u32>(n))) : -1;
    const auto boff  = [&](int idx) { return batched ? add(base, idx) : idx; };

    // complex mul (sr,si)*(wr,wi) → (outr,outi).
    const auto cmul = [&](int sr, int si, int wr, int wi, int& outr, int& outi) {
        outr = sub(mul(sr, wr), mul(si, wi));
        outi = add(mul(sr, wi), mul(si, wr));
    };
    // exact 4-point DIT DFT: p[0..3] → x[0..3] (no twiddles; ±i rotation flips on inverse).
    const auto dft4 = [&](const int* pr, const int* pi, int* xr, int* xi) {
        const int t0r = add(pr[0], pr[2]); const int t0i = add(pi[0], pi[2]);
        const int t1r = sub(pr[0], pr[2]); const int t1i = sub(pi[0], pi[2]);
        const int t2r = add(pr[1], pr[3]); const int t2i = add(pi[1], pi[3]);
        const int t3r = sub(pr[1], pr[3]); const int t3i = sub(pi[1], pi[3]);
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i);
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i);
        if (!inverse) { xr[1] = add(t1r, t3i); xi[1] = sub(t1i, t3r); xr[3] = sub(t1r, t3i); xi[3] = add(t1i, t3r); }
        else          { xr[1] = sub(t1r, t3i); xi[1] = add(t1i, t3r); xr[3] = add(t1r, t3i); xi[3] = sub(t1i, t3r); }
    };

    const int mark = g.kernel_stmt_mark();
    for (int m = 0; m < 8; ++m)
    {
        const int idx = add(tid, ku(static_cast<crd::u32>(m * eighth)));
        g.stmt_shared_store(a_re, idx, g.buffer_load(in_re, boff(idx)));
        g.stmt_shared_store(a_im, idx, g.buffer_load(in_im, boff(idx)));
    }
    g.stmt_barrier();

    for (int s = 0; s < p8; ++s)
    {
        const bool even = (s % 2) == 0;
        const int  sre  = even ? a_re : b_re;
        const int  sim  = even ? a_im : b_im;
        const int  dre  = even ? b_re : a_re;
        const int  dim  = even ? b_im : a_im;
        const int  rs   = 1 << (3 * s);       // 8^s
        const int  ll   = 1 << (3 * s + 3);   // 8^(s+1)
        const int  nl   = n / ll;
        const int  gidx = g.binary(KOp::Div, tid, ku(static_cast<crd::u32>(rs)));
        const int  jidx = g.binary(KOp::Mod, tid, ku(static_cast<crd::u32>(rs)));

        int ar[8];
        int ai[8];
        for (int m = 0; m < 8; ++m)
        {
            const int inm = add(add(mul(gidx, ku(static_cast<crd::u32>(rs))), jidx), ku(static_cast<crd::u32>(m * eighth)));
            const int sr  = g.shared_load(sre, inm);
            const int sii = g.shared_load(sim, inm);
            if (m == 0) { ar[0] = sr; ai[0] = sii; continue; }
            const int twidx = mul(jidx, ku(static_cast<crd::u32>(m * nl)));
            const int wr    = g.buffer_load(tw_re, twidx);
            int       wi    = g.buffer_load(tw_im, twidx);
            if (inverse) { wi = neg(wi); }
            cmul(sr, sii, wr, wi, ar[m], ai[m]);
        }
        // 8-point DFT: even/odd 4-DFTs + radix-2 combine with W_8^k.
        const int evr[4] = {ar[0], ar[2], ar[4], ar[6]};
        const int evi[4] = {ai[0], ai[2], ai[4], ai[6]};
        const int odr[4] = {ar[1], ar[3], ar[5], ar[7]};
        const int odi[4] = {ai[1], ai[3], ai[5], ai[7]};
        int       er[4]; int ei[4]; int orr[4]; int oii[4];
        dft4(evr, evi, er, ei);
        dft4(odr, odi, orr, oii);
        int wr8[4]; int wi8[4];
        wr8[0] = orr[0]; wi8[0] = oii[0];                                                        // W_8^0 = 1
        if (!inverse) { wr8[2] = oii[2]; wi8[2] = neg(orr[2]); } else { wr8[2] = neg(oii[2]); wi8[2] = orr[2]; } // W_8^2 = ∓i
        cmul(orr[1], oii[1], cc, inverse ? cc : neg(cc), wr8[1], wi8[1]);                         // W_8^1 = (c, ∓c)
        cmul(orr[3], oii[3], neg(cc), inverse ? cc : neg(cc), wr8[3], wi8[3]);                    // W_8^3 = (-c, ∓c)
        int xr[8]; int xi[8];
        for (int k = 0; k < 4; ++k)
        {
            xr[k]     = add(er[k], wr8[k]); xi[k]     = add(ei[k], wi8[k]);
            xr[k + 4] = sub(er[k], wr8[k]); xi[k + 4] = sub(ei[k], wi8[k]);
        }
        for (int k = 0; k < 8; ++k)
        {
            const int outk = add(add(mul(gidx, ku(static_cast<crd::u32>(ll))), jidx), ku(static_cast<crd::u32>(k * rs)));
            g.stmt_shared_store(dre, outk, xr[k]);
            g.stmt_shared_store(dim, outk, xi[k]);
        }
        g.stmt_barrier();
    }

    const bool odd = (p8 % 2) == 1;
    const int  fre = odd ? b_re : a_re;
    const int  fim = odd ? b_im : a_im;
    for (int m = 0; m < 8; ++m)
    {
        const int idx = add(tid, ku(static_cast<crd::u32>(m * eighth)));
        g.stmt_buffer_store(out_re, boff(idx), g.shared_load(fre, idx));
        g.stmt_buffer_store(out_im, boff(idx), g.shared_load(fim, idx));
    }

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(eighth);
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = 3 * p8;
    plan.inverse                 = inverse;
    return plan;
}

// ⭐⭐ THE REGISTER-BLOCKED radix-16 Stockham FFT — the ncu-profiled crush lever (2026-07-13). The radix-4 kernel is
// INSTRUCTION-BOUND (ncu: SM 57% vs cuFFT's 21%; 5 shared ping-pong stages shuttle every value through shared), so it is
// pinned at DRAM speed even when the image is L2-resident — while cuFFT's EPT<16> kernels (16 elements per thread in
// REGISTERS, 64-thread blocks, ~2 shared exchanges) have 3× compute headroom and run ~2× faster on a warm L2. This builder
// is that architecture in CKIR: **N/16 threads, each owning 16 points as SSA temps**; a 16-point butterfly per radix-16
// stage = pre-twiddle → two layers of the EXACT 4-pt DFT with W_16 twiddles between (W_16^{ce} = W_N^{ce·N/16}, compile-time
// indices into the SAME uploaded table — the radix-8 W_8 policy, so bit-exactness via `precise` is preserved). N = 4^p:
// p/2 radix-16 stages + (p odd) one radix-4 tail stage (each thread then does 4 butterflies) ⇒ 1024 = [16,16,4] = 3 shared
// exchanges instead of 5 (40% less LSU traffic), 64-thread blocks (more blocks/SM, better overlap), 16 independent global
// loads per thread (deep MLP). Same buffers as radix-4: in 0,1 · tw 2,3 (FULL W_N[N]) · out 4,5. `inverse` conjugates all
// three twiddle families (pre, W_16, and the DFT4 ±i rotations). 1/N is the caller's.
// `tile_c`/`col_stride` (>1 / >0): the TILE-STAGED COLUMN FFT (cuFFT's `regular_fft` layout) — ONE block does `tile_c`
// adjacent columns of a row-major image, so consecutive threads touch consecutive columns ⇒ the strided column access is
// COALESCED (the fix for the transpose-on-write's uncoalesced loss). local_size = tile_c·(n/16); a thread's column is
// `tid % tile_c` and its FFT-thread is `tid / tile_c`; global element `idx` maps to `idx*col_stride + (WorkgroupIndex*tile_c
// + col)`; each column's single-buffer exchange lives in its own `col*n` shared slice. All plain index arithmetic ⇒ every
// backend lowers it identically (CKIR-pure). Defaults tile_c=1,col_stride=0 = the plain contiguous batched FFT (unchanged).
[[nodiscard]] inline Fft1dPlan build_fft1d_radix16(KGraph& g, int n, bool inverse = false, bool batched = false,
                                                   int tile_c = 1, int col_stride = 0)
{
    const int   p4       = fft_log2(n) / 2; // log4(n); n must be 4^p4, p4 >= 2
    const int   n16      = p4 / 2;          // radix-16 stages
    const int   rem4     = p4 % 2;          // 0 or 1 radix-4 tail stage
    const int   nstages  = n16 + rem4;
    const int   tthreads = n / 16;          // FFT threads per column (one 16-pt butterfly per thread per radix-16 stage)
    const bool  tiled    = tile_c > 1;
    const Shape sh1      = make_shape({1});
    const auto  ku       = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add      = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub      = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul      = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto  neg      = [&](int a) { return g.unary(KOp::Neg, a); };

    const int in_re  = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_im  = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re  = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im  = g.buffer_decl(DType::F32, 0, 3, false);
    const int out_re = g.buffer_decl(DType::F32, 0, 4, true);
    const int out_im = g.buffer_decl(DType::F32, 0, 5, true);
    // ⭐ ONE shared (re,im) pair per column = 8 KB·tile_c (the ncu occupancy lever); a middle stage FREEZES its inputs into
    // registers (stmt_materialize) then barriers before overwriting X (register-residency, cuFFT's trick). Bit-exact.
    const int cstride = tiled ? (n + 1) : n; // +1 per-column PAD (n is a multiple of 32) ⇒ conflict-free shared banks
    const int x_re    = g.shared_decl(DType::F32, tile_c * cstride);
    const int x_im    = g.shared_decl(DType::F32, tile_c * cstride);
    const int rawtid  = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wgix    = g.builtin(KBuiltin::WorkgroupIndex);
    const int tid     = tiled ? g.binary(KOp::Div, rawtid, ku(static_cast<crd::u32>(tile_c))) : rawtid; // FFT thread
    const int col     = tiled ? g.binary(KOp::Mod, rawtid, ku(static_cast<crd::u32>(tile_c))) : -1;     // column in the tile
    // tiled column base. BATCHED+STRIDED (batched && col_stride>0): the grid is (cols/tile_c)·B over B contiguous rows×cols
    // images, so WorkgroupIndex splits into (image, col-tile) and gcol carries the per-image element base (image·rows·cols +
    // in-image column). Single-image (batched=false): gcol = WorkgroupIndex·tile_c + col (unchanged). Mirrors the batched
    // strided column-conv's split (build_fft1d_convolution16_tiled) so a plain batched 2-D FFT reuses the coalesced tile.
    int gcol = -1;
    if (tiled)
    {
        if (batched && col_stride > 0)
        {
            const int tpi     = col_stride / tile_c; // tiles per image (= cols/tile_c)
            const int bstride = n * col_stride;      // rows·cols elements per image
            const int image   = g.binary(KOp::Div, wgix, ku(static_cast<crd::u32>(tpi)));
            const int ctile   = g.binary(KOp::Mod, wgix, ku(static_cast<crd::u32>(tpi)));
            gcol              = add(mul(image, ku(static_cast<crd::u32>(bstride))), add(mul(ctile, ku(static_cast<crd::u32>(tile_c))), col));
        }
        else { gcol = add(mul(wgix, ku(static_cast<crd::u32>(tile_c))), col); }
    }
    int base = -1; // contiguous batched base (one image per workgroup); -1 = unused (tiled/strided compute the offset in boff)
    if (!tiled && col_stride <= 0 && batched) { base = mul(wgix, ku(static_cast<crd::u32>(n))); }
    // global element offset: tiled/strided column of a row-major image, else contiguous batched. filter/col share it.
    const auto boff = [&](int idx) {
        if (col_stride > 0) { return add(mul(idx, ku(static_cast<crd::u32>(col_stride))), tiled ? gcol : wgix); }
        return batched ? add(base, idx) : idx;
    };
    // shared slot: each column owns the [col*cstride, +n) slice (padded so tile_c columns don't collide on banks).
    const auto sp = [&](int slot) { return tiled ? add(mul(col, ku(static_cast<crd::u32>(cstride))), slot) : slot; };

    const auto cmul = [&](int sr, int si, int wr, int wi, int& outr, int& outi) {
        outr = sub(mul(sr, wr), mul(si, wi));
        outi = add(mul(sr, wi), mul(si, wr));
    };
    // exact 4-point DIT DFT (adds/subs + ±i rotation; flips on inverse) — the radix-4/8 builders' proven core.
    const auto dft4 = [&](const int* pr, const int* pi, int* xr, int* xi) {
        const int t0r = add(pr[0], pr[2]); const int t0i = add(pi[0], pi[2]);
        const int t1r = sub(pr[0], pr[2]); const int t1i = sub(pi[0], pi[2]);
        const int t2r = add(pr[1], pr[3]); const int t2i = add(pi[1], pi[3]);
        const int t3r = sub(pr[1], pr[3]); const int t3i = sub(pi[1], pi[3]);
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i);
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i);
        if (!inverse) { xr[1] = add(t1r, t3i); xi[1] = sub(t1i, t3r); xr[3] = sub(t1r, t3i); xi[3] = add(t1i, t3r); }
        else          { xr[1] = sub(t1r, t3i); xi[1] = add(t1i, t3r); xr[3] = add(t1r, t3i); xi[3] = sub(t1i, t3r); }
    };
    // exact-as-possible 16-point DFT on pre-twiddled inputs a[0..15]: X[k] = sum_n a[n] W_16^{nk}, decomposed 16 = 4x4:
    // u_c[a] = a[4a+c] -> U_c = DFT4(u_c) -> V_c[e] = W_16^{ce} U_c[e] (table: W_N^{ce*N/16}) -> X[e+4d] = DFT4_over_c(V)[d].
    const auto dft16 = [&](const int* ar, const int* ai, int* xr, int* xi) {
        int ur[4][4]; int ui[4][4]; // U_c[e]
        for (int c = 0; c < 4; ++c)
        {
            const int pr[4] = {ar[c], ar[4 + c], ar[8 + c], ar[12 + c]};
            const int pi[4] = {ai[c], ai[4 + c], ai[8 + c], ai[12 + c]};
            dft4(pr, pi, ur[c], ui[c]);
        }
        for (int c = 1; c < 4; ++c) // V_c[e] = W_16^{ce} U_c[e]; c==0 or e==0 are identity
        {
            for (int e = 1; e < 4; ++e)
            {
                const int twidx = c * e * (n / 16);
                const int wr    = g.buffer_load(tw_re, ku(static_cast<crd::u32>(twidx)));
                int       wi    = g.buffer_load(tw_im, ku(static_cast<crd::u32>(twidx)));
                if (inverse) { wi = neg(wi); }
                cmul(ur[c][e], ui[c][e], wr, wi, ur[c][e], ui[c][e]);
            }
        }
        for (int e = 0; e < 4; ++e) // X[e+4d] = DFT4 over c of V_c[e]
        {
            const int pr[4] = {ur[0][e], ur[1][e], ur[2][e], ur[3][e]};
            const int pi[4] = {ui[0][e], ui[1][e], ui[2][e], ui[3][e]};
            int       yr[4]; int yi[4];
            dft4(pr, pi, yr, yi);
            for (int d = 0; d < 4; ++d) { xr[e + 4 * d] = yr[d]; xi[e + 4 * d] = yi[d]; }
        }
    };

    const int mark = g.kernel_stmt_mark();
    int       r    = 1; // cumulative Stockham group size
    for (int s = 0; s < nstages; ++s)
    {
        const bool first  = s == 0;
        const bool last   = s == nstages - 1;
        const bool r16    = s < n16;
        const int  radix  = r16 ? 16 : 4;
        const int  nbut   = r16 ? 1 : 4;         // butterflies per thread (radix-4 tail keeps all N/16 threads busy)
        const int  ll     = radix * r;
        const int  nl     = n / ll;
        const bool freeze = !first && !last;     // a middle stage reads X AND writes X ⇒ freeze inputs, barrier, then write

        // READ the thread's 16 raw inputs (from GLOBAL for stage 0, else from X). Freeze the shared reads of a middle stage.
        int rr[16]; int ri[16]; int gidxs[4]; int jidxs[4];
        for (int b = 0; b < nbut; ++b)
        {
            const int tb = (nbut == 1) ? tid : add(tid, ku(static_cast<crd::u32>(b * tthreads)));
            gidxs[b]     = g.binary(KOp::Div, tb, ku(static_cast<crd::u32>(r)));
            jidxs[b]     = g.binary(KOp::Mod, tb, ku(static_cast<crd::u32>(r)));
            for (int m = 0; m < radix; ++m)
            {
                const int leg = b * radix + m;
                const int inm = add(add(mul(gidxs[b], ku(static_cast<crd::u32>(r))), jidxs[b]), ku(static_cast<crd::u32>(m * (n / radix))));
                if (first) { rr[leg] = g.buffer_load(in_re, boff(inm)); ri[leg] = g.buffer_load(in_im, boff(inm)); }
                else
                {
                    rr[leg] = g.shared_load(x_re, sp(inm));
                    ri[leg] = g.shared_load(x_im, sp(inm));
                    if (freeze) { g.stmt_materialize(rr[leg]); g.stmt_materialize(ri[leg]); }
                }
            }
        }
        if (freeze) { g.stmt_barrier(); } // inputs frozen in registers ⇒ safe to overwrite X below

        // PRE-TWIDDLE + BUTTERFLY (registers) → xr[16], xi[16].
        int xr[16]; int xi[16];
        for (int b = 0; b < nbut; ++b)
        {
            int ar[16]; int ai[16];
            for (int m = 0; m < radix; ++m)
            {
                const int leg = b * radix + m;
                if (m == 0) { ar[0] = rr[leg]; ai[0] = ri[leg]; continue; }
                const int twidx = mul(jidxs[b], ku(static_cast<crd::u32>(m * nl))); // W_L^{jm} = W_N^{jm·N/L}
                const int wr    = g.buffer_load(tw_re, twidx);
                int       wi    = g.buffer_load(tw_im, twidx);
                if (inverse) { wi = neg(wi); }
                cmul(rr[leg], ri[leg], wr, wi, ar[m], ai[m]);
            }
            if (r16) { dft16(ar, ai, &xr[0], &xi[0]); }
            else { dft4(ar, ai, &xr[b * 4], &xi[b * 4]); }
        }

        // WRITE the thread's 16 outputs (to GLOBAL if last, else to X).
        for (int b = 0; b < nbut; ++b)
        {
            for (int k = 0; k < radix; ++k)
            {
                const int leg  = b * radix + k;
                const int outk = add(add(mul(gidxs[b], ku(static_cast<crd::u32>(ll))), jidxs[b]), ku(static_cast<crd::u32>(k * r)));
                if (last) { g.stmt_buffer_store(out_re, boff(outk), xr[leg]); g.stmt_buffer_store(out_im, boff(outk), xi[leg]); }
                else { g.stmt_shared_store(x_re, sp(outk), xr[leg]); g.stmt_shared_store(x_im, sp(outk), xi[leg]); }
            }
        }
        if (!last) { g.stmt_barrier(); }
        r = ll;
    }

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(tile_c * tthreads); // tile_c columns × n/16 FFT-threads
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = 2 * p4;
    plan.inverse                 = inverse;
    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// REAL FFT (R2C / C2R) — a real N-point signal has a HERMITIAN spectrum (X[N-k] = conj(X[k])), so only the N/2+1 outputs
// c ∈ [0, N/2] are unique. Storing/processing only that half HALVES the downstream traffic + column-FFT work — the DRAM-bound
// crush multiplier for a REAL image (bloom is real-valued). These reuse the proven radix-16 forward/inverse core; the only
// changes are the I/O: R2C reads a real row (imag=0) and CONDITIONALLY stores the half; C2R HERMITIAN-EXPANDS the half back to
// the full spectrum on load (branchless: q = min(k, N-k), conjugate when k > N/2) and stores the real part only. Row w =
// WorkgroupIndex (batched over rows·B by the grid). `half_stride` = the half-spectrum row width (≥ N/2+1; pad to a tile_c
// multiple for the coalesced column conv). Authored in CKIR — index arithmetic + Min/Select/If, every backend lowers it
// identically. ⚠ the FFT compute stays full N-point (only the store is halved), so the row pass is bandwidth-saved not
// compute-saved; the big win is the HALF-WIDTH column conv it feeds.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

// R2C: real row (buffer (0,0), stride N) → half complex spectrum (out (0,3) re / (0,4) im, stride `half_stride`, cols 0..N/2).
[[nodiscard]] inline Fft1dPlan build_fft1d_r2c(KGraph& g, int n, int half_stride)
{
    const int   p4       = fft_log2(n) / 2;
    const int   n16      = p4 / 2;
    const int   rem4     = p4 % 2;
    const int   nstages  = n16 + rem4;
    const int   tthreads = n / 16;
    const int   half     = n / 2;
    const Shape sh1      = make_shape({1});
    const auto  ku       = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add      = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub      = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul      = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_re  = g.buffer_decl(DType::F32, 0, 0, false); // REAL input (no imag buffer)
    const int tw_re  = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_im  = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_re = g.buffer_decl(DType::F32, 0, 3, true);
    const int out_im = g.buffer_decl(DType::F32, 0, 4, true);
    const int x_re   = g.shared_decl(DType::F32, n);
    const int x_im   = g.shared_decl(DType::F32, n);
    const int tid    = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wrow   = g.builtin(KBuiltin::WorkgroupIndex);
    const int inbase = mul(wrow, ku(static_cast<crd::u32>(n)));
    const int obase  = mul(wrow, ku(static_cast<crd::u32>(half_stride)));
    const int zero   = g.constant(0.0, sh1, DType::F32);

    const auto cmul = [&](int sr, int si, int wr, int wi, int& outr, int& outi) {
        outr = sub(mul(sr, wr), mul(si, wi));
        outi = add(mul(sr, wi), mul(si, wr));
    };
    const auto dft4 = [&](const int* pr, const int* pi, int* xr, int* xi) {
        const int t0r = add(pr[0], pr[2]); const int t0i = add(pi[0], pi[2]);
        const int t1r = sub(pr[0], pr[2]); const int t1i = sub(pi[0], pi[2]);
        const int t2r = add(pr[1], pr[3]); const int t2i = add(pi[1], pi[3]);
        const int t3r = sub(pr[1], pr[3]); const int t3i = sub(pi[1], pi[3]);
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i);
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i);
        xr[1] = add(t1r, t3i); xi[1] = sub(t1i, t3r); xr[3] = sub(t1r, t3i); xi[3] = add(t1i, t3r);
    };
    const auto dft16 = [&](const int* ar, const int* ai, int* xr, int* xi) {
        int ur[4][4]; int ui[4][4];
        for (int c = 0; c < 4; ++c)
        {
            const int pr[4] = {ar[c], ar[4 + c], ar[8 + c], ar[12 + c]};
            const int pi[4] = {ai[c], ai[4 + c], ai[8 + c], ai[12 + c]};
            dft4(pr, pi, ur[c], ui[c]);
        }
        for (int c = 1; c < 4; ++c)
        {
            for (int e = 1; e < 4; ++e)
            {
                const int twidx = c * e * (n / 16);
                const int wr    = g.buffer_load(tw_re, ku(static_cast<crd::u32>(twidx)));
                const int wi    = g.buffer_load(tw_im, ku(static_cast<crd::u32>(twidx)));
                cmul(ur[c][e], ui[c][e], wr, wi, ur[c][e], ui[c][e]);
            }
        }
        for (int e = 0; e < 4; ++e)
        {
            const int pr[4] = {ur[0][e], ur[1][e], ur[2][e], ur[3][e]};
            const int pi[4] = {ui[0][e], ui[1][e], ui[2][e], ui[3][e]};
            int       yr[4]; int yi[4];
            dft4(pr, pi, yr, yi);
            for (int d = 0; d < 4; ++d) { xr[e + 4 * d] = yr[d]; xi[e + 4 * d] = yi[d]; }
        }
    };

    const int mark = g.kernel_stmt_mark();
    int       r    = 1;
    for (int s = 0; s < nstages; ++s)
    {
        const bool first  = s == 0;
        const bool last   = s == nstages - 1;
        const bool r16    = s < n16;
        const int  radix  = r16 ? 16 : 4;
        const int  nbut   = r16 ? 1 : 4;
        const int  ll     = radix * r;
        const int  nl     = n / ll;
        const bool freeze = !first && !last;

        int rr[16]; int ri[16]; int gidxs[4]; int jidxs[4];
        for (int b = 0; b < nbut; ++b)
        {
            const int tb = (nbut == 1) ? tid : add(tid, ku(static_cast<crd::u32>(b * tthreads)));
            gidxs[b]     = g.binary(KOp::Div, tb, ku(static_cast<crd::u32>(r)));
            jidxs[b]     = g.binary(KOp::Mod, tb, ku(static_cast<crd::u32>(r)));
            for (int m = 0; m < radix; ++m)
            {
                const int leg = b * radix + m;
                const int inm = add(add(mul(gidxs[b], ku(static_cast<crd::u32>(r))), jidxs[b]), ku(static_cast<crd::u32>(m * (n / radix))));
                if (first) { rr[leg] = g.buffer_load(in_re, add(inbase, inm)); ri[leg] = zero; } // REAL input ⇒ imag 0
                else
                {
                    rr[leg] = g.shared_load(x_re, inm);
                    ri[leg] = g.shared_load(x_im, inm);
                    if (freeze) { g.stmt_materialize(rr[leg]); g.stmt_materialize(ri[leg]); }
                }
            }
        }
        if (freeze) { g.stmt_barrier(); }

        int xr[16]; int xi[16];
        for (int b = 0; b < nbut; ++b)
        {
            int ar[16]; int ai[16];
            for (int m = 0; m < radix; ++m)
            {
                const int leg = b * radix + m;
                if (m == 0) { ar[0] = rr[leg]; ai[0] = ri[leg]; continue; }
                const int twidx = mul(jidxs[b], ku(static_cast<crd::u32>(m * nl)));
                const int wr    = g.buffer_load(tw_re, twidx);
                const int wi    = g.buffer_load(tw_im, twidx);
                cmul(rr[leg], ri[leg], wr, wi, ar[m], ai[m]);
            }
            if (r16) { dft16(ar, ai, &xr[0], &xi[0]); }
            else { dft4(ar, ai, &xr[b * 4], &xi[b * 4]); }
        }

        // FREEZE every value/base the conditional stores touch into enclosing-scope temps BEFORE the per-output `if` blocks:
        // the GLSL emitter declares a lazy temp at first use, so a node first used INSIDE if-block-0 (a shared store base, or a
        // butterfly intermediate the dft4/dft16 share across outputs) is out of scope in if-block-1. Materialize hoists them.
        if (last)
        {
            g.stmt_materialize(obase);
            for (int b = 0; b < nbut; ++b) { g.stmt_materialize(gidxs[b]); g.stmt_materialize(jidxs[b]); }
            for (int j = 0; j < nbut * radix; ++j) { g.stmt_materialize(xr[j]); g.stmt_materialize(xi[j]); }
        }
        for (int b = 0; b < nbut; ++b)
        {
            for (int k = 0; k < radix; ++k)
            {
                const int leg  = b * radix + k;
                const int outk = add(add(mul(gidxs[b], ku(static_cast<crd::u32>(ll))), jidxs[b]), ku(static_cast<crd::u32>(k * r)));
                if (last) // CONDITIONAL half-store: only the Hermitian-unique columns c ∈ [0, N/2] are written
                {
                    const int cond = g.binary(KOp::CmpLe, outk, ku(static_cast<crd::u32>(half)));
                    const int ifid = g.stmt_if_begin(cond);
                    g.stmt_buffer_store(out_re, add(obase, outk), xr[leg]);
                    g.stmt_buffer_store(out_im, add(obase, outk), xi[leg]);
                    g.stmt_if_end(ifid);
                }
                else { g.stmt_shared_store(x_re, outk, xr[leg]); g.stmt_shared_store(x_im, outk, xi[leg]); }
            }
        }
        if (!last) { g.stmt_barrier(); }
        r = ll;
    }

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(tthreads);
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = 2 * p4;
    plan.inverse                 = false;
    return plan;
}

// C2R: half complex spectrum (in (0,0) re / (0,1) im, stride `half_stride`, cols 0..N/2) → real row (out (0,4), stride N).
// HERMITIAN-EXPAND on load (q = min(k, N-k); conjugate imag when k > N/2), inverse N-point FFT, store the real part only.
[[nodiscard]] inline Fft1dPlan build_fft1d_c2r(KGraph& g, int n, int half_stride)
{
    const int   p4       = fft_log2(n) / 2;
    const int   n16      = p4 / 2;
    const int   rem4     = p4 % 2;
    const int   nstages  = n16 + rem4;
    const int   tthreads = n / 16;
    const int   half     = n / 2;
    const Shape sh1      = make_shape({1});
    const auto  ku       = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add      = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  sub      = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto  mul      = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto  neg      = [&](int a) { return g.unary(KOp::Neg, a); };

    const int in_re  = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_im  = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re  = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im  = g.buffer_decl(DType::F32, 0, 3, false);
    const int out_re = g.buffer_decl(DType::F32, 0, 4, true); // REAL output (no imag buffer)
    const int x_re   = g.shared_decl(DType::F32, n);
    const int x_im   = g.shared_decl(DType::F32, n);
    const int tid    = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wrow   = g.builtin(KBuiltin::WorkgroupIndex);
    const int inbase = mul(wrow, ku(static_cast<crd::u32>(half_stride)));
    const int obase  = mul(wrow, ku(static_cast<crd::u32>(n)));

    const auto cmul = [&](int sr, int si, int wr, int wi, int& outr, int& outi) {
        outr = sub(mul(sr, wr), mul(si, wi));
        outi = add(mul(sr, wi), mul(si, wr));
    };
    const auto dft4 = [&](const int* pr, const int* pi, int* xr, int* xi) {
        const int t0r = add(pr[0], pr[2]); const int t0i = add(pi[0], pi[2]);
        const int t1r = sub(pr[0], pr[2]); const int t1i = sub(pi[0], pi[2]);
        const int t2r = add(pr[1], pr[3]); const int t2i = add(pi[1], pi[3]);
        const int t3r = sub(pr[1], pr[3]); const int t3i = sub(pi[1], pi[3]);
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i);
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i);
        xr[1] = sub(t1r, t3i); xi[1] = add(t1i, t3r); xr[3] = add(t1r, t3i); xi[3] = sub(t1i, t3r); // inverse rotation
    };
    const auto dft16 = [&](const int* ar, const int* ai, int* xr, int* xi) {
        int ur[4][4]; int ui[4][4];
        for (int c = 0; c < 4; ++c)
        {
            const int pr[4] = {ar[c], ar[4 + c], ar[8 + c], ar[12 + c]};
            const int pi[4] = {ai[c], ai[4 + c], ai[8 + c], ai[12 + c]};
            dft4(pr, pi, ur[c], ui[c]);
        }
        for (int c = 1; c < 4; ++c)
        {
            for (int e = 1; e < 4; ++e)
            {
                const int twidx = c * e * (n / 16);
                const int wr    = g.buffer_load(tw_re, ku(static_cast<crd::u32>(twidx)));
                const int wi    = neg(g.buffer_load(tw_im, ku(static_cast<crd::u32>(twidx)))); // inverse ⇒ conj twiddle
                cmul(ur[c][e], ui[c][e], wr, wi, ur[c][e], ui[c][e]);
            }
        }
        for (int e = 0; e < 4; ++e)
        {
            const int pr[4] = {ur[0][e], ur[1][e], ur[2][e], ur[3][e]};
            const int pi[4] = {ui[0][e], ui[1][e], ui[2][e], ui[3][e]};
            int       yr[4]; int yi[4];
            dft4(pr, pi, yr, yi);
            for (int d = 0; d < 4; ++d) { xr[e + 4 * d] = yr[d]; xi[e + 4 * d] = yi[d]; }
        }
    };

    const int mark = g.kernel_stmt_mark();
    int       r    = 1;
    for (int s = 0; s < nstages; ++s)
    {
        const bool first  = s == 0;
        const bool last   = s == nstages - 1;
        const bool r16    = s < n16;
        const int  radix  = r16 ? 16 : 4;
        const int  nbut   = r16 ? 1 : 4;
        const int  ll     = radix * r;
        const int  nl     = n / ll;
        const bool freeze = !first && !last;

        int rr[16]; int ri[16]; int gidxs[4]; int jidxs[4];
        for (int b = 0; b < nbut; ++b)
        {
            const int tb = (nbut == 1) ? tid : add(tid, ku(static_cast<crd::u32>(b * tthreads)));
            gidxs[b]     = g.binary(KOp::Div, tb, ku(static_cast<crd::u32>(r)));
            jidxs[b]     = g.binary(KOp::Mod, tb, ku(static_cast<crd::u32>(r)));
            for (int m = 0; m < radix; ++m)
            {
                const int leg = b * radix + m;
                const int inm = add(add(mul(gidxs[b], ku(static_cast<crd::u32>(r))), jidxs[b]), ku(static_cast<crd::u32>(m * (n / radix))));
                if (first) // HERMITIAN-EXPAND: input index inm ∈ [0,N) reads stored q = min(inm, N-inm), conj if inm > N/2
                {
                    const int q    = g.binary(KOp::Min, inm, sub(ku(static_cast<crd::u32>(n)), inm));
                    const int re   = g.buffer_load(in_re, add(inbase, q));
                    const int imq  = g.buffer_load(in_im, add(inbase, q));
                    const int cond = g.binary(KOp::CmpGt, inm, ku(static_cast<crd::u32>(half)));
                    rr[leg]        = re;
                    ri[leg]        = g.select(cond, neg(imq), imq);
                }
                else
                {
                    rr[leg] = g.shared_load(x_re, inm);
                    ri[leg] = g.shared_load(x_im, inm);
                    if (freeze) { g.stmt_materialize(rr[leg]); g.stmt_materialize(ri[leg]); }
                }
            }
        }
        if (freeze) { g.stmt_barrier(); }

        int xr[16]; int xi[16];
        for (int b = 0; b < nbut; ++b)
        {
            int ar[16]; int ai[16];
            for (int m = 0; m < radix; ++m)
            {
                const int leg = b * radix + m;
                if (m == 0) { ar[0] = rr[leg]; ai[0] = ri[leg]; continue; }
                const int twidx = mul(jidxs[b], ku(static_cast<crd::u32>(m * nl)));
                const int wr    = g.buffer_load(tw_re, twidx);
                const int wi    = neg(g.buffer_load(tw_im, twidx)); // inverse
                cmul(rr[leg], ri[leg], wr, wi, ar[m], ai[m]);
            }
            if (r16) { dft16(ar, ai, &xr[0], &xi[0]); }
            else { dft4(ar, ai, &xr[b * 4], &xi[b * 4]); }
        }

        for (int b = 0; b < nbut; ++b)
        {
            for (int k = 0; k < radix; ++k)
            {
                const int leg  = b * radix + k;
                const int outk = add(add(mul(gidxs[b], ku(static_cast<crd::u32>(ll))), jidxs[b]), ku(static_cast<crd::u32>(k * r)));
                if (last) { g.stmt_buffer_store(out_re, add(obase, outk), xr[leg]); } // REAL part only (imag ≈ 0, discarded)
                else { g.stmt_shared_store(x_re, outk, xr[leg]); g.stmt_shared_store(x_im, outk, xi[leg]); }
            }
        }
        if (!last) { g.stmt_barrier(); }
        r = ll;
    }

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(tthreads);
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = 2 * p4;
    plan.inverse                 = true;
    return plan;
}

// The REGISTER-BLOCKED radix-16 FUSED FFT-convolution — the radix-16 edition of `build_fft1d_convolution` (same contract:
// fwd FFT → ×filter spectrum → inverse FFT → `scale`, ONE dispatch; buffers in 0,1 · tw 2,3 · filt 4,5 · out 6,7;
// `batched_filter` = per-workgroup filter slice). n = 4^p, N/16 threads, [16..16,4?] stages — ~7 barriers instead of 11
// and 40% less shared/LSU traffic, so the kernel is memory- not instruction-bound (the ncu-profiled crush lever).
// `col_stride` (>0, batched only): the thread's global element `idx` maps to `idx*col_stride + WorkgroupIndex` instead of
// `WorkgroupIndex*n + idx` — i.e. this signal is a STRIDED COLUMN of a row-major image (in/out/filter all row-major). This is
// the TRANSPOSE-ON-WRITE fusion: the 2-D conv's column pass reads/writes the image in place, no separate transpose kernel.
// ⚠ the column access is UNCOALESCED (consecutive threads stride by `col_stride`) — measure vs the coalesced 7-dispatch path.
[[nodiscard]] inline Fft1dPlan build_fft1d_convolution16(KGraph& g, int n, bool batched = false, crd::f64 scale = -1.0,
                                                         bool batched_filter = false, int col_stride = 0)
{
    const int      p4       = fft_log2(n) / 2;
    const int      n16      = p4 / 2;
    const int      rem4     = p4 % 2;
    const int      nstages  = n16 + rem4;
    const int      tthreads = n / 16;
    const Shape    sh1      = make_shape({1});
    const crd::f64 sc       = scale < 0.0 ? 1.0 / static_cast<crd::f64>(n) : scale;
    const auto     ku       = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto     add      = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto     sub      = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto     mul      = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto     neg      = [&](int a) { return g.unary(KOp::Neg, a); };

    const int in_re   = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_im   = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re   = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im   = g.buffer_decl(DType::F32, 0, 3, false);
    const int filt_re = g.buffer_decl(DType::F32, 0, 4, false);
    const int filt_im = g.buffer_decl(DType::F32, 0, 5, false);
    const int out_re  = g.buffer_decl(DType::F32, 0, 6, true);
    const int out_im  = g.buffer_decl(DType::F32, 0, 7, true);
    const int a_re    = g.shared_decl(DType::F32, n);
    const int a_im    = g.shared_decl(DType::F32, n);
    const int b_re    = g.shared_decl(DType::F32, n);
    const int b_im    = g.shared_decl(DType::F32, n);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int  wgcol     = (col_stride > 0) ? g.builtin(KBuiltin::WorkgroupIndex) : -1; // the image column this block owns
    const int  base      = (col_stride <= 0 && batched) ? mul(g.builtin(KBuiltin::WorkgroupIndex), ku(static_cast<crd::u32>(n))) : -1;
    // strided: idx*col_stride + column (row-major column of an image). contiguous: WorkgroupIndex*n + idx. filter shares it.
    const auto boff      = [&](int idx) { if (col_stride > 0) { return add(mul(idx, ku(static_cast<crd::u32>(col_stride))), wgcol); } return batched ? add(base, idx) : idx; };
    const auto boff_filt = [&](int idx) { if (col_stride > 0) { return add(mul(idx, ku(static_cast<crd::u32>(col_stride))), wgcol); } return (batched && batched_filter) ? add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(static_cast<crd::u32>(n))), idx) : idx; };

    int        cre = a_re; int cim = a_im; int ore = b_re; int oim = b_im;
    const auto flip = [&]() { const int tr = cre; cre = ore; ore = tr; const int ti = cim; cim = oim; oim = ti; };

    const auto cmul = [&](int sr, int si, int wr, int wi, int& outr, int& outi) {
        outr = sub(mul(sr, wr), mul(si, wi));
        outi = add(mul(sr, wi), mul(si, wr));
    };
    const auto dft4 = [&](bool inv, const int* pr, const int* pi, int* xr, int* xi) {
        const int t0r = add(pr[0], pr[2]); const int t0i = add(pi[0], pi[2]);
        const int t1r = sub(pr[0], pr[2]); const int t1i = sub(pi[0], pi[2]);
        const int t2r = add(pr[1], pr[3]); const int t2i = add(pi[1], pi[3]);
        const int t3r = sub(pr[1], pr[3]); const int t3i = sub(pi[1], pi[3]);
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i);
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i);
        if (!inv) { xr[1] = add(t1r, t3i); xi[1] = sub(t1i, t3r); xr[3] = sub(t1r, t3i); xi[3] = add(t1i, t3r); }
        else      { xr[1] = sub(t1r, t3i); xi[1] = add(t1i, t3r); xr[3] = add(t1r, t3i); xi[3] = sub(t1i, t3r); }
    };
    const auto dft16 = [&](bool inv, const int* ar, const int* ai, int* xr, int* xi) {
        int ur[4][4]; int ui[4][4];
        for (int c = 0; c < 4; ++c)
        {
            const int pr[4] = {ar[c], ar[4 + c], ar[8 + c], ar[12 + c]};
            const int pi[4] = {ai[c], ai[4 + c], ai[8 + c], ai[12 + c]};
            dft4(inv, pr, pi, ur[c], ui[c]);
        }
        for (int c = 1; c < 4; ++c)
        {
            for (int e = 1; e < 4; ++e)
            {
                const int twidx = c * e * (n / 16);
                const int wr    = g.buffer_load(tw_re, ku(static_cast<crd::u32>(twidx)));
                int       wi    = g.buffer_load(tw_im, ku(static_cast<crd::u32>(twidx)));
                if (inv) { wi = neg(wi); }
                cmul(ur[c][e], ui[c][e], wr, wi, ur[c][e], ui[c][e]);
            }
        }
        for (int e = 0; e < 4; ++e)
        {
            const int pr[4] = {ur[0][e], ur[1][e], ur[2][e], ur[3][e]};
            const int pi[4] = {ui[0][e], ui[1][e], ui[2][e], ui[3][e]};
            int       yr[4]; int yi[4];
            dft4(inv, pr, pi, yr, yi);
            for (int d = 0; d < 4; ++d) { xr[e + 4 * d] = yr[d]; xi[e + 4 * d] = yi[d]; }
        }
    };
    const int invn = g.constant(sc, sh1, DType::F32);
    // one stage (radix-16 for s < n16, else the radix-4 tail) reading CURRENT (or GLOBAL when `first_g`), writing OTHER
    // (or GLOBAL·scale when `last_g`); `fuse_filt` multiplies each output by the filter spectrum AT its absolute index
    // (elementwise, so it composes with any output permutation) — the fwd-last stage absorbs the whole multiply pass.
    const auto stage = [&](int s, int rr, bool inv, bool first_g, bool fuse_filt, bool last_g) -> int {
        const auto ld = [&](int idx, bool im) {
            if (first_g) { return g.buffer_load(im ? in_im : in_re, boff(idx)); }
            return g.shared_load(im ? cim : cre, idx);
        };
        const auto st = [&](int idx, int vre, int vim) {
            if (fuse_filt) // (vre,vim) × filt[idx]
            {
                const int fr = g.buffer_load(filt_re, boff_filt(idx));
                const int fi = g.buffer_load(filt_im, boff_filt(idx));
                int       pr = 0; int pi = 0;
                cmul(vre, vim, fr, fi, pr, pi);
                vre = pr; vim = pi;
            }
            if (last_g)
            {
                g.stmt_buffer_store(out_re, boff(idx), mul(vre, invn));
                g.stmt_buffer_store(out_im, boff(idx), mul(vim, invn));
                return;
            }
            g.stmt_shared_store(ore, idx, vre);
            g.stmt_shared_store(oim, idx, vim);
        };
        if (s < n16)
        {
            const int ll   = 16 * rr;
            const int nl   = n / ll;
            const int gidx = g.binary(KOp::Div, tid, ku(static_cast<crd::u32>(rr)));
            const int jidx = g.binary(KOp::Mod, tid, ku(static_cast<crd::u32>(rr)));
            int       ar16[16]; int ai16[16];
            for (int m = 0; m < 16; ++m)
            {
                const int inm = add(add(mul(gidx, ku(static_cast<crd::u32>(rr))), jidx), ku(static_cast<crd::u32>(m * tthreads)));
                const int sr  = ld(inm, false);
                const int sii = ld(inm, true);
                if (m == 0) { ar16[0] = sr; ai16[0] = sii; continue; }
                const int twidx = mul(jidx, ku(static_cast<crd::u32>(m * nl)));
                const int wr    = g.buffer_load(tw_re, twidx);
                int       wi    = g.buffer_load(tw_im, twidx);
                if (inv) { wi = neg(wi); }
                cmul(sr, sii, wr, wi, ar16[m], ai16[m]);
            }
            int xr16[16]; int xi16[16];
            dft16(inv, ar16, ai16, xr16, xi16);
            for (int k = 0; k < 16; ++k)
            {
                const int outk = add(add(mul(gidx, ku(static_cast<crd::u32>(ll))), jidx), ku(static_cast<crd::u32>(k * rr)));
                st(outk, xr16[k], xi16[k]);
            }
            if (!last_g) { g.stmt_barrier(); }
            return ll;
        }
        const int ll = 4 * rr;
        const int nl = n / ll;
        for (int b = 0; b < 4; ++b)
        {
            const int tb   = add(tid, ku(static_cast<crd::u32>(b * tthreads)));
            const int gidx = g.binary(KOp::Div, tb, ku(static_cast<crd::u32>(rr)));
            const int jidx = g.binary(KOp::Mod, tb, ku(static_cast<crd::u32>(rr)));
            int       ar4[4]; int ai4[4];
            for (int m = 0; m < 4; ++m)
            {
                const int inm = add(add(mul(gidx, ku(static_cast<crd::u32>(rr))), jidx), ku(static_cast<crd::u32>(m * (n / 4))));
                const int sr  = ld(inm, false);
                const int sii = ld(inm, true);
                if (m == 0) { ar4[0] = sr; ai4[0] = sii; continue; }
                const int twidx = mul(jidx, ku(static_cast<crd::u32>(m * nl)));
                const int wr    = g.buffer_load(tw_re, twidx);
                int       wi    = g.buffer_load(tw_im, twidx);
                if (inv) { wi = neg(wi); }
                cmul(sr, sii, wr, wi, ar4[m], ai4[m]);
            }
            int xr4[4]; int xi4[4];
            dft4(inv, ar4, ai4, xr4, xi4);
            for (int k = 0; k < 4; ++k)
            {
                const int outk = add(add(mul(gidx, ku(static_cast<crd::u32>(ll))), jidx), ku(static_cast<crd::u32>(k * rr)));
                st(outk, xr4[k], xi4[k]);
            }
        }
        if (!last_g) { g.stmt_barrier(); }
        return ll;
    };

    // DIRECT GLOBAL I/O + FUSED MULTIPLY (the ncu levers): fwd stage 0 reads global; the fwd-LAST stage multiplies by the
    // filter as it stores (no separate multiply pass); the inv-LAST stage writes global ×scale. Barriers: 2·nstages−1
    // (was 2·nstages+3). Same values in the same order ⇒ bit-exactness unchanged.
    const int mark = g.kernel_stmt_mark();
    int       r    = 1;
    for (int s = 0; s < nstages; ++s) { r = stage(s, r, false, s == 0, s == nstages - 1, false); flip(); } // FORWARD ×filt
    r = 1;
    for (int s = 0; s < nstages; ++s) { r = stage(s, r, true, false, false, s == nstages - 1); flip(); } // INVERSE ×scale

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(tthreads);
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = 2 * p4;
    plan.inverse                 = false;
    return plan;
}

// ⭐⭐⭐ THE TILED FUSED FFT-CONVOLUTION — cuFFT's `regular_fft` layout in CKIR: ONE block does `tile_c` ADJACENT columns of a
// row-major image, so the strided column access is COALESCED (consecutive threads → consecutive columns), and the whole
// fwd-FFT · ×filter · inv-FFT stays on-chip (single 8 KB·tile_c shared, register-blocked radix-16, `Materialize` freeze).
// This is the transpose-on-write done RIGHT — no transpose passes, no scratch, coalesced. `col_stride`>0 = column stride =
// cols; `tile_c` columns/block (shared caps it: 8 KB·tile_c ≤ device limit). ×filter is fused into the fwd-LAST stage's
// shared write (filter row-major, filt[bin*cols+col]); ×`scale` (1/(R·C)) into the inv-LAST global write. Bit-exact via
// `precise` + table twiddles. Buffers in 0,1 · tw 2,3 · filt 4,5 · out 6,7. All plain index arithmetic ⇒ every backend
// lowers it identically (CKIR-pure).
[[nodiscard]] inline Fft1dPlan build_fft1d_convolution16_tiled(KGraph& g, int n, int tile_c, crd::f64 scale, int col_stride,
                                                               bool batched = false)
{
    const int      p4       = fft_log2(n) / 2;
    const int      n16      = p4 / 2;
    const int      rem4     = p4 % 2;
    const int      nstages  = n16 + rem4;
    const int      tthreads = n / 16;
    const crd::f64 sc       = scale < 0.0 ? 1.0 / static_cast<crd::f64>(n) : scale;
    const Shape    sh1      = make_shape({1});
    const auto     ku       = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto     add      = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto     sub      = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto     mul      = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto     neg      = [&](int a) { return g.unary(KOp::Neg, a); };

    const int in_re   = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_im   = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re   = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im   = g.buffer_decl(DType::F32, 0, 3, false);
    const int filt_re = g.buffer_decl(DType::F32, 0, 4, false);
    const int filt_im = g.buffer_decl(DType::F32, 0, 5, false);
    const int out_re  = g.buffer_decl(DType::F32, 0, 6, true);
    const int out_im  = g.buffer_decl(DType::F32, 0, 7, true);
    const int cstride = n + 1; // +1 per-column PAD: n is a multiple of 32, so col*n would map every column to the SAME shared
                               // banks (tile_c-way conflict); col*(n+1) staggers them ⇒ conflict-free.
    const int x_re    = g.shared_decl(DType::F32, tile_c * cstride);
    const int x_im    = g.shared_decl(DType::F32, tile_c * cstride);
    const int rawtid  = g.builtin(KBuiltin::LocalInvocationIndex);
    const int ftid    = g.binary(KOp::Div, rawtid, ku(static_cast<crd::u32>(tile_c)));
    const int col     = g.binary(KOp::Mod, rawtid, ku(static_cast<crd::u32>(tile_c)));
    const int wgi     = g.builtin(KBuiltin::WorkgroupIndex);
    // BATCHED: the grid is (cols/tile_c)·B blocks over B contiguous images; the WorkgroupIndex splits into (image, col-tile)
    // so `gcol` carries the per-image base offset while `gcol_flt` stays the in-image column — the PSF filter is ONE spectrum
    // shared by every image, so it is indexed WITHOUT the image offset. Non-batched: image=0 ⇒ gcol==gcol_flt (identical IR).
    int        gcol;     // image data column (image·batch_stride + in-image column)
    int        gcol_flt; // filter column (in-image column only)
    if (batched)
    {
        const int tpi     = col_stride / tile_c; // tiles per image
        const int bstride = n * col_stride;      // rows·cols elements per image
        const int image   = g.binary(KOp::Div, wgi, ku(static_cast<crd::u32>(tpi)));
        const int ctile   = g.binary(KOp::Mod, wgi, ku(static_cast<crd::u32>(tpi)));
        gcol_flt          = add(mul(ctile, ku(static_cast<crd::u32>(tile_c))), col);
        gcol              = add(mul(image, ku(static_cast<crd::u32>(bstride))), gcol_flt);
    }
    else
    {
        gcol_flt = add(mul(wgi, ku(static_cast<crd::u32>(tile_c))), col);
        gcol     = gcol_flt;
    }
    const auto boff = [&](int idx) { return add(mul(idx, ku(static_cast<crd::u32>(col_stride))), gcol); };     // coalesced column
    const auto foff = [&](int idx) { return add(mul(idx, ku(static_cast<crd::u32>(col_stride))), gcol_flt); }; // shared filter
    const auto sp   = [&](int slot) { return add(mul(col, ku(static_cast<crd::u32>(cstride))), slot); };       // padded per-column slice
    const int  invn   = g.constant(sc, sh1, DType::F32);

    const auto cmul = [&](int sr, int si, int wr, int wi, int& outr, int& outi) {
        outr = sub(mul(sr, wr), mul(si, wi));
        outi = add(mul(sr, wi), mul(si, wr));
    };
    const auto dft4 = [&](bool inv, const int* pr, const int* pi, int* xr, int* xi) {
        const int t0r = add(pr[0], pr[2]); const int t0i = add(pi[0], pi[2]);
        const int t1r = sub(pr[0], pr[2]); const int t1i = sub(pi[0], pi[2]);
        const int t2r = add(pr[1], pr[3]); const int t2i = add(pi[1], pi[3]);
        const int t3r = sub(pr[1], pr[3]); const int t3i = sub(pi[1], pi[3]);
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i);
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i);
        if (!inv) { xr[1] = add(t1r, t3i); xi[1] = sub(t1i, t3r); xr[3] = sub(t1r, t3i); xi[3] = add(t1i, t3r); }
        else      { xr[1] = sub(t1r, t3i); xi[1] = add(t1i, t3r); xr[3] = add(t1r, t3i); xi[3] = sub(t1i, t3r); }
    };
    const auto dft16 = [&](bool inv, const int* ar, const int* ai, int* xr, int* xi) {
        int ur[4][4]; int ui[4][4];
        for (int c = 0; c < 4; ++c)
        {
            const int pr[4] = {ar[c], ar[4 + c], ar[8 + c], ar[12 + c]};
            const int pi[4] = {ai[c], ai[4 + c], ai[8 + c], ai[12 + c]};
            dft4(inv, pr, pi, ur[c], ui[c]);
        }
        for (int c = 1; c < 4; ++c)
        {
            for (int e = 1; e < 4; ++e)
            {
                const int twidx = c * e * (n / 16);
                const int wr    = g.buffer_load(tw_re, ku(static_cast<crd::u32>(twidx)));
                int       wi    = g.buffer_load(tw_im, ku(static_cast<crd::u32>(twidx)));
                if (inv) { wi = neg(wi); }
                cmul(ur[c][e], ui[c][e], wr, wi, ur[c][e], ui[c][e]);
            }
        }
        for (int e = 0; e < 4; ++e)
        {
            const int pr[4] = {ur[0][e], ur[1][e], ur[2][e], ur[3][e]};
            const int pi[4] = {ui[0][e], ui[1][e], ui[2][e], ui[3][e]};
            int       yr[4]; int yi[4];
            dft4(inv, pr, pi, yr, yi);
            for (int d = 0; d < 4; ++d) { xr[e + 4 * d] = yr[d]; xi[e + 4 * d] = yi[d]; }
        }
    };

    const int mark = g.kernel_stmt_mark();
    int       r    = 1;
    for (int si = 0; si < 2 * nstages; ++si) // nstages forward + nstages inverse, single-buffer, filter/scale fused
    {
        const bool inv     = si >= nstages;
        const int  s       = inv ? si - nstages : si;
        if (s == 0) { r = 1; }
        const bool rd_glob = si == 0;                 // fwd stage 0 reads the image
        const bool wr_glob = si == 2 * nstages - 1;   // inv last writes the image (×scale)
        const bool r16     = s < n16;
        const int  radix   = r16 ? 16 : 4;
        const int  nbut    = r16 ? 1 : 4;
        const int  ll      = radix * r;
        const int  nl      = n / ll;
        const bool freeze  = !rd_glob && !wr_glob;    // reads X and writes X ⇒ freeze inputs, barrier, then overwrite
        const bool fuse_f  = si == nstages - 1;       // fwd last stage multiplies by the filter as it writes X

        int rr[16]; int ri[16]; int gidxs[4]; int jidxs[4];
        for (int b = 0; b < nbut; ++b)
        {
            const int tb = (nbut == 1) ? ftid : add(ftid, ku(static_cast<crd::u32>(b * tthreads)));
            gidxs[b]     = g.binary(KOp::Div, tb, ku(static_cast<crd::u32>(r)));
            jidxs[b]     = g.binary(KOp::Mod, tb, ku(static_cast<crd::u32>(r)));
            for (int m = 0; m < radix; ++m)
            {
                const int leg = b * radix + m;
                const int inm = add(add(mul(gidxs[b], ku(static_cast<crd::u32>(r))), jidxs[b]), ku(static_cast<crd::u32>(m * (n / radix))));
                if (rd_glob) { rr[leg] = g.buffer_load(in_re, boff(inm)); ri[leg] = g.buffer_load(in_im, boff(inm)); }
                else
                {
                    rr[leg] = g.shared_load(x_re, sp(inm));
                    ri[leg] = g.shared_load(x_im, sp(inm));
                    if (freeze) { g.stmt_materialize(rr[leg]); g.stmt_materialize(ri[leg]); }
                }
            }
        }
        if (freeze) { g.stmt_barrier(); }

        int xr[16]; int xi[16];
        for (int b = 0; b < nbut; ++b)
        {
            int ar[16]; int ai[16];
            for (int m = 0; m < radix; ++m)
            {
                const int leg = b * radix + m;
                if (m == 0) { ar[0] = rr[leg]; ai[0] = ri[leg]; continue; }
                const int twidx = mul(jidxs[b], ku(static_cast<crd::u32>(m * nl)));
                const int wr    = g.buffer_load(tw_re, twidx);
                int       wi    = g.buffer_load(tw_im, twidx);
                if (inv) { wi = neg(wi); }
                cmul(rr[leg], ri[leg], wr, wi, ar[m], ai[m]);
            }
            if (r16) { dft16(inv, ar, ai, &xr[0], &xi[0]); }
            else { dft4(inv, ar, ai, &xr[b * 4], &xi[b * 4]); }
        }

        for (int b = 0; b < nbut; ++b)
        {
            for (int k = 0; k < radix; ++k)
            {
                const int leg  = b * radix + k;
                const int outk = add(add(mul(gidxs[b], ku(static_cast<crd::u32>(ll))), jidxs[b]), ku(static_cast<crd::u32>(k * r)));
                int       vr = xr[leg]; int vi = xi[leg];
                if (fuse_f) { cmul(vr, vi, g.buffer_load(filt_re, foff(outk)), g.buffer_load(filt_im, foff(outk)), vr, vi); }
                if (wr_glob)
                {
                    g.stmt_buffer_store(out_re, boff(outk), mul(vr, invn));
                    g.stmt_buffer_store(out_im, boff(outk), mul(vi, invn));
                }
                else { g.stmt_shared_store(x_re, sp(outk), vr); g.stmt_shared_store(x_im, sp(outk), vi); }
            }
        }
        if (!wr_glob) { g.stmt_barrier(); }
        r = ll;
    }

    Fft1dPlan plan;
    plan.entry.stage             = KStage::Compute;
    plan.entry.local_size[0]     = static_cast<crd::u32>(tile_c * tthreads);
    plan.entry.kernel_body_begin = mark;
    plan.entry.kernel_body_count = g.stmt_count() - mark;
    plan.n                       = n;
    plan.log2n                   = 2 * p4;
    plan.inverse                 = false;
    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// B-cmp Phase 2 — the 2-D image FFT. A separable 2-D FFT is row-FFTs then column-FFTs. On a GPU there is NO cross-workgroup
// barrier WITHIN a dispatch, so a 2-D FFT CANNOT be one kernel — it is an ordered SEQUENCE of dispatches (row FFT → transpose
// → column FFT → transpose-back) that the CPU oracle and every GPU drive identically. Transposing between the passes makes
// each original column a CONTIGUOUS row, so the column pass reuses the same coalesced batched 1-D FFT (the cuFFT/VkFFT
// design). `Fft2dPlan` is the reusable description a driver executes; `build_fft2d_c2c` authors it.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

// A tiled shared-memory TRANSPOSE of a `rows`×`cols` row-major matrix into its `cols`×`rows` transpose (out[c*rows+r] =
// in[r*cols+c]). ONE workgroup per `tile`×`tile` block; the 1-D WorkgroupIndex maps to a 2-D tile coordinate via Div/Mod.
// **local_size = tile*tile — ONE THREAD PER TILE ELEMENT** (the standard high-occupancy GPU transpose): both the load and
// the transposed store are fully COALESCED (consecutive threads touch consecutive columns), with NO serial per-row loop —
// the earlier `tile`-thread + inner-loop form ran the transpose at ~25% of peak bandwidth and was 69% of the 2-D conv time.
// The shared tile has a +1 padded row stride so the transposed (cross-thread) read hits distinct banks. `tile` must divide
// BOTH rows and cols, and tile*tile ≤ the device max workgroup size (tile ≤ 32). Pure data movement ⇒ bit-exact everywhere.
// Buffers: in=(0,0) ro, out=(0,1) rw. The caller sets grid = (rows/tile)*(cols/tile).
[[nodiscard]] inline KEntry build_transpose2d(KGraph& g, int rows, int cols, int tile)
{
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int stride  = tile + 1;    // padded shared row stride (bank-conflict avoidance on the transposed read)
    const int tiles_c = cols / tile; // tile columns across one matrix row
    const int inbuf   = g.buffer_decl(DType::F32, 0, 0, false);
    const int outbuf  = g.buffer_decl(DType::F32, 0, 1, true);
    const int smem    = g.shared_decl(DType::F32, tile * stride);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex); // 0 .. tile*tile-1
    const int wg      = g.builtin(KBuiltin::WorkgroupIndex);
    const int lr      = g.binary(KOp::Div, tid, ku(static_cast<crd::u32>(tile))); // tile-local row of this thread's element
    const int lc      = g.binary(KOp::Mod, tid, ku(static_cast<crd::u32>(tile))); // tile-local column
    // tile origin from the 1-D workgroup index (recomputed per phase — the two phases are straight-line, but composites are
    // emitted at first use, so keeping them local to each store keeps every value in scope).
    const auto rbase = [&]() { return mul(g.binary(KOp::Div, wg, ku(static_cast<crd::u32>(tiles_c))), ku(static_cast<crd::u32>(tile))); }; // tile_r*tile
    const auto cbase = [&]() { return mul(g.binary(KOp::Mod, wg, ku(static_cast<crd::u32>(tiles_c))), ku(static_cast<crd::u32>(tile))); }; // tile_c*tile

    const int mark = g.kernel_stmt_mark();
    // LOAD (coalesced): shared[lr*stride + lc] = in[(rbase+lr)*cols + (cbase+lc)]
    const int gin = add(mul(add(rbase(), lr), ku(static_cast<crd::u32>(cols))), add(cbase(), lc));
    const int si1 = add(mul(lr, ku(static_cast<crd::u32>(stride))), lc);
    g.stmt_shared_store(smem, si1, g.buffer_load(inbuf, gin));
    g.stmt_barrier();
    // STORE (coalesced, transposed): out[(cbase+lr)*rows + (rbase+lc)] = shared[lc*stride + lr]  (cross-thread column read)
    const int gout = add(mul(add(cbase(), lr), ku(static_cast<crd::u32>(rows))), add(rbase(), lc));
    const int si2  = add(mul(lc, ku(static_cast<crd::u32>(stride))), lr);
    g.stmt_buffer_store(outbuf, gout, g.shared_load(smem, si2));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(tile * tile); // one thread per tile element
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Pick the fastest available BATCHED radix-k 1-D FFT for a power-of-two n: REGISTER-BLOCKED radix-16 for 4^k >= 256 (the
// ncu-profiled winner: 3 shared exchanges, 64-thread blocks, values in registers), radix-4 for small 4^k, radix-8 (8^k),
// else radix-2 (any 2^k). The 2-D FFT chooses independently for the row (cols) and column (rows) dimensions.
[[nodiscard]] inline Fft1dPlan build_fft1d_batched(KGraph& g, int n, bool inverse)
{
    const int lg = fft_log2(n);
    if ((lg % 2) == 0 && n >= 1024) { return build_fft1d_radix16(g, n, inverse, true); } // 4^k, register-blocked (n/16 >= 64 threads)
    if ((lg % 2) == 0) { return build_fft1d_radix4(g, n, inverse, true); }               // small 4^k (radix-16's tiny blocks lose)
    if ((lg % 3) == 0) { return build_fft1d_radix8(g, n, inverse, true); }               // 8^k
    return build_fft1d_radix2(g, n, inverse, true);                                      // any 2^k
}

// The role of a logical buffer in a 2-D plan, so a driver knows what to fill (image, twiddles) and where the result lands.
enum class Fft2dBufRole : crd::u8 { InRe, InIm, TwColRe, TwColIm, TwRowRe, TwRowIm, ResRe, ResIm, Scratch };

struct Fft2dBuffer
{
    crd::i32     size = 0;                       // element count (f32/f64 scalars)
    Fft2dBufRole role = Fft2dBufRole::Scratch;
};

// One dispatch: the GRAPH the entry was authored into (one graph per unique entry — a CKIR emitter emits ALL of a graph's
// buffer/shared decls, so two entries must never share a graph or their bindings collide), the entry, its workgroup grid,
// and the ordered logical buffers bound to its buffer_decls (binding k ← bind[k]).
struct Fft2dPass
{
    KGraph*  graph          = nullptr;
    KEntry   entry;
    crd::u32 num_workgroups = 1;
    crd::i32 bind[8]        = {-1, -1, -1, -1, -1, -1, -1, -1};
    crd::i32 nbind          = 0;
};

// A complete 2-D FFT as an ordered dispatch list over a set of logical buffers. A driver allocates one physical buffer per
// logical id (by `.size`), fills InRe/InIm (the image) + TwCol*/TwRow* (the twiddle tables W_cols / W_rows), runs each pass
// in order with a barrier between them, then reads ResRe/ResIm. Multi-dispatch is mandatory: a separable 2-D FFT needs a
// GLOBAL sync between the row and column passes, which a single GPU dispatch cannot provide.
struct Fft2dPlan
{
    crd::i32    rows      = 0;
    crd::i32    cols      = 0;
    bool        inverse   = false;
    Fft2dPass   passes[8];
    crd::i32    npasses   = 0;
    Fft2dBuffer buffers[20];
    crd::i32    nbuffers  = 0;
    crd::i32    in_re = -1, in_im = -1;
    crd::i32    tw_col_re = -1, tw_col_im = -1; // cols-point twiddles (row FFT)
    crd::i32    tw_row_re = -1, tw_row_im = -1; // rows-point twiddles (column FFT)
    crd::i32    filt_re = -1, filt_im = -1;     // convolution only: the PSF spectrum H^T (transposed), size rows*cols
    crd::i32    res_re = -1, res_im = -1;
};

// Author a complex-to-complex 2-D FFT of a `rows`×`cols` row-major image as a 6-dispatch plan: batched ROW FFT (grid=rows)
// → TRANSPOSE re,im (rows×cols→cols×rows) → batched COLUMN FFT (grid=cols) → TRANSPOSE-BACK re,im (cols×rows→rows×cols) ⇒
// the spectrum X[kr][kc] in natural [row,col] layout. `graphs` is FOUR caller-owned KGraphs — one per UNIQUE entry (row FFT,
// R×C transpose, column FFT, C×R transpose) because a CKIR emitter emits every buffer/shared decl in a graph, so two entries
// in one graph collide on their binding-0 blocks. `tile` must divide rows AND cols (default 16); both dims must be powers of
// two. `inverse` conjugates each 1-D pass (the 1/(rows*cols) normalization is the caller's, kept out so fwd+inv share the
// authoring and stay bit-exact). Row/column passes auto-select the fastest batched radix (4/8/2).
[[nodiscard]] inline Fft2dPlan build_fft2d_c2c(KGraph** graphs, int rows, int cols, bool inverse = false, int tile = 16)
{
    Fft2dPlan plan;
    plan.rows    = rows;
    plan.cols    = cols;
    plan.inverse = inverse;

    const int  rc     = rows * cols;
    const auto add_buf = [&](Fft2dBufRole role, int size) -> int {
        const int id          = plan.nbuffers;
        plan.buffers[id].role = role;
        plan.buffers[id].size = size;
        ++plan.nbuffers;
        return id;
    };
    const int b_in_re  = add_buf(Fft2dBufRole::InRe, rc);
    const int b_in_im  = add_buf(Fft2dBufRole::InIm, rc);
    const int b_twc_re = add_buf(Fft2dBufRole::TwColRe, cols);
    const int b_twc_im = add_buf(Fft2dBufRole::TwColIm, cols);
    const int b_twr_re = add_buf(Fft2dBufRole::TwRowRe, rows);
    const int b_twr_im = add_buf(Fft2dBufRole::TwRowIm, rows);
    const int b_row_re = add_buf(Fft2dBufRole::Scratch, rc); // row-FFT output (rows×cols)
    const int b_row_im = add_buf(Fft2dBufRole::Scratch, rc);
    const int b_tr_re  = add_buf(Fft2dBufRole::Scratch, rc); // transposed (cols×rows)
    const int b_tr_im  = add_buf(Fft2dBufRole::Scratch, rc);
    const int b_col_re = add_buf(Fft2dBufRole::Scratch, rc); // column-FFT output (cols×rows)
    const int b_col_im = add_buf(Fft2dBufRole::Scratch, rc);
    const int b_res_re = add_buf(Fft2dBufRole::ResRe, rc);
    const int b_res_im = add_buf(Fft2dBufRole::ResIm, rc);
    plan.in_re     = b_in_re;
    plan.in_im     = b_in_im;
    plan.tw_col_re = b_twc_re;
    plan.tw_col_im = b_twc_im;
    plan.tw_row_re = b_twr_re;
    plan.tw_row_im = b_twr_im;
    plan.res_re    = b_res_re;
    plan.res_im    = b_res_im;

    // pass 0: batched ROW FFT (cols-point) into graphs[0], one workgroup per row. bindings 0..5: in_re,in_im,tw_re,tw_im,out_re,out_im.
    const Fft1dPlan rowfft = build_fft1d_batched(*graphs[0], cols, inverse);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[0];
        p.entry          = rowfft.entry;
        p.num_workgroups = static_cast<crd::u32>(rows);
        p.bind[0] = b_in_re; p.bind[1] = b_in_im; p.bind[2] = b_twc_re; p.bind[3] = b_twc_im; p.bind[4] = b_row_re; p.bind[5] = b_row_im;
        p.nbind = 6;
    }

    // pass 1,2: TRANSPOSE rows×cols → cols×rows (re, then im — same entry graphs[1], different buffers). grid = (rows/tile)*(cols/tile).
    const KEntry   tr_rc   = build_transpose2d(*graphs[1], rows, cols, tile);
    const crd::u32 grid_rc = static_cast<crd::u32>((rows / tile) * (cols / tile));
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[1];
        p.entry          = tr_rc;
        p.num_workgroups = grid_rc;
        p.bind[0] = b_row_re; p.bind[1] = b_tr_re; p.nbind = 2;
    }
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[1];
        p.entry          = tr_rc;
        p.num_workgroups = grid_rc;
        p.bind[0] = b_row_im; p.bind[1] = b_tr_im; p.nbind = 2;
    }

    // pass 3: batched COLUMN FFT (rows-point) into graphs[2] — each original column is now a contiguous row of the cols×rows
    // transpose, one workgroup per column. bindings: in_re,in_im (transposed), tw_re,tw_im (rows-point), out_re,out_im.
    const Fft1dPlan colfft = build_fft1d_batched(*graphs[2], rows, inverse);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[2];
        p.entry          = colfft.entry;
        p.num_workgroups = static_cast<crd::u32>(cols);
        p.bind[0] = b_tr_re; p.bind[1] = b_tr_im; p.bind[2] = b_twr_re; p.bind[3] = b_twr_im; p.bind[4] = b_col_re; p.bind[5] = b_col_im;
        p.nbind = 6;
    }

    // pass 4,5: TRANSPOSE-BACK cols×rows → rows×cols (re, then im — graphs[3]) ⇒ natural [row,col] layout. grid = (cols/tile)*(rows/tile).
    // (cols,rows) is deliberate — the transpose-back's INPUT matrix is cols×rows, so the swap vs pass 1's (rows,cols) is correct.
    const KEntry tr_cr = build_transpose2d(*graphs[3], cols, rows, tile); // NOLINT(readability-suspicious-call-argument)
    const crd::u32 grid_cr = static_cast<crd::u32>((cols / tile) * (rows / tile));
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[3];
        p.entry          = tr_cr;
        p.num_workgroups = grid_cr;
        p.bind[0] = b_col_re; p.bind[1] = b_res_re; p.nbind = 2;
    }
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[3];
        p.entry          = tr_cr;
        p.num_workgroups = grid_cr;
        p.bind[0] = b_col_im; p.bind[1] = b_res_im; p.nbind = 2;
    }

    return plan;
}

// A BATCHED complex-to-complex 2-D FFT of `batch` contiguous rows×cols row-major images, authored as a TWO-dispatch plan in
// the transpose-on-write STRIDED form (the fewest-round-trips design): batched ROW FFT (contiguous, grid=rows·batch) → batched
// STRIDED+TILED COLUMN FFT (col_stride=cols, grid=(cols/tile_c)·batch — one block transforms tile_c adjacent columns of ONE
// image, coalesced). No transpose scratch and no transpose kernel: the column pass reads and writes its column IN PLACE in the
// row-major image. This is exactly the DRAM-bound batched regime where paying ONE global round-trip per pass (vs the vendor's
// per-image passes) is the win. `graphs` = TWO caller-owned KGraphs (row FFT, strided column FFT). `rows` and `cols` are
// powers of two and `rows` is a power of FOUR (the radix-16/4 tiled column). `inverse` conjugates both passes. NO 1/(rows·cols)
// normalization is applied — the caller folds any scale into its data (the FFT-ocean uses Tessendorf's UNNORMALISED inverse).
// `tile_c` (default 8) = columns per column-FFT block. Result lands back over `in` (`res_re/res_im` alias `in_re/in_im`).
[[nodiscard]] inline Fft2dPlan build_fft2d_c2c_batched(KGraph** graphs, int rows, int cols, int batch, bool inverse,
                                                       int tile_c = 8)
{
    Fft2dPlan plan;
    plan.rows    = rows;
    plan.cols    = cols;
    plan.inverse = inverse;

    const int  rc      = rows * cols;
    const int  rcb     = rc * batch;
    const auto add_buf = [&](Fft2dBufRole role, int size) -> int {
        const int id          = plan.nbuffers;
        plan.buffers[id].role = role;
        plan.buffers[id].size = size;
        ++plan.nbuffers;
        return id;
    };
    const int b_in_re  = add_buf(Fft2dBufRole::InRe, rcb);
    const int b_in_im  = add_buf(Fft2dBufRole::InIm, rcb);
    const int b_twc_re = add_buf(Fft2dBufRole::TwColRe, cols);
    const int b_twc_im = add_buf(Fft2dBufRole::TwColIm, cols);
    const int b_twr_re = add_buf(Fft2dBufRole::TwRowRe, rows);
    const int b_twr_im = add_buf(Fft2dBufRole::TwRowIm, rows);
    const int b_x_re   = add_buf(Fft2dBufRole::Scratch, rcb); // row-FFT output (row-major, batch-contiguous)
    const int b_x_im   = add_buf(Fft2dBufRole::Scratch, rcb);
    plan.in_re     = b_in_re;
    plan.in_im     = b_in_im;
    plan.tw_col_re = b_twc_re;
    plan.tw_col_im = b_twc_im;
    plan.tw_row_re = b_twr_re;
    plan.tw_row_im = b_twr_im;
    plan.res_re    = b_in_re; // the strided column pass writes the final result back over `in` (free after pass 0)
    plan.res_im    = b_in_im;

    // pass 0: batched ROW FFT (cols-point, contiguous), grid=rows·batch → x (row-major).
    const Fft1dPlan rowfft = build_fft1d_batched(*graphs[0], cols, inverse);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[0];
        p.entry          = rowfft.entry;
        p.num_workgroups = static_cast<crd::u32>(rows * batch);
        p.bind[0] = b_in_re; p.bind[1] = b_in_im; p.bind[2] = b_twc_re; p.bind[3] = b_twc_im; p.bind[4] = b_x_re; p.bind[5] = b_x_im;
        p.nbind = 6;
    }

    // pass 1: batched STRIDED+TILED COLUMN FFT (rows-point, col_stride=cols), grid=(cols/tile_c)·batch → res (over `in`).
    const Fft1dPlan colfft = build_fft1d_radix16(*graphs[1], rows, inverse, /*batched=*/true, tile_c, /*col_stride=*/cols);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[1];
        p.entry          = colfft.entry;
        p.num_workgroups = static_cast<crd::u32>((cols / tile_c) * batch);
        p.bind[0] = b_x_re; p.bind[1] = b_x_im; p.bind[2] = b_twr_re; p.bind[3] = b_twr_im; p.bind[4] = b_in_re; p.bind[5] = b_in_im;
        p.nbind = 6;
    }

    return plan;
}

// ⭐⭐ THE CRUSH (2-D): a FUSED FFT-convolution of a rows×cols image with a PSF — y = IFFT2(FFT2(x) ⊙ H) — as a 7-dispatch
// plan. The column FFT + ×H + inverse-column FFT all live in the SAME transposed layout (one workgroup per column), so they
// COLLAPSE into ONE on-chip dispatch (the batched 1-D fused conv). The vendor (cuFFT) pays a full forward 2-D FFT + a full-
// image multiply + a full inverse 2-D FFT — THREE global round-trips over the column dimension; we pay ONE. Pipeline: row FFT
// (grid=rows) → transpose re,im → FUSED column conv (FFT·×H·IFFT·1/(R·C), on-chip, grid=cols) → transpose-back re,im →
// inverse row FFT (grid=rows). `H` is the PSF's 2-D spectrum in TRANSPOSED layout (filt[kc*rows+ku] = FFT2(h)[ku][kc]),
// precomputed + uploaded ONCE (the bloom pattern: one PSF, many frames — its cost is amortized, not in the per-image path).
// `graphs` is FIVE caller-owned KGraphs (row FFT, R×C transpose, fused column conv, C×R transpose, inverse row FFT). `tile`
// divides rows AND cols; both are powers of FOUR (the fused conv is radix-4). The full 1/(R·C) rides the fused conv (exact in
// f32 for power-of-two R·C) and the inverse row FFT stays RAW ⇒ the whole pipeline is bit-exact via `precise`.
[[nodiscard]] inline Fft2dPlan build_fft2d_convolution(KGraph** graphs, int rows, int cols, int tile = 16)
{
    Fft2dPlan plan;
    plan.rows    = rows;
    plan.cols    = cols;
    plan.inverse = false;

    const int  rc      = rows * cols;
    const auto add_buf = [&](Fft2dBufRole role, int size) -> int {
        const int id          = plan.nbuffers;
        plan.buffers[id].role = role;
        plan.buffers[id].size = size;
        ++plan.nbuffers;
        return id;
    };
    const int b_in_re  = add_buf(Fft2dBufRole::InRe, rc);
    const int b_in_im  = add_buf(Fft2dBufRole::InIm, rc);
    const int b_twc_re = add_buf(Fft2dBufRole::TwColRe, cols);
    const int b_twc_im = add_buf(Fft2dBufRole::TwColIm, cols);
    const int b_twr_re = add_buf(Fft2dBufRole::TwRowRe, rows);
    const int b_twr_im = add_buf(Fft2dBufRole::TwRowIm, rows);
    const int b_flt_re = add_buf(Fft2dBufRole::Scratch, rc); // H^T (PSF spectrum, transposed layout)
    const int b_flt_im = add_buf(Fft2dBufRole::Scratch, rc);
    // ⭐ Just TWO ping-pong image buffers (A, B), REUSED across all 7 passes — the L2-residency crush lever: the working set
    // (in + filt + A + B = 8 image planes) stays UNDER the 48 MB L2, vs the 18 distinct planes (~56 MB) that spilled to DRAM
    // and ran the pipeline at DRAM peak (~745 GB/s) instead of L2 (~1.9 TB/s, cuFFT's regime). res reuses `in` (free after
    // pass 0). Dataflow (no read-after-overwrite hazard — verified): p0 in→A · p1,2 A→B · p3 B→A · p4,5 A→B · p6 B→in.
    const int b_a_re = add_buf(Fft2dBufRole::Scratch, rc);
    const int b_a_im = add_buf(Fft2dBufRole::Scratch, rc);
    const int b_b_re = add_buf(Fft2dBufRole::Scratch, rc);
    const int b_b_im = add_buf(Fft2dBufRole::Scratch, rc);
    const int b_x1_re  = b_a_re; const int b_x1_im  = b_a_im; // pass 0 writes A
    const int b_x1t_re = b_b_re; const int b_x1t_im = b_b_im; // pass 1,2 write B
    const int b_y1t_re = b_a_re; const int b_y1t_im = b_a_im; // pass 3 writes A (reuse)
    const int b_y1_re  = b_b_re; const int b_y1_im  = b_b_im; // pass 4,5 write B (reuse)
    const int b_res_re = b_in_re; const int b_res_im = b_in_im; // pass 6 writes in (reuse)
    plan.in_re     = b_in_re;
    plan.in_im     = b_in_im;
    plan.tw_col_re = b_twc_re;
    plan.tw_col_im = b_twc_im;
    plan.tw_row_re = b_twr_re;
    plan.tw_row_im = b_twr_im;
    plan.filt_re   = b_flt_re;
    plan.filt_im   = b_flt_im;
    plan.res_re    = b_res_re;
    plan.res_im    = b_res_im;

    // pass 0: forward ROW FFT (cols-point) into graphs[0], grid=rows. bindings: in_re,in_im,tw_re,tw_im,out_re,out_im.
    const Fft1dPlan rowfft = build_fft1d_batched(*graphs[0], cols, false);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[0];
        p.entry          = rowfft.entry;
        p.num_workgroups = static_cast<crd::u32>(rows);
        p.bind[0] = b_in_re; p.bind[1] = b_in_im; p.bind[2] = b_twc_re; p.bind[3] = b_twc_im; p.bind[4] = b_x1_re; p.bind[5] = b_x1_im;
        p.nbind = 6;
    }

    // pass 1,2: TRANSPOSE rows×cols → cols×rows (re, im), graphs[1]. grid = (rows/tile)*(cols/tile).
    const KEntry   tr_rc   = build_transpose2d(*graphs[1], rows, cols, tile);
    const crd::u32 grid_rc = static_cast<crd::u32>((rows / tile) * (cols / tile));
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[1];
        p.entry          = tr_rc;
        p.num_workgroups = grid_rc;
        p.bind[0] = b_x1_re; p.bind[1] = b_x1t_re; p.nbind = 2;
    }
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[1];
        p.entry          = tr_rc;
        p.num_workgroups = grid_rc;
        p.bind[0] = b_x1_im; p.bind[1] = b_x1t_im; p.nbind = 2;
    }

    // pass 3: the FUSED COLUMN CONV (rows-point, grid=cols) into graphs[2] — FFT·×H·IFFT·1/(R·C) on-chip, per column. The
    // filter is per-workgroup (each column its own H slice). bindings: in_re,in_im, tw_re,tw_im, filt_re,filt_im, out_re,out_im.
    // Register-blocked radix-16 for 4^k rows >= 1024 (the ncu crush lever; below that its tiny blocks lose), radix-4 otherwise.
    const bool      r16  = (fft_log2(rows) % 2) == 0 && rows >= 1024;
    const Fft1dPlan conv = r16 ? build_fft1d_convolution16(*graphs[2], rows, true, 1.0 / static_cast<crd::f64>(rc), true)
                               : build_fft1d_convolution(*graphs[2], rows, true, 1.0 / static_cast<crd::f64>(rc), true);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[2];
        p.entry          = conv.entry;
        p.num_workgroups = static_cast<crd::u32>(cols);
        p.bind[0] = b_x1t_re; p.bind[1] = b_x1t_im; p.bind[2] = b_twr_re; p.bind[3] = b_twr_im;
        p.bind[4] = b_flt_re; p.bind[5] = b_flt_im; p.bind[6] = b_y1t_re; p.bind[7] = b_y1t_im;
        p.nbind = 8;
    }

    // pass 4,5: TRANSPOSE-BACK cols×rows → rows×cols (re, im), graphs[3]. grid = (cols/tile)*(rows/tile).
    const KEntry   tr_cr   = build_transpose2d(*graphs[3], cols, rows, tile); // NOLINT(readability-suspicious-call-argument)
    const crd::u32 grid_cr = static_cast<crd::u32>((cols / tile) * (rows / tile));
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[3];
        p.entry          = tr_cr;
        p.num_workgroups = grid_cr;
        p.bind[0] = b_y1t_re; p.bind[1] = b_y1_re; p.nbind = 2;
    }
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[3];
        p.entry          = tr_cr;
        p.num_workgroups = grid_cr;
        p.bind[0] = b_y1t_im; p.bind[1] = b_y1_im; p.nbind = 2;
    }

    // pass 6: RAW inverse ROW FFT (cols-point, grid=rows) into graphs[4] — no scale (the 1/(R·C) already rode the fused conv).
    const Fft1dPlan irowfft = build_fft1d_batched(*graphs[4], cols, true);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[4];
        p.entry          = irowfft.entry;
        p.num_workgroups = static_cast<crd::u32>(rows);
        p.bind[0] = b_y1_re; p.bind[1] = b_y1_im; p.bind[2] = b_twc_re; p.bind[3] = b_twc_im; p.bind[4] = b_res_re; p.bind[5] = b_res_im;
        p.nbind = 6;
    }

    return plan;
}

// ⭐⭐ THE TRANSPOSE-ON-WRITE 2-D FUSED CONVOLUTION — the 4 transpose passes FUSED INTO the column conv's strided image
// access ⇒ just THREE dispatches (row FFT → strided column conv → inverse row FFT) instead of seven. The column pass reads
// and writes its column IN PLACE in the row-major image (`col_stride = cols`), so no cols×rows scratch and no transpose
// kernel. Authored entirely in CKIR (the strided offset is plain index arithmetic — every backend lowers it identically);
// this does NOT betray the IR with an emitter special-case. `graphs` = THREE (row FFT, strided conv, inverse row FFT). Both
// dims powers of two, rows a power of 4 (radix-16/4). H is the PSF spectrum ROW-MAJOR (filt[u*cols+c] = FFT2(h)[u][c]).
// ⚠ TRADE-OFF: the column access is UNCOALESCED (a warp strides by `cols`); measure vs `build_fft2d_convolution` (which pays
// 4 extra COALESCED transpose passes) — fewer passes vs worse locality. The bench (`docs/bench/...`) records which wins.
// `tile_c` > 1 ⇒ the column conv is the TILED (coalesced) `build_fft1d_convolution16_tiled` (grid = cols/tile_c), the
// transpose-on-write done RIGHT. `tile_c` = 1 keeps the plain strided conv (the coalescing-loss baseline).
// `batch` > 1 ⇒ B contiguous images share ONE PSF spectrum: the three grids scale by B and the tiled conv splits its
// WorkgroupIndex into (image, col-tile). This is the DRAM-BOUND regime where our fewer-round-trips FUSION crushes cuFFT —
// at single-image 1024² the 8 MB image is L2-resident and cuFFT's arithmetic (FMA, which bit-exactness forbids us) wins.
[[nodiscard]] inline Fft2dPlan build_fft2d_convolution_strided(KGraph** graphs, int rows, int cols, int tile_c = 1, int batch = 1)
{
    Fft2dPlan plan;
    plan.rows    = rows;
    plan.cols    = cols;
    plan.inverse = false;

    const int  rc      = rows * cols;
    const int  rcb     = rc * batch; // B contiguous images share one filter spectrum; each image is rc elements
    const auto add_buf = [&](Fft2dBufRole role, int size) -> int {
        const int id          = plan.nbuffers;
        plan.buffers[id].role = role;
        plan.buffers[id].size = size;
        ++plan.nbuffers;
        return id;
    };
    const int b_in_re  = add_buf(Fft2dBufRole::InRe, rcb);
    const int b_in_im  = add_buf(Fft2dBufRole::InIm, rcb);
    const int b_twc_re = add_buf(Fft2dBufRole::TwColRe, cols);
    const int b_twc_im = add_buf(Fft2dBufRole::TwColIm, cols);
    const int b_twr_re = add_buf(Fft2dBufRole::TwRowRe, rows);
    const int b_twr_im = add_buf(Fft2dBufRole::TwRowIm, rows);
    const int b_flt_re = add_buf(Fft2dBufRole::Scratch, rc); // H row-major (H[u*cols+c]) — ONE spectrum shared by all B images
    const int b_flt_im = add_buf(Fft2dBufRole::Scratch, rc);
    const int b_x1_re  = add_buf(Fft2dBufRole::Scratch, rcb); // row-FFT output (row-major)
    const int b_x1_im  = add_buf(Fft2dBufRole::Scratch, rcb);
    const int b_y1_re  = add_buf(Fft2dBufRole::Scratch, rcb); // conv output (row-major)
    const int b_y1_im  = add_buf(Fft2dBufRole::Scratch, rcb);
    plan.in_re     = b_in_re;
    plan.in_im     = b_in_im;
    plan.tw_col_re = b_twc_re;
    plan.tw_col_im = b_twc_im;
    plan.tw_row_re = b_twr_re;
    plan.tw_row_im = b_twr_im;
    plan.filt_re   = b_flt_re;
    plan.filt_im   = b_flt_im;
    plan.res_re    = b_in_re; // pass 2 writes the result back over `in` (free after pass 0)
    plan.res_im    = b_in_im;

    // pass 0: forward ROW FFT (cols-point, contiguous), grid=rows → x1 (row-major).
    const Fft1dPlan rowfft = build_fft1d_batched(*graphs[0], cols, false);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[0];
        p.entry          = rowfft.entry;
        p.num_workgroups = static_cast<crd::u32>(rows * batch); // B contiguous images ⇒ WorkgroupIndex·cols spans all rows
        p.bind[0] = b_in_re; p.bind[1] = b_in_im; p.bind[2] = b_twc_re; p.bind[3] = b_twc_im; p.bind[4] = b_x1_re; p.bind[5] = b_x1_im;
        p.nbind = 6;
    }

    // pass 1: fused COLUMN conv over column c of x1/y1 (col_stride=cols). tile_c>1 = TILED coalesced (grid=cols/tile_c); batched
    // (B>1) always uses the tiled builder (batched=true splits WorkgroupIndex into image+col-tile); else the plain strided baseline.
    const bool      use_tiled = tile_c > 1 || batch > 1;
    const Fft1dPlan conv      = use_tiled ? build_fft1d_convolution16_tiled(*graphs[1], rows, tile_c, 1.0 / static_cast<crd::f64>(rc), cols, batch > 1)
                                          : build_fft1d_convolution16(*graphs[1], rows, true, 1.0 / static_cast<crd::f64>(rc), true, cols);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[1];
        p.entry          = conv.entry;
        p.num_workgroups = static_cast<crd::u32>((cols / tile_c) * batch);
        p.bind[0] = b_x1_re; p.bind[1] = b_x1_im; p.bind[2] = b_twr_re; p.bind[3] = b_twr_im;
        p.bind[4] = b_flt_re; p.bind[5] = b_flt_im; p.bind[6] = b_y1_re; p.bind[7] = b_y1_im;
        p.nbind = 8;
    }

    // pass 2: inverse ROW FFT (cols-point, contiguous, raw), grid=rows·B → res (over `in`).
    const Fft1dPlan irowfft = build_fft1d_batched(*graphs[2], cols, true);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[2];
        p.entry          = irowfft.entry;
        p.num_workgroups = static_cast<crd::u32>(rows * batch);
        p.bind[0] = b_y1_re; p.bind[1] = b_y1_im; p.bind[2] = b_twc_re; p.bind[3] = b_twc_im; p.bind[4] = b_in_re; p.bind[5] = b_in_im;
        p.nbind = 6;
    }

    return plan;
}

// ⭐⭐⭐ THE REAL 2-D FFT-CONVOLUTION — y = IFFT2(FFT2(x) ⊙ H) for a REAL image x and REAL PSF h (bloom is real-valued). The
// real spectrum is HERMITIAN in the row-FFT (column) direction, so only the half-width Wp = pad(cols/2+1, tile_c) columns are
// unique — HALVING the column-conv work AND the x1/y1/filter traffic vs the full-complex `build_fft2d_convolution_strided`.
// THREE dispatches: R2C row FFT (real→half) → half-width TILED column conv (FFT·×H·IFFT·1/(rows·cols)) → C2R row FFT
// (half→real). Everything CKIR-pure (the half-store/Hermitian-expand is Min/Select/If index arithmetic; all backends identical).
// `graphs` = THREE (R2C, conv, C2R). Both dims powers of two, rows a power of 4. `batch` B images share ONE half PSF (the
// DRAM-bound crush regime). H is the half PSF spectrum ROW-MAJOR (filt[u*Wp + c], c ∈ [0, cols/2]).
[[nodiscard]] inline Fft2dPlan build_fft2d_convolution_r2c(KGraph** graphs, int rows, int cols, int tile_c = 4, int batch = 1)
{
    Fft2dPlan plan;
    plan.rows    = rows;
    plan.cols    = cols;
    plan.inverse = false;

    const int  rc      = rows * cols;
    const int  half1   = cols / 2 + 1;
    const int  hw      = ((half1 + tile_c - 1) / tile_c) * tile_c; // half-spectrum row width, padded to a tile_c multiple
    const int  rcb     = rc * batch;                               // real image (B contiguous)
    const int  hb      = rows * hw * batch;                        // half-spectrum image (B contiguous)
    const auto add_buf = [&](Fft2dBufRole role, int size) -> int {
        const int id          = plan.nbuffers;
        plan.buffers[id].role = role;
        plan.buffers[id].size = size;
        ++plan.nbuffers;
        return id;
    };
    const int b_in_re  = add_buf(Fft2dBufRole::InRe, rcb);      // REAL image (no imag)
    const int b_twr_re = add_buf(Fft2dBufRole::TwColRe, cols);  // cols-point twiddles (R2C/C2R row FFTs)
    const int b_twr_im = add_buf(Fft2dBufRole::TwColIm, cols);
    const int b_twc_re = add_buf(Fft2dBufRole::TwRowRe, rows);  // rows-point twiddles (column conv)
    const int b_twc_im = add_buf(Fft2dBufRole::TwRowIm, rows);
    const int b_flt_re = add_buf(Fft2dBufRole::Scratch, rows * hw); // HALF PSF H (row-major, width hw), shared by all B
    const int b_flt_im = add_buf(Fft2dBufRole::Scratch, rows * hw);
    const int b_x1_re  = add_buf(Fft2dBufRole::Scratch, hb); // R2C output = half spectrum
    const int b_x1_im  = add_buf(Fft2dBufRole::Scratch, hb);
    const int b_y1_re  = add_buf(Fft2dBufRole::Scratch, hb); // conv output = half spectrum
    const int b_y1_im  = add_buf(Fft2dBufRole::Scratch, hb);
    plan.in_re     = b_in_re;
    plan.tw_col_re = b_twr_re;
    plan.tw_col_im = b_twr_im;
    plan.tw_row_re = b_twc_re;
    plan.tw_row_im = b_twc_im;
    plan.filt_re   = b_flt_re;
    plan.filt_im   = b_flt_im;
    plan.res_re    = b_in_re; // C2R writes the real result back over the real input (free after pass 0)

    // pass 0: R2C row FFT (real row length cols → half spectrum), grid = rows·B → x1 (rows×hw).
    const Fft1dPlan r2c = build_fft1d_r2c(*graphs[0], cols, hw);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[0];
        p.entry          = r2c.entry;
        p.num_workgroups = static_cast<crd::u32>(rows * batch);
        p.bind[0] = b_in_re; p.bind[1] = b_twr_re; p.bind[2] = b_twr_im; p.bind[3] = b_x1_re; p.bind[4] = b_x1_im;
        p.nbind = 5;
    }

    // pass 1: fused HALF-WIDTH column conv over hw columns (col_stride = hw), grid = (hw/tile_c)·B.
    const Fft1dPlan conv = build_fft1d_convolution16_tiled(*graphs[1], rows, tile_c, 1.0 / static_cast<crd::f64>(rc), hw, batch > 1);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[1];
        p.entry          = conv.entry;
        p.num_workgroups = static_cast<crd::u32>((hw / tile_c) * batch);
        p.bind[0] = b_x1_re; p.bind[1] = b_x1_im; p.bind[2] = b_twc_re; p.bind[3] = b_twc_im;
        p.bind[4] = b_flt_re; p.bind[5] = b_flt_im; p.bind[6] = b_y1_re; p.bind[7] = b_y1_im;
        p.nbind = 8;
    }

    // pass 2: C2R inverse row FFT (half spectrum → real row length cols), grid = rows·B → res (over `in`).
    const Fft1dPlan c2r = build_fft1d_c2r(*graphs[2], cols, hw);
    {
        Fft2dPass& p     = plan.passes[plan.npasses++];
        p.graph          = graphs[2];
        p.entry          = c2r.entry;
        p.num_workgroups = static_cast<crd::u32>(rows * batch);
        p.bind[0] = b_y1_re; p.bind[1] = b_y1_im; p.bind[2] = b_twr_re; p.bind[3] = b_twr_im; p.bind[4] = b_in_re;
        p.nbind = 5;
    }

    return plan;
}

} // namespace crd::kir
