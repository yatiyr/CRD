#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/direct/multifrontal_cholesky_blr.hpp>
#include <crd/hesap/ordering/amd.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace sp = crd::hesap::sparse;
namespace dir = crd::hesap::direct;

namespace
{
// 2D 5-point Laplacian on a k×k grid (SPD: diagonal 4, 4 neighbors −1). Full symmetric CSC.
sp::SparseMatrix<double, sp::SparseFormat::Csc> laplacian_2d(crd::u32 k, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = k * k;
    sp::TripletBuilder<double> tb(alloc, n, n);
    for (crd::u32 y = 0; y < k; ++y)
    {
        for (crd::u32 x = 0; x < k; ++x)
        {
            const crd::u32 i = y * k + x;
            tb.add(i, i, 4.0);
            if (x > 0)
            {
                tb.add(i, i - 1, -1.0);
            }
            if (x + 1 < k)
            {
                tb.add(i, i + 1, -1.0);
            }
            if (y > 0)
            {
                tb.add(i, i - k, -1.0);
            }
            if (y + 1 < k)
            {
                tb.add(i, i + k, -1.0);
            }
        }
    }
    return sp::to_csc<double>(tb.compress(), alloc);
}

// 3D 7-point Laplacian on a k×k×k grid (SPD: diagonal 6, 6 neighbors −1) — BIG dense
// fronts (separator ~n^2/3), exercises the BLR front-factor path for real.
sp::SparseMatrix<double, sp::SparseFormat::Csc> laplacian_3d(crd::u32 k, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = k * k * k;
    sp::TripletBuilder<double> tb(alloc, n, n);
    const auto idx = [k](crd::u32 x, crd::u32 y, crd::u32 z) { return (z * k + y) * k + x; };
    for (crd::u32 z = 0; z < k; ++z)
    {
        for (crd::u32 y = 0; y < k; ++y)
        {
            for (crd::u32 x = 0; x < k; ++x)
            {
                const crd::u32 i = idx(x, y, z);
                tb.add(i, i, 6.0);
                if (x > 0)
                {
                    tb.add(i, idx(x - 1, y, z), -1.0);
                }
                if (x + 1 < k)
                {
                    tb.add(i, idx(x + 1, y, z), -1.0);
                }
                if (y > 0)
                {
                    tb.add(i, idx(x, y - 1, z), -1.0);
                }
                if (y + 1 < k)
                {
                    tb.add(i, idx(x, y + 1, z), -1.0);
                }
                if (z > 0)
                {
                    tb.add(i, idx(x, y, z - 1), -1.0);
                }
                if (z + 1 < k)
                {
                    tb.add(i, idx(x, y, z + 1), -1.0);
                }
            }
        }
    }
    return sp::to_csc<double>(tb.compress(), alloc);
}

// Relative solution error ‖x − x_true‖/‖x_true‖ of a solved A·x = A·x_true.
double check_solve(const sp::SparseMatrix<double, sp::SparseFormat::Csc>& a,
                   dir::MultifrontalCholeskyBlr<double>& chol, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.cols();
    crd::containers::Array<double> xt(alloc);
    crd::containers::Array<double> b(alloc);
    crd::containers::Array<double> x(alloc);
    xt.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = std::sin(static_cast<double>(i) * 0.1 + 0.3);
        b[i] = 0.0;
    }
    const auto& pat = a.pattern();
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 p = pat.outer_ptr[c]; p < pat.outer_ptr[c + 1]; ++p)
        {
            b[pat.inner_idx[p]] += a.values().values[p] * xt[c];
        }
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    if (!chol.solve(crd::containers::Span<double>{x.data(), n}))
    {
        return 1e30;
    }
    double e = 0.0;
    double nrm = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        e += (x[i] - xt[i]) * (x[i] - xt[i]);
        nrm += xt[i] * xt[i];
    }
    return std::sqrt(e) / std::sqrt(nrm);
}
} // namespace

TEST_CASE("v5e-3d MultifrontalCholeskyBlr: 2D Laplacian factor + solve (dense fronts)", "[hesap][mfblr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(512U) * 1024U * 1024U);
    const auto a = laplacian_2d(32, &alloc);  // n=1024
    dir::MultifrontalCholeskyBlr<double> chol(&alloc);
    REQUIRE(chol.factorize(a, 256, 1e-8, 100000));  // blr_min huge ⇒ all dense
    REQUIRE(chol.info() == 0);
    CHECK(check_solve(a, chol, &alloc) < 1e-8);
}

