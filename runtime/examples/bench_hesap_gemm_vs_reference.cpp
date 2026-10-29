// bench_hesap_gemm_vs_reference — Phase 3.1.6 v0d-parallelism-scaling.
//
// Head-to-head GEMM shootout: Cerid `gemm_parallel` vs Eigen-MT vs OpenBLAS.
// Identical workloads, identical matrices, same dev box. Exact GFLOPS + per-
// op wall time + speedup-vs-Cerid for both references. Goal: Cerid >= Eigen-MT
// AND Cerid >= OpenBLAS at every measured N, for both f32 and f64.
//
// Build only when -DCRD_BUILD_HESAP_VS_REFERENCE=ON is passed at configure.
// Eigen + OpenBLAS are fetched into build/_deps/ on first configure; Eigen runs in
// its peer library at the host's best ISA (examples/peers/eigen_peer.hpp).

#include "eigen_peer.hpp"

#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cblas.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace
{
crd::f64 measure_clock_ghz()
{
    const auto t0 = std::chrono::steady_clock::now();
    volatile crd::u64 x = 0;
    for (crd::u64 i = 0; i < 100000000ULL; ++i)
    {
        x += i;
    }
    (void)x;
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
    return 100.0 / ms;
}

// FLOPs per cycle of one core at the level the dense kernels run (CRD_CPU_LEVEL forces it): two FMA ports on Intel
// (AMD Zen has one, so half), the 128-bit levels without FMA a multiply and an add port, scalar one of each.
crd::f64 peak_gflops_per_core(crd::f64 clock_ghz, bool f64)
{
    crd::f64 f32_per_cycle = 2.0;
    switch (crd::hesap::dense::dense_kernels().level)
    {
        case crd::CpuLevel::Avx512:
            f32_per_cycle = 64.0;
            break;
        case crd::CpuLevel::Avx2:
            f32_per_cycle = 32.0;
            break;
        case crd::CpuLevel::Neon:
            f32_per_cycle = 16.0;
            break;
        case crd::CpuLevel::Sse2:
        case crd::CpuLevel::Sse42:
            f32_per_cycle = 8.0;
            break;
        case crd::CpuLevel::Portable:
            return 2.0 * clock_ghz;
    }
    return (f64 ? f32_per_cycle / 2.0 : f32_per_cycle) * clock_ghz;
}

template <typename T> struct Bench
{
    crd::f64 cerid_gflops;
    crd::f64 eigen_gflops;
    crd::f64 openblas_gflops;
    int cerid_iters;
    int eigen_iters;
    int openblas_iters;
    crd::u32 cerid_best_nw;        // best worker count picked from {8,16,24,32}
    crd::f64 max_rel_err_eigen;    // max|c_cerid - c_eigen| / max|c_eigen|
    crd::f64 max_rel_err_openblas; // max|c_cerid - c_openblas| / max|c_openblas|
};

template <typename T> crd::f64 max_rel_err(const T* a, const T* b, crd::usize n)
{
    crd::f64 max_abs = 0.0;
    crd::f64 max_diff = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        const crd::f64 va = static_cast<crd::f64>(a[i]);
        const crd::f64 vb = static_cast<crd::f64>(b[i]);
        const crd::f64 d = std::fabs(va - vb);
        const crd::f64 m = std::fabs(vb);
        if (d > max_diff)
        {
            max_diff = d;
        }
        if (m > max_abs)
        {
            max_abs = m;
        }
    }
    return max_abs > 0.0 ? (max_diff / max_abs) : 0.0;
}

template <typename T> void fill(T* p, crd::usize n, crd::u32 seed)
{
    crd::u32 s = seed;
    for (crd::usize i = 0; i < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        p[i] = static_cast<T>(static_cast<crd::i32>(s >> 8) % 1000) * static_cast<T>(0.001);
    }
}

