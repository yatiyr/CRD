// test_ckir_kernel.cpp — B-cmp Phase 0: the imperative compute-kernel / shared-memory IR. CPU-oracle tests that a
// hand-authored workgroup kernel (shared arrays + barriers + storage buffers) evaluates correctly, with GPU barrier
// semantics (a cross-thread shared read is only correct BECAUSE of the barrier before it).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

TEST_CASE("B-cmp: shared-memory REVERSE kernel -- CPU oracle (barrier-gated cross-thread read)", "[kir][kernel]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              ls = 8; // one workgroup of 8 threads
    const kir::Shape           sh1 = kir::make_shape({1});

    const int inbuf  = g.buffer_decl(kir::DType::F64, 0, 0, false); // readonly input  at (set 0, binding 0)
    const int outbuf = g.buffer_decl(kir::DType::F64, 0, 1, true);  // writable output at (set 0, binding 1)
    const int smem   = g.shared_decl(kir::DType::F64, ls);
    const int lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex); // scalar uint thread id

    // shared[lid] = in[lid];  barrier;  out[lid] = shared[ls-1-lid];
    const int mark = g.kernel_stmt_mark();
    g.stmt_shared_store(smem, lid, g.buffer_load(inbuf, lid));
    g.stmt_barrier();
    const int revidx = g.binary(kir::KOp::Sub, g.constant(static_cast<crd::f64>(ls - 1), sh1, kir::DType::U32), lid);
    g.stmt_buffer_store(outbuf, lid, g.shared_load(smem, revidx));

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = ls;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    crd::f64 in[ls];
    crd::f64 out[ls];
    for (int i = 0; i < ls; ++i) { in[i] = 1.0 + 3.0 * i; out[i] = -1.0; }
    kir::KernelBuffer bufs[2] = {{in, ls, 0, 0}, {out, ls, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, static_cast<crd::u32>(ls), &alloc);

    int bad = 0;
    for (int i = 0; i < ls; ++i) { if (out[i] != in[ls - 1 - i]) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: shared-memory workgroup REDUCTION kernel -- CPU oracle (tree sum, select-guarded)", "[kir][kernel]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              ls = 8;
    const kir::Shape           sh1 = kir::make_shape({1});
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, kir::DType::U32); };

    const int inbuf  = g.buffer_decl(kir::DType::F64, 0, 0, false);
    const int outbuf = g.buffer_decl(kir::DType::F64, 0, 1, true);
    const int smem   = g.shared_decl(kir::DType::F64, ls);
    const int lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex);

    const int mark = g.kernel_stmt_mark();
    g.stmt_shared_store(smem, lid, g.buffer_load(inbuf, lid)); // load
    g.stmt_barrier();
    for (int stride = ls / 2; stride >= 1; stride >>= 1) // unrolled tree reduction
    {
        // active = lid < stride;  partner idx guarded into range;  shared[lid] += active ? shared[idx] : 0
        const int active  = g.binary(kir::KOp::CmpLt, lid, ku(static_cast<crd::u32>(stride)));
        const int rawidx  = g.binary(kir::KOp::Add, lid, ku(static_cast<crd::u32>(stride)));
        const int idx     = g.binary(kir::KOp::Min, rawidx, ku(static_cast<crd::u32>(ls - 1)));
        const int partner = g.select(active, g.shared_load(smem, idx), g.constant(0.0, sh1, kir::DType::F64));
        const int sum     = g.binary(kir::KOp::Add, g.shared_load(smem, lid), partner);
        g.stmt_shared_store(smem, lid, sum);
        g.stmt_barrier();
    }
    g.stmt_buffer_store(outbuf, lid, g.shared_load(smem, ku(0U))); // every thread writes the total to its slot

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = ls;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    crd::f64 in[ls];
    crd::f64 out[ls];
    crd::f64 ref = 0.0;
    for (int i = 0; i < ls; ++i) { in[i] = 1.0 + 3.0 * i; out[i] = -1.0; ref += in[i]; }
    kir::KernelBuffer bufs[2] = {{in, ls, 0, 0}, {out, ls, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, static_cast<crd::u32>(ls), &alloc);

    CHECK(out[0] == ref);
    CHECK(out[ls - 1] == ref);
}