TEST_CASE("v5e-3d MultifrontalCholeskyBlr: 2D Laplacian via the BLR front path", "[hesap][mfblr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(512U) * 1024U * 1024U);
    const auto a = laplacian_2d(40, &alloc);  // n=1600
    dir::MultifrontalCholeskyBlr<double> chol(&alloc);
    REQUIRE(chol.factorize(a, 32, 1e-9, 1));  // blr_min=1 ⇒ EVERY front through the BLR path
    REQUIRE(chol.info() == 0);
    CHECK(check_solve(a, chol, &alloc) < 1e-6);  // BLR-approximate + IR recovers accuracy
}

TEST_CASE("v5e-3d MultifrontalCholeskyBlr: 3D Laplacian (big dense fronts) BLR path", "[hesap][mfblr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1024U) * 1024U * 1024U);
    const auto a = laplacian_3d(16, &alloc);  // n=4096, separator fronts ~256
    dir::MultifrontalCholeskyBlr<double> chol(&alloc);
    REQUIRE(chol.factorize(a, 128, 1e-9, 128));  // BLR on the large fronts
    REQUIRE(chol.info() == 0);
    CHECK(check_solve(a, chol, &alloc) < 1e-6);
}

// The determinism MOAT: tree-parallel factor + solve must be BIT-IDENTICAL across worker
// counts (the differentiator MUMPS/CHOLMOD lack). The factor is a pure function of the
// pattern; the per-front BLR/dense kernels + gemm fixed-reduction + fixed child order deliver it.
TEST_CASE("v5e-3d MultifrontalCholeskyBlr: tree-parallel factor+solve bit-identical {1,2,4,8}",
          "[hesap][mfblr][real][moat]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    const auto a = laplacian_3d(16, &alloc);  // n=4096; big near-root fronts ⇒ within-front gemm parallelism
    const crd::u32 n = a.cols();
    crd::containers::Array<double> b(&alloc);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = std::sin(static_cast<double>(i) * 0.37 + 0.1);
    }
    crd::containers::Array<double> ref(&alloc);
    crd::u64 ref_nnz = 0;
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);  // the within-front gemm uses `nw` workers (node-parallel)
        dir::MultifrontalCholeskyBlr<double> chol(&alloc);
        REQUIRE(chol.factorize(a, 128, 1e-9, 128));  // mix: BLR large + dense small fronts
        crd::containers::Array<double> x(&alloc);
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = b[i];
        }
        REQUIRE(chol.solve(crd::containers::Span<double>{x.data(), n}));
        crd::jobs::shutdown();
        if (!have_ref)
        {
            ref.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                ref[i] = x[i];
            }
            ref_nnz = chol.nnz();
            have_ref = true;
        }
        else
        {
            CHECK(chol.nnz() == ref_nnz);  // identical L structure
            bool ident = true;
            for (crd::u32 i = 0; i < n && ident; ++i)
            {
                ident = (x[i] == ref[i]);  // bit-identical solution ⇒ deterministic across worker counts
            }
            CHECK(ident);
        }
    }
}

// AMD-permuted 3D Laplacian (k³) — the realistic fill-reduced big-front corpus.
namespace
{
sp::SparseMatrix<double, sp::SparseFormat::Csc> laplacian_3d_amd(crd::u32 k, crd::memory::IAllocator* alloc)
{
    namespace ord = crd::hesap::ordering;
    const auto a0 = laplacian_3d(k, alloc);
    const auto perm = ord::amd_order(a0.pattern(), alloc);  // fill-reducing order
    const crd::u32 n = a0.cols();
    sp::TripletBuilder<double> tb(alloc, n, n);
    const auto& pat = a0.pattern();
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 p = pat.outer_ptr[c]; p < pat.outer_ptr[c + 1]; ++p)
        {
            tb.add(perm.inv_perm[pat.inner_idx[p]], perm.inv_perm[c], a0.values().values[p]);
        }
    }
    return sp::to_csc<double>(tb.compress(), alloc);
}
} // namespace