template <typename Op> crd::f64 time_loop(const Op& op, int& iters_out)
{
    op(); // warm-up 1
    op(); // warm-up 2 — large-N GEMM benefits from a second warm-up.
    op(); // warm-up 3 — Eigen's OpenMP thread launch settles after several
          // calls; under-warming penalizes Eigen-MT at small N by 100-200x.
    op();
    // Best of 3 measurement runs — Eigen's OpenMP thread-launch variance
    // at small N otherwise dominates the bench. For each library we ask:
    // "what is the fastest sustained throughput it can produce?"
    crd::f64 best_gflops_inv = 1e300;
    int best_iters = 0;
    crd::f64 best_elapsed = 0.0;
    for (int trial = 0; trial < 3; ++trial)
    {
        const auto t0 = std::chrono::steady_clock::now();
        int iters = 0;
        while (true)
        {
            op();
            ++iters;
            const auto el = std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
            if (el > 0.6 && iters >= 5)
            {
                break;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const crd::f64 elapsed = std::chrono::duration<crd::f64>(t1 - t0).count();
        // We want the lowest "elapsed / iters" (= best throughput per call).
        const crd::f64 inv = elapsed / iters;
        if (inv < best_gflops_inv)
        {
            best_gflops_inv = inv;
            best_iters = iters;
            best_elapsed = elapsed;
        }
    }
    iters_out = best_iters;
    return best_elapsed;
}

// --one-thread: every library on one thread at 64 to 1024 (the per-level kernel board); the default is the
// multithreaded shootout at 256 to 4096, where Cerid takes its best worker count.
bool g_one_thread = false;
constexpr crd::u32 kWorkerCounts[] = {1U, 8U, 16U, 24U, 32U};

template <typename T> Bench<T> run_at_n(crd::usize n, crd::u32 num_workers, crd::memory::IAllocator* alloc)
{
    using namespace crd::hesap::dense;
    namespace peer = crd_peer::eigen;

    // ---- Cerid -------------------------------------------------------------
    Matrix<T> a(alloc, n, n);
    Matrix<T> b(alloc, n, n);
    Matrix<T> c_cerid(alloc, n, n);
    fill(a.data(), n * n, 11U);
    fill(b.data(), n * n, 22U);
    fill(c_cerid.data(), n * n, 0U);

    // Auto-pick best worker count. Include 1 (serial fallback) for small
    // matrices where parallel_for overhead exceeds the work.
    crd::u32 best_nw = num_workers;
    int cerid_iters = 0;
    crd::f64 best_gflops = 0.0;
    for (const crd::u32 nw : kWorkerCounts)
    {
        if (g_one_thread && nw != 1U)
        {
            continue;
        }
        int iters = 0;
        const crd::f64 elapsed = time_loop(
            [&]()
            {
                std::memset(c_cerid.data(), 0, n * n * sizeof(T));
                gemm_parallel<T, Layout::RowMajor>(nw, T{1}, a, b, T{}, c_cerid);
            },
            iters);
        const crd::f64 nd = static_cast<crd::f64>(n);
        const crd::f64 g = (2.0 * nd * nd * nd * iters) / (elapsed * 1e9);
        if (g > best_gflops)
        {
            best_gflops = g;
            best_nw = nw;
            cerid_iters = iters;
        }
    }
    // Re-run best to get a clean c_cerid for validation.
    std::memset(c_cerid.data(), 0, n * n * sizeof(T));
    gemm_parallel<T, Layout::RowMajor>(best_nw, T{1}, a, b, T{}, c_cerid);
    const crd::f64 nd_for_calc = static_cast<crd::f64>(n);
    const crd::f64 cerid_elapsed = (2.0 * nd_for_calc * nd_for_calc * nd_for_calc * cerid_iters) / (best_gflops * 1e9);

    // ---- Eigen-MT ----------------------------------------------------------
    // Read as column-major, the row-major operands are their transposes, so Eigen computes C^T = B^T * A^T, whose
    // column-major storage is C row-major: the kernel Eigen's row-major product runs.
    const auto ni = static_cast<peer::Index>(n);
    const peer::Owned eat{peer::mat(ni, ni, a.data(), false)};
    const peer::Owned ebt{peer::mat(ni, ni, b.data(), false)};
    const peer::Owned ect{peer::mat(ni, ni, static_cast<const T*>(nullptr), false)};
    peer::set_threads(static_cast<int>(num_workers));
    int eigen_iters = 0;
    const crd::f64 eigen_elapsed = time_loop([&]() { peer::gemm(ebt.get(), eat.get(), ect.get()); }, eigen_iters);

    // ---- OpenBLAS ----------------------------------------------------------
    openblas_set_num_threads(static_cast<int>(num_workers));
    Matrix<T> c_ob(alloc, n, n);
    int ob_iters = 0;
    const crd::f64 ob_elapsed = time_loop(
        [&]()
        {
            std::memset(c_ob.data(), 0, n * n * sizeof(T));
            if constexpr (std::is_same_v<T, crd::f32>)
            {
                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, static_cast<int>(n), static_cast<int>(n),
                            static_cast<int>(n), 1.0F, a.data(), static_cast<int>(n), b.data(), static_cast<int>(n),
                            0.0F, c_ob.data(), static_cast<int>(n));
            }
            else
            {
                cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, static_cast<int>(n), static_cast<int>(n),
                            static_cast<int>(n), 1.0, a.data(), static_cast<int>(n), b.data(), static_cast<int>(n), 0.0,
                            c_ob.data(), static_cast<int>(n));
            }
        },
        ob_iters);

    // ---- Validation: ensure outputs agree before trusting the GFLOPS -------
    // Tolerance accounts for different summation orders across the three
    // implementations. ADR-0063 only guarantees bit-exact within Cerid;
    // Eigen / OpenBLAS use FMA + different reduction trees.
    const crd::f64 err_eigen = max_rel_err(c_cerid.data(), peer::data(ect.get()), n * n);
    const crd::f64 err_ob = max_rel_err(c_cerid.data(), c_ob.data(), n * n);

    const crd::f64 nd = static_cast<crd::f64>(n);
    const crd::f64 flop_per_iter = 2.0 * nd * nd * nd;
    return {flop_per_iter * cerid_iters / (cerid_elapsed * 1e9),
            flop_per_iter * eigen_iters / (eigen_elapsed * 1e9),
            flop_per_iter * ob_iters / (ob_elapsed * 1e9),
            cerid_iters,
            eigen_iters,
            ob_iters,
            best_nw,
            err_eigen,
            err_ob};
}