// The FIRST test of the STRUCTURED control flow (For + If + a barrier INSIDE the loop): a Hillis-Steele inclusive prefix
// scan. Each stage adds the element `offset` slots back (guarded by `lid >= offset`, an `If`), with a barrier per stage so
// every read sees the previous stage's committed values. Integer-valued ⇒ bit-exact. This is the machinery the FFT needs.
TEST_CASE("B-cmp: workgroup PREFIX-SCAN kernel -- For + If + inner barrier (Hillis-Steele)", "[kir][kernel][ctrlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              ls  = 8; // log2(8) = 3 stages
    const kir::Shape           sh1 = kir::make_shape({1});
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, kir::DType::U32); };

    const int inbuf  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int outbuf = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int smem   = g.shared_decl(kir::DType::F32, ls);
    const int lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex);

    const int mark = g.kernel_stmt_mark();
    g.stmt_shared_store(smem, lid, g.buffer_load(inbuf, lid));
    g.stmt_barrier();
    const int fid    = g.stmt_for_begin(ku(3)); // for stage d in 0..3
    const int d      = g.kernel_loop_var(fid);
    const int offset = g.binary(kir::KOp::Shl, ku(1), d); // 1 << d  (= 1,2,4)
    const int cond   = g.binary(kir::KOp::CmpGe, lid, offset);
    const int cid    = g.stmt_if_begin(cond); // if (lid >= offset)
    const int sum    = g.binary(kir::KOp::Add, g.shared_load(smem, lid), g.shared_load(smem, g.binary(kir::KOp::Sub, lid, offset)));
    g.stmt_shared_store(smem, lid, sum); // reads the PRE-stage committed values (barrier below)
    g.stmt_if_end(cid);
    g.stmt_barrier();
    g.stmt_for_end(fid);
    g.stmt_buffer_store(outbuf, lid, g.shared_load(smem, lid));

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = ls;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    crd::f64 in[ls];
    crd::f64 out[ls];
    crd::f64 ref[ls];
    crd::f64 acc = 0.0;
    for (int i = 0; i < ls; ++i) { in[i] = static_cast<crd::f64>(i + 1); acc += in[i]; ref[i] = acc; out[i] = -1.0; } // 1,3,6,10,...
    kir::KernelBuffer bufs[2] = {{in, ls, 0, 0}, {out, ls, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, static_cast<crd::u32>(ls), &alloc);

    int bad = 0;
    for (int i = 0; i < ls; ++i) { if (out[i] != ref[i]) { ++bad; } }
    CHECK(bad == 0);
    CHECK(out[ls - 1] == 36.0); // 1+2+...+8
}

// The FFT's other on-chip primitive: a SHARED-MEMORY TRANSPOSE of a T×T tile. Thread `r` owns row r: it loads row r into
// shared, and after a barrier writes row r of the output by reading COLUMN r of shared — a genuine CROSS-THREAD read that is
// correct ONLY because of the barrier. Two For loops (over the T columns) exercise the loop machinery; pure data movement ⇒
// bit-exact (Div/Mod-free indexing: r*T+c and c*T+r are add/mul only).
TEST_CASE("B-cmp: shared-memory TRANSPOSE kernel -- For loops + barrier + cross-thread read", "[kir][kernel][ctrlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              tt  = 4;
    constexpr int              ls  = tt; // one thread per row
    const kir::Shape           sh1 = kir::make_shape({1});
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, kir::DType::U32); };

    const int inbuf  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int outbuf = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int smem   = g.shared_decl(kir::DType::F32, tt * tt);
    const int lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex); // = row r

    const int mark = g.kernel_stmt_mark();
    // load row r: for c in 0..tt:  shared[r*tt + c] = in[r*tt + c]
    const int f1 = g.stmt_for_begin(ku(tt));
    const int c1 = g.kernel_loop_var(f1);
    const int rowbase = g.binary(kir::KOp::Mul, lid, ku(tt));
    const int ld_idx  = g.binary(kir::KOp::Add, rowbase, c1);
    g.stmt_shared_store(smem, ld_idx, g.buffer_load(inbuf, ld_idx));
    g.stmt_for_end(f1);
    g.stmt_barrier();
    // write row r transposed: for c in 0..tt:  out[r*tt + c] = shared[c*tt + r]
    const int f2 = g.stmt_for_begin(ku(tt));
    const int c2 = g.kernel_loop_var(f2);
    const int out_idx = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, lid, ku(tt)), c2);
    const int sh_idx  = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, c2, ku(tt)), lid); // column r of shared → cross-thread
    g.stmt_buffer_store(outbuf, out_idx, g.shared_load(smem, sh_idx));
    g.stmt_for_end(f2);

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = ls;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    crd::f64 in[tt * tt];
    crd::f64 out[tt * tt];
    for (int i = 0; i < tt * tt; ++i) { in[i] = static_cast<crd::f64>(i); out[i] = -1.0; }
    kir::KernelBuffer bufs[2] = {{in, tt * tt, 0, 0}, {out, tt * tt, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, static_cast<crd::u32>(ls), &alloc);

    int bad = 0;
    for (int r = 0; r < tt; ++r) { for (int c = 0; c < tt; ++c) { if (out[r * tt + c] != in[c * tt + r]) { ++bad; } } }
    CHECK(bad == 0);
}