// v5e-3d crush signal: BLR driver vs DENSE driver factor time on the AMD-permuted 3D
// Laplacian at n≥110K (where MUMPS-BLR beats MUMPS-full ~2×). Build optimized + run:
//   crd-hesap-direct-tests "[mfblr-bench]"
TEST_CASE("v5e-3d MultifrontalCholeskyBlr crush signal (manual bench)", "[.][mfblr-bench]")
{
    using Clock = std::chrono::steady_clock;
    const auto secs = [](Clock::time_point a, Clock::time_point b)
    { return std::chrono::duration<double>(b - a).count(); };
    for (crd::u32 k : {40U, 48U, 56U})  // n = 64000, 110592, 175616 (>crossover; k=64 long-pole dropped)
    {
        crd::memory::GrowableTlsfAllocator alloc(crd::usize{512} << 20);  // 512MB chunks, unbounded
        const auto a = laplacian_3d_amd(k, &alloc);
        const crd::u32 n = a.cols();

        dir::MultifrontalCholeskyBlr<double> s1(&alloc);  // SERIAL (jobs down ⇒ within-front gemm serial)
        const auto t0 = Clock::now();
        REQUIRE(s1.factorize(a, 256, 1e-9, 100000000U));  // all dense
        const double tser = secs(t0, Clock::now());

        crd::jobs::init();  // node-parallel: the within-front gemm now uses jobs::num_workers()
        const crd::u32 nw = crd::jobs::num_workers();
        dir::MultifrontalCholeskyBlr<double> s2(&alloc);
        const auto t1 = Clock::now();
        REQUIRE(s2.factorize(a, 256, 1e-9, 100000000U));
        const double tpar = secs(t1, Clock::now());
        crd::jobs::shutdown();

        std::fprintf(stderr, "[mfblr] k=%2u n=%6u  serial=%.3fs  par(%uw)=%.3fs  speedup=%.2fx  nnz(L)=%llu\n", k, n,
                     tser, nw, tpar, tser / tpar, static_cast<unsigned long long>(s2.nnz()));
    }
}

// v5e-3 GATING CHECK (advisor #2): is BLR net-POSITIVE vs Cerid's OWN dense path at N≥110K?
// Both runs share the same jobs state (node-parallel scales both equally ⇒ the ratio is the
// fair dense-vs-BLR signal). Reports factor time AND solve accuracy (residual vs true x) for
// each — BLR is only a win if it is FASTER *and* IR still recovers a small residual.
//   crd-hesap-direct-tests "[mfblr-blrgate]"
TEST_CASE("v5e-3d MultifrontalCholeskyBlr: BLR vs own dense path (manual bench)", "[.][mfblr-blrgate]")
{
    using Clock = std::chrono::steady_clock;
    const auto secs = [](Clock::time_point a, Clock::time_point b)
    { return std::chrono::duration<double>(b - a).count(); };
    for (crd::u32 k : {48U, 56U})  // n = 110592, 175616 (>crossover; k=64 long-pole dropped)
    {
        crd::memory::GrowableTlsfAllocator alloc(crd::usize{512} << 20);
        const auto a = laplacian_3d_amd(k, &alloc);
        const crd::u32 n = a.cols();

        dir::MultifrontalCholeskyBlr<double> sd(&alloc);
        const auto t0 = Clock::now();
        REQUIRE(sd.factorize(a, 256, 1e-9, 100000000U));  // DENSE: blr_min = ∞
        const double tdense = secs(t0, Clock::now());
        const double accd = check_solve(a, sd, &alloc);
        const crd::u64 nnzd = sd.nnz();

        dir::MultifrontalCholeskyBlr<double> sb(&alloc);
        const auto t1 = Clock::now();
        REQUIRE(sb.factorize(a, 256, 1e-6, 512U));  // BLR: big fronts compressed at tol=1e-6
        const double tblr = secs(t1, Clock::now());
        const double accb = check_solve(a, sb, &alloc);
        const crd::u64 nnzb = sb.nnz();

        std::fprintf(stderr,
                     "[blrgate] k=%2u n=%6u | dense %.3fs acc=%.1e | BLR %.3fs acc=%.1e | "
                     "BLR/dense=%.2fx nnz %llu->%llu\n",
                     k, n, tdense, accd, tblr, accb, tblr / tdense,
                     static_cast<unsigned long long>(nnzd), static_cast<unsigned long long>(nnzb));
    }
}