template <typename T>
void shootout(const char* label, crd::f64 peak_per_core, crd::u32 nw, crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== %s shootout, %u workers (single-core peak %.1f GFLOPS) ====\n", label, nw,
                 peak_per_core);
    std::fprintf(stdout, "%-6s | %-26s | %-22s | %-22s | %-10s | %-10s | %-10s | %-10s\n", "N",
                 "Cerid (GFLOPS,iters,nw)", "Eigen (GFLOPS,iters)", "OBLAS (GFLOPS,iters)", "C/Eigen", "C/OBLAS",
                 "err Eigen", "err OBLAS");
    std::fprintf(stdout, "%s\n",
                 "----------------------------------------------------------------------------"
                 "------------------------------------------------------------");
    constexpr crd::usize sizes[] = {64, 256, 512, 1024, 2048, 4096};
    for (const crd::usize n : sizes)
    {
        if ((g_one_thread && n > 1024) || (!g_one_thread && n == 64))
        {
            continue;
        }
        const auto r = run_at_n<T>(n, nw, alloc);
        const char* eigen_pass = (r.max_rel_err_eigen < 1e-3) ? "" : " !MISMATCH!";
        const char* ob_pass = (r.max_rel_err_openblas < 1e-3) ? "" : " !MISMATCH!";
        std::fprintf(
            stdout, "%-6zu | %8.2f (%4d,nw=%2u) | %8.2f (%4d) | %8.2f (%4d) | %8.2fx | %8.2fx | %9.2e%s | %9.2e%s\n",
            static_cast<std::size_t>(n), r.cerid_gflops, r.cerid_iters, r.cerid_best_nw, r.eigen_gflops, r.eigen_iters,
            r.openblas_gflops, r.openblas_iters, r.cerid_gflops / r.eigen_gflops, r.cerid_gflops / r.openblas_gflops,
            r.max_rel_err_eigen, eigen_pass, r.max_rel_err_openblas, ob_pass);
        std::fflush(stdout);
    }
}