// The FFT UNIT TEST: a radix-2 DIT butterfly PASS (the final N=8 stage, half=4). Each thread j does one butterfly reading
// complex pair (j, j+4) from SHARED and writing a SEPARATE output buffer — so no in-place read-after-write aliasing (the
// reason a full in-place FFT needs ping-pong). Precomputed twiddles W_8^j from a buffer keep the butterfly to mul/add/sub
// (bit-exact on the oracle). Verified against the closed-form butterfly + exact spot-checks; correctness of the twiddle
// algebra vs a direct evaluation within f32 tolerance. Complex = separate re/im buffers.
TEST_CASE("B-cmp: radix-2 butterfly PASS kernel -- twiddled complex butterfly (the FFT primitive)", "[kir][kernel][fft]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              n    = 8;
    constexpr int              half = n / 2; // 4 butterflies, one per thread
    const kir::Shape           sh1  = kir::make_shape({1});
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, kir::DType::U32); };

    const int in_re  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int in_im  = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int tw_re  = g.buffer_decl(kir::DType::F32, 0, 2, false);
    const int tw_im  = g.buffer_decl(kir::DType::F32, 0, 3, false);
    const int out_re = g.buffer_decl(kir::DType::F32, 0, 4, true);
    const int out_im = g.buffer_decl(kir::DType::F32, 0, 5, true);
    const int shr    = g.shared_decl(kir::DType::F32, n);
    const int shi    = g.shared_decl(kir::DType::F32, n);
    const int j      = g.builtin(kir::KBuiltin::LocalInvocationIndex); // butterfly index 0..half-1

    const int mark = g.kernel_stmt_mark();
    // load: thread j loads elements j and j+half into shared (two complex values)
    const int jh = g.binary(kir::KOp::Add, j, ku(half));
    g.stmt_shared_store(shr, j, g.buffer_load(in_re, j));
    g.stmt_shared_store(shi, j, g.buffer_load(in_im, j));
    g.stmt_shared_store(shr, jh, g.buffer_load(in_re, jh));
    g.stmt_shared_store(shi, jh, g.buffer_load(in_im, jh));
    g.stmt_barrier();
    // butterfly: t = W^j * x[j+half];  out[j] = x[j] + t;  out[j+half] = x[j] - t   (writes a SEPARATE out buffer)
    const int wr  = g.buffer_load(tw_re, j);
    const int wi  = g.buffer_load(tw_im, j);
    const int x1r = g.shared_load(shr, jh);
    const int x1i = g.shared_load(shi, jh);
    const int tr  = g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, wr, x1r), g.binary(kir::KOp::Mul, wi, x1i));
    const int ti  = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, wr, x1i), g.binary(kir::KOp::Mul, wi, x1r));
    const int x0r = g.shared_load(shr, j);
    const int x0i = g.shared_load(shi, j);
    g.stmt_buffer_store(out_re, j, g.binary(kir::KOp::Add, x0r, tr));
    g.stmt_buffer_store(out_im, j, g.binary(kir::KOp::Add, x0i, ti));
    g.stmt_buffer_store(out_re, jh, g.binary(kir::KOp::Sub, x0r, tr));
    g.stmt_buffer_store(out_im, jh, g.binary(kir::KOp::Sub, x0i, ti));

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = half;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    // twiddles W_8^k = (cos(2πk/8), -sin(2πk/8)), k=0..3
    constexpr crd::f64 two_pi  = 6.28318530717958647693;
    const auto         fabs64  = [](crd::f64 x) { return x < 0.0 ? -x : x; };
    crd::f64           twr[half];
    crd::f64           twi[half];
    for (int kk = 0; kk < half; ++kk)
    {
        const crd::f64 ang = two_pi * static_cast<crd::f64>(kk) / static_cast<crd::f64>(n);
        twr[kk] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(ang)));
        twi[kk] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(ang)));
    }

    SECTION("general input — matches the closed-form butterfly within f32 tolerance")
    {
        crd::f64 ir[n];
        crd::f64 ii[n];
        crd::f64 orr[n];
        crd::f64 oi[n];
        for (int i = 0; i < n; ++i) { ir[i] = static_cast<crd::f64>(i + 1); ii[i] = static_cast<crd::f64>((i % 3) - 1); orr[i] = -9.0; oi[i] = -9.0; }
        kir::KernelBuffer bufs[6] = {{ir, n, 0, 0}, {ii, n, 0, 1}, {twr, half, 0, 2}, {twi, half, 0, 3}, {orr, n, 0, 4}, {oi, n, 0, 5}};
        kir::eval_cpu_kernel(g, e, bufs, 6, static_cast<crd::u32>(half), &alloc);

        int bad = 0;
        for (int jj = 0; jj < half; ++jj)
        {
            const crd::f64 tr_ref = twr[jj] * ir[jj + half] - twi[jj] * ii[jj + half];
            const crd::f64 ti_ref = twr[jj] * ii[jj + half] + twi[jj] * ir[jj + half];
            if (fabs64(orr[jj] - (ir[jj] + tr_ref)) > 1e-4) { ++bad; }
            if (fabs64(oi[jj] - (ii[jj] + ti_ref)) > 1e-4) { ++bad; }
            if (fabs64(orr[jj + half] - (ir[jj] - tr_ref)) > 1e-4) { ++bad; }
            if (fabs64(oi[jj + half] - (ii[jj] - ti_ref)) > 1e-4) { ++bad; }
        }
        CHECK(bad == 0);
    }

    SECTION("upper half zero — butterfly degenerates to a copy (BIT-EXACT: twiddle × 0)")
    {
        crd::f64 ir[n];
        crd::f64 ii[n];
        crd::f64 orr[n];
        crd::f64 oi[n];
        for (int i = 0; i < n; ++i) { ir[i] = (i < half) ? static_cast<crd::f64>(i + 1) : 0.0; ii[i] = 0.0; orr[i] = -9.0; oi[i] = -9.0; }
        kir::KernelBuffer bufs[6] = {{ir, n, 0, 0}, {ii, n, 0, 1}, {twr, half, 0, 2}, {twi, half, 0, 3}, {orr, n, 0, 4}, {oi, n, 0, 5}};
        kir::eval_cpu_kernel(g, e, bufs, 6, static_cast<crd::u32>(half), &alloc);

        int bad = 0;
        for (int jj = 0; jj < half; ++jj) // x[j+half]=0 ⇒ out[j]=out[j+half]=x[j], exactly
        {
            if (orr[jj] != ir[jj] || orr[jj + half] != ir[jj]) { ++bad; }
            if (oi[jj] != 0.0 || oi[jj + half] != 0.0) { ++bad; }
        }
        CHECK(bad == 0);
    }
}