// v5e-3 SERIAL-DENSE PHASE PROFILE (advisor: profile before cutting — the 2.3×-behind-MUMPS-full
// gap is likely symbolic/assembly, not the gemm kernel, per v5a/v5b history). Run with the env var
// set so the driver prints the [mfblr-prof] symbolic/assembly/factor/extract breakdown:
//   CRD_MFBLR_PROFILE=1 crd-hesap-direct-tests "[mfblr-prof]"
TEST_CASE("v5e-3d MultifrontalCholeskyBlr serial-dense phase profile (manual)", "[.][mfblr-prof]")
{
    using Clock = std::chrono::steady_clock;
    for (crd::u32 k : {48U, 56U})  // n = 110592, 175616 — the serial dense path vs MUMPS-full (AMD, same fill)
    {
        crd::memory::GrowableTlsfAllocator alloc(crd::usize{512} << 20);
        const auto a = laplacian_3d_amd(k, &alloc);
        dir::MultifrontalCholeskyBlr<double> sd(&alloc);
        const auto t0 = Clock::now();
        REQUIRE(sd.factorize(a, 256, 1e-9, 100000000U));  // DENSE, SERIAL (no jobs::init in this test)
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        std::fprintf(stderr, "[mfblr-prof] *** factorize WALL = %.3fs (n=%u, vs MUMPS-full %s) ***\n", wall, a.cols(),
                     k == 48 ? "3.36s" : "10.3s");
        CHECK(check_solve(a, sd, &alloc) < 1e-8);  // sanity: the factor is correct
    }
}

// v5e-3 LEG B Step 1 — reproduce the CURRENT parallel path (Leg A's syrk_lower_sub + direct-CSC
// feeding STRIDED sub-views into gemm_parallel_auto) under jobs::init, at sizes whose big fronts
// cross the parallel-gemm threshold (mnk≥256K). Run under ASan to localize any OOB:
//   CRD ... crd-hesap-direct-tests "[mfblr-par]"  (with the ASan DLL in PATH)
TEST_CASE("v5e-3d MultifrontalCholeskyBlr: PARALLEL dense factor (ASan repro)", "[.][mfblr-par]")
{
    for (crd::u32 k : {40U, 48U})  // n = 64000, 110592 — k=40 was the FrameArena-exhaust point, k=48 the orig corruption
    {
        crd::memory::GrowableTlsfAllocator alloc(crd::usize{512} << 20);
        const auto a = laplacian_3d_amd(k, &alloc);
        crd::jobs::init();  // parallel: within-front gemm uses all workers
        dir::MultifrontalCholeskyBlr<double> sd(&alloc);
        REQUIRE(sd.factorize(a, 256, 1e-9, 100000000U));  // DENSE, PARALLEL
        const double res = check_solve(a, sd, &alloc);
        crd::jobs::shutdown();
        std::fprintf(stderr, "[mfblr-par] k=%2u n=%6u  parallel factor OK  resid=%.2e\n", k, a.cols(), res);
        CHECK(res < 1e-8);
    }
}

TEST_CASE("v5e-3d MultifrontalCholeskyBlr: deterministic factor + solve", "[hesap][mfblr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(512U) * 1024U * 1024U);
    const auto a = laplacian_2d(40, &alloc);
    const crd::u32 n = a.cols();
    auto run = [&]()
    {
        dir::MultifrontalCholeskyBlr<double> chol(&alloc);
        REQUIRE(chol.factorize(a, 32, 1e-9, 1));
        crd::containers::Array<double> x(&alloc);
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = std::sin(static_cast<double>(i) * 0.2);
        }
        REQUIRE(chol.solve(crd::containers::Span<double>{x.data(), n}));
        return x;
    };
    const auto x1 = run();
    const auto x2 = run();
    bool ident = true;
    for (crd::u32 i = 0; i < n && ident; ++i)
    {
        ident = (x1[i] == x2[i]);
    }
    CHECK(ident);  // the moat: BLR front compression is deterministic
}