// v5a-4 OpenBLAS-on-shapes probe (the RIGHT denominator). Times OpenBLAS on Cerid's ACTUAL
// cmod (NT-ColMajor dgemm) and cdiv (dtrsm below-solve + dgemm/dsyrk trailing) shapes, SINGLE
// THREADED, so we compare per-phase against the real opponent — not against peak. dtrsm/dsyrk
// are inherently far below dgemm even in OpenBLAS, so "% of peak" misleads; this shows whether
// cdiv's 32 GF/s is near OpenBLAS's achievable ceiling (don't grind) or has real headroom.
void probe_openblas_kernel_shapes(crd::memory::IAllocator* alloc)
{
    using crd::hesap::dense::Layout;
    using crd::hesap::dense::Matrix;
    const int saved = openblas_get_num_threads();
    openblas_set_num_threads(1); // per-thread comparison vs Cerid's 1-thread phase numbers
    auto fill = [](Matrix<double, Layout::ColMajor>& m, double base)
    {
        for (crd::usize j = 0; j < m.cols(); ++j)
        {
            for (crd::usize i = 0; i < m.rows(); ++i)
            {
                m(i, j) = base + 0.001 * static_cast<double>((i + 7 * j) % 13);
            }
        }
    };
    auto best5 = [](auto&& fn) -> double
    {
        double best = 1e30;
        for (int r = 0; r < 5; ++r)
        {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            best = s < best ? s : best;
        }
        return best;
    };
    std::fprintf(stdout, "\n==== OpenBLAS-on-shapes probe (1 thread; Cerid's ACTUAL cmod/cdiv shapes) ====\n");
    std::fprintf(stdout, "  compare vs Cerid in-situ 1T: cmod ~51.7 GF/s | cdiv-A/below ~26 | cdiv-B/trailing ~38\n");
    const int cmod_m[2] = {512, 2048};
    for (int si = 0; si < 2; ++si) // cmod: C(M×N) = A(M×K)·B(N×K)^T, ColMajor NT
    {
        const int mm = cmod_m[si];
        const int nn = 512;
        const int kk = 200;
        Matrix<double, Layout::ColMajor> a(alloc, static_cast<crd::usize>(mm), static_cast<crd::usize>(kk));
        Matrix<double, Layout::ColMajor> b(alloc, static_cast<crd::usize>(nn), static_cast<crd::usize>(kk));
        Matrix<double, Layout::ColMajor> c(alloc, static_cast<crd::usize>(mm), static_cast<crd::usize>(nn));
        fill(a, 0.5);
        fill(b, 0.3);
        fill(c, 0.0);
        const double s = best5(
            [&]
            {
                cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, mm, nn, kk, 1.0, a.data(), mm, b.data(), nn, 0.0,
                            c.data(), mm);
            });
        std::fprintf(stdout, "  OBLAS dgemm  cmod-NT   M=%-4d N=%-3d K=%-3d : %6.1f GF/s\n", mm, nn, kk,
                     2.0 * mm * nn * kk / s / 1e9);
    }
    { // cdiv below-solve: X·L11^T = A21, X=(below×nc), L11=(nc×nc) lower-tri  → cblas_dtrsm Right/Lower/Trans
        const int below = 1500;
        const int nc = 256;
        Matrix<double, Layout::ColMajor> l11(alloc, static_cast<crd::usize>(nc), static_cast<crd::usize>(nc));
        Matrix<double, Layout::ColMajor> x(alloc, static_cast<crd::usize>(below), static_cast<crd::usize>(nc));
        for (crd::usize j = 0; j < l11.cols(); ++j) // diag-dominant lower
        {
            for (crd::usize i = 0; i < l11.rows(); ++i)
            {
                const double lower = i > j ? 0.01 : 0.0;
                l11(i, j) = i == j ? static_cast<double>(2 * nc) : lower;
            }
        }
        fill(x, 0.4);
        const double s = best5(
            [&] {
                cblas_dtrsm(CblasColMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit, below, nc, 1.0, l11.data(),
                            nc, x.data(), below);
            });
        std::fprintf(stdout, "  OBLAS dtrsm  cdiv-blw  below=%-4d nc=%-3d : %6.1f GF/s\n", below, nc,
                     static_cast<double>(below) * nc * nc / s / 1e9);
    }
    { // cdiv trailing: Cerid's B is a full dgemm; dsyrk is the symmetric-block (the syrk lever) ceiling
        const int mm = 1500;
        const int nn = 512;
        const int kk = 256;
        Matrix<double, Layout::ColMajor> a(alloc, static_cast<crd::usize>(mm), static_cast<crd::usize>(kk));
        Matrix<double, Layout::ColMajor> b(alloc, static_cast<crd::usize>(nn), static_cast<crd::usize>(kk));
        Matrix<double, Layout::ColMajor> c(alloc, static_cast<crd::usize>(mm), static_cast<crd::usize>(nn));
        fill(a, 0.5);
        fill(b, 0.3);
        fill(c, 0.1);
        const double s = best5(
            [&]
            {
                cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, mm, nn, kk, -1.0, a.data(), mm, b.data(), nn, 1.0,
                            c.data(), mm);
            });
        std::fprintf(stdout, "  OBLAS dgemm  cdiv-trl  M=%-4d N=%-3d K=%-3d : %6.1f GF/s\n", mm, nn, kk,
                     2.0 * mm * nn * kk / s / 1e9);
        const int n = 512;
        const int k = 256;
        Matrix<double, Layout::ColMajor> as(alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(k));
        Matrix<double, Layout::ColMajor> cs(alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(n));
        fill(as, 0.5);
        fill(cs, 0.1);
        const double ss =
            best5([&] { cblas_dsyrk(CblasColMajor, CblasLower, CblasNoTrans, n, k, -1.0, as.data(), n, 1.0, cs.data(), n); });
        std::fprintf(stdout, "  OBLAS dsyrk  cdiv-sym  n=%-4d k=%-3d     : %6.1f GF/s (syrk ceiling for the sym block)\n",
                     n, k, static_cast<double>(n) * n * k / ss / 1e9);
    }
    openblas_set_num_threads(saved);
}
} // namespace