// MATERIALIZE: freeze a shared value into a register so it SURVIVES a later overwrite of that shared slot (the register-
// residency / single-buffer time-multiplexed exchange the FFT crush needs). Without stmt_materialize, the final read would
// see the overwritten 999; with it, the frozen reversed value is returned.
TEST_CASE("B-cmp: MATERIALIZE freezes a shared value across an overwrite (CPU oracle)", "[kir][kernel]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              ls  = 8;
    const kir::Shape           sh1 = kir::make_shape({1});
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, kir::DType::F32); };

    const int inbuf  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int outbuf = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int smem   = g.shared_decl(kir::DType::F32, ls);
    const int lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex);

    const int mark = g.kernel_stmt_mark();
    g.stmt_shared_store(smem, lid, g.buffer_load(inbuf, lid));
    g.stmt_barrier();
    const int rev = g.binary(kir::KOp::Sub, g.constant(static_cast<crd::f64>(ls - 1), sh1, kir::DType::U32), lid);
    const int r   = g.shared_load(smem, rev); // this thread's reversed value
    g.stmt_materialize(r);                     // FREEZE it before the overwrite
    g.stmt_shared_store(smem, lid, ku(999));   // clobber the whole array
    g.stmt_barrier();
    g.stmt_buffer_store(outbuf, lid, r);       // must be the frozen reversed value, NOT 999

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = ls;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    crd::f64 in[ls];
    crd::f64 out[ls];
    for (int i = 0; i < ls; ++i) { in[i] = static_cast<crd::f64>(i + 1); out[i] = -1.0; }
    kir::KernelBuffer bufs[2] = {{in, ls, 0, 0}, {out, ls, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, static_cast<crd::u32>(ls), &alloc);

    int bad = 0;
    for (int i = 0; i < ls; ++i) { if (out[i] != in[ls - 1 - i]) { ++bad; } } // reversed, not 999
    CHECK(bad == 0);
}