int main(int argc, char** argv)
{
    // On the i9-14900K hybrid CPU (8 P-cores + 16 E-cores) the 32-thread
    // default schedule has E-cores bottlenecking every parallel barrier.
    // Constrain BOTH Cerid and Eigen to the P-core hyperthreads only —
    // 16 logical threads, all on P-cores — for a fair apples-to-apples
    // benchmark. Pass --all-cores to opt out and use the full 32.
    bool p_cores_only = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--all-cores") == 0)
        {
            p_cores_only = false;
        }
        if (std::strcmp(argv[i], "--one-thread") == 0)
        {
            g_one_thread = true;
        }
    }
#ifdef _WIN32
    if (p_cores_only)
    {
        // P-core HT bits = 0..15 on 14900K (16 logical P-threads).
        const DWORD_PTR mask = 0xFFFFULL;
        if (!SetProcessAffinityMask(GetCurrentProcess(), mask))
        {
            std::fprintf(stderr, "[bench] WARNING: SetProcessAffinityMask failed\n");
        }
        else
        {
            std::fprintf(stdout, "[bench] Affinity locked to P-cores (mask=0x%llX, 16 logical threads)\n",
                         static_cast<unsigned long long>(mask));
        }
    }
#endif
    // Match jobs worker count to actual usable threads. 16 fibers on 16
    // P-threads avoids the worker-vs-affinity-slot contention.
    crd::jobs::Config jobs_cfg;
    const crd::u32 shootout_threads = p_cores_only ? 16U : 0U;
    jobs_cfg.num_threads = g_one_thread ? 1U : shootout_threads;
    crd::jobs::init(jobs_cfg);
    const crd::f64 ghz = measure_clock_ghz();
    const crd::f64 peak_f32 = peak_gflops_per_core(ghz, false);
    const crd::f64 peak_f64 = peak_gflops_per_core(ghz, true);
    const crd::u32 nw = crd::jobs::num_workers();

    // Use jobs worker count for everyone — that's the actual scheduling budget.
    const crd::u32 effective_threads = crd::jobs::num_workers();
    crd_peer::eigen::set_threads(static_cast<int>(effective_threads));
    const int eigen_nb = crd_peer::eigen::threads();
    openblas_set_num_threads(static_cast<int>(effective_threads));
    const int ob_nb = openblas_get_num_threads();

    std::fprintf(stdout,
                 "==== bench_hesap_gemm_vs_reference (v0d-parallelism-scaling) ====\n"
                 "  SIMD backend  : %s\n"
                 "  Dense kernels : %s level (CRD_CPU_LEVEL forces it)\n"
                 "  Clock         : %.2f GHz\n"
                 "  Per-core peak : f32 = %.1f GFLOPS, f64 = %.1f GFLOPS\n"
                 "  Cerid jobs    : %u workers (incl. main)\n"
                 "  Eigen         : %s\n"
                 "  Eigen threads : Eigen::nbThreads() = %d %s\n"
                 "  OpenBLAS thr. : openblas_get_num_threads() = %d %s\n"
                 "  Goal          : Cerid >= Eigen-MT AND Cerid >= OpenBLAS at every N\n",
                 crd::math::simd::backend_name(), crd::cpu_level_name(crd::hesap::dense::dense_kernels().level), ghz,
                 peak_f32, peak_f64, nw, crd_peer::eigen::isa(), eigen_nb,
                 (eigen_nb > 1 ? "(MT)" : "(SERIAL — Eigen will not scale!)"), ob_nb,
                 (ob_nb > 1 ? "(MT)" : "(SERIAL — OpenBLAS will not scale!)"));

    {
        crd::memory::TlsfAllocator alloc(1024ULL * 1024ULL * 1024ULL); // 1 GB
        shootout<crd::f32>("f32 GEMM", peak_f32, nw, &alloc);
    }
    {
        crd::memory::TlsfAllocator alloc(1024ULL * 1024ULL * 1024ULL);
        shootout<crd::f64>("f64 GEMM", peak_f64, nw, &alloc);
    }
    {
        // Cerid-vs-OpenBLAS at the ACTUAL cmod/cdiv shapes (v5a-4). NOTE: meaningful only with a
        // NATIVE OpenBLAS (Linux CI / system libopenblas); the Windows CPM `_deps` OpenBLAS is the
        // slow f2c accuracy-oracle build (BLAS ~27 GF/s) — for the real per-phase headroom number,
        // run via the cholmod bench (WSL system libopenblas) instead.
        crd::memory::TlsfAllocator alloc(1024ULL * 1024ULL * 1024ULL);
        if (!g_one_thread)
        {
            probe_openblas_kernel_shapes(&alloc);
        }
    }

    std::fprintf(stdout, "\nDone.\n");
    crd::jobs::shutdown();
    return 0;
}
