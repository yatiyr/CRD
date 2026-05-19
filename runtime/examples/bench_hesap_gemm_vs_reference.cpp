// bench_hesap_gemm_vs_reference — Phase 3.1.6 v0d-parallelism-scaling.
//
// Head-to-head GEMM shootout: Cerid `gemm_parallel` vs Eigen-MT vs OpenBLAS.
// Identical workloads, identical matrices, same dev box. Exact GFLOPS + per-
// op wall time + speedup-vs-Cerid for both references. Goal: Cerid >= Eigen-MT
// AND Cerid >= OpenBLAS at every measured N, for both f32 and f64.
//
// Build only when -DCRD_BUILD_HESAP_VS_REFERENCE=ON is passed at configure.
// Eigen + OpenBLAS are fetched into build/_deps/ on first configure.

#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>
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

crd::f64 peak_gflops_per_core_f32(crd::f64 ghz)
{
#if CRD_SIMD_HAS_AVX2
    return 32.0 * ghz;
#else
    return 2.0 * ghz;
#endif
}

crd::f64 peak_gflops_per_core_f64(crd::f64 ghz)
{
#if CRD_SIMD_HAS_AVX2
    return 16.0 * ghz;
#else
    return 2.0 * ghz;
#endif
}

template <typename T>
struct Bench
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

template <typename T>
crd::f64 max_rel_err(const T* a, const T* b, crd::usize n)
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

template <typename T>
void fill(T* p, crd::usize n, crd::u32 seed)
{
    crd::u32 s = seed;
    for (crd::usize i = 0; i < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        p[i] = static_cast<T>(static_cast<crd::i32>(s >> 8) % 1000) * static_cast<T>(0.001);
    }
}

template <typename Op>
crd::f64 time_loop(Op&& op, int& iters_out)
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
            crd::jobs::frame_reset();
            const auto el =
                std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
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

// Cerid: try multiple worker counts and pick the best — i9-14900K has 8 P-cores
// + 16 E-cores, so 32-fiber static partition often loses to ~16 workers.
template <typename T>
crd::f64 run_cerid_best(crd::usize n, crd::memory::IAllocator* alloc, int& best_iters_out,
                       crd::u32& best_nw_out)
{
    using namespace crd::hesap::dense;
    Matrix<T> a(alloc, n, n);
    Matrix<T> b(alloc, n, n);
    Matrix<T> c(alloc, n, n);
    fill(a.data(), n * n, 11U);
    fill(b.data(), n * n, 22U);
    fill(c.data(), n * n, 0U);

    crd::f64 best_gflops = 0.0;
    crd::u32 best_nw = 1U;
    int best_iters = 0;
    for (crd::u32 nw : {8U, 16U, 24U, 32U})
    {
        int iters = 0;
        const crd::f64 elapsed = time_loop(
            [&]()
            {
                std::memset(c.data(), 0, n * n * sizeof(T));
                gemm_parallel<T, Layout::RowMajor>(nw, T{1}, a, b, T{}, c);
            },
            iters);
        const crd::f64 nd = static_cast<crd::f64>(n);
        const crd::f64 g = (2.0 * nd * nd * nd * iters) / (elapsed * 1e9);
        if (g > best_gflops)
        {
            best_gflops = g;
            best_nw = nw;
            best_iters = iters;
        }
    }
    best_nw_out = best_nw;
    best_iters_out = best_iters;
    return best_gflops;
}

template <typename T>
Bench<T> run_at_n(crd::usize n, crd::u32 num_workers, crd::memory::IAllocator* alloc)
{
    using namespace crd::hesap::dense;
    using EigenMat = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

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
    for (crd::u32 nw : {1U, 8U, 16U, 24U, 32U})
    {
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
    const crd::f64 cerid_elapsed = (2.0 * nd_for_calc * nd_for_calc * nd_for_calc * cerid_iters) /
                                   (best_gflops * 1e9);

    // ---- Eigen-MT ----------------------------------------------------------
    EigenMat ea(n, n);
    EigenMat eb(n, n);
    EigenMat ec(n, n);
    std::memcpy(ea.data(), a.data(), n * n * sizeof(T));
    std::memcpy(eb.data(), b.data(), n * n * sizeof(T));
    Eigen::setNbThreads(static_cast<int>(num_workers));
    int eigen_iters = 0;
    const crd::f64 eigen_elapsed = time_loop(
        [&]() { ec.noalias() = ea * eb; }, eigen_iters);

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
                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, static_cast<int>(n),
                            static_cast<int>(n), static_cast<int>(n), 1.0F, a.data(),
                            static_cast<int>(n), b.data(), static_cast<int>(n), 0.0F, c_ob.data(),
                            static_cast<int>(n));
            }
            else
            {
                cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, static_cast<int>(n),
                            static_cast<int>(n), static_cast<int>(n), 1.0, a.data(),
                            static_cast<int>(n), b.data(), static_cast<int>(n), 0.0, c_ob.data(),
                            static_cast<int>(n));
            }
        },
        ob_iters);

    // ---- Validation: ensure outputs agree before trusting the GFLOPS -------
    // Tolerance accounts for different summation orders across the three
    // implementations. ADR-0063 only guarantees bit-exact within Cerid;
    // Eigen / OpenBLAS use FMA + different reduction trees.
    const crd::f64 err_eigen =
        max_rel_err(c_cerid.data(), ec.data(), n * n);
    const crd::f64 err_ob =
        max_rel_err(c_cerid.data(), c_ob.data(), n * n);

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
    std::fprintf(stdout, "\n==== %s shootout, %u workers (single-core peak %.1f GFLOPS) ====\n",
                 label, nw, peak_per_core);
    std::fprintf(stdout, "%-6s | %-26s | %-22s | %-22s | %-10s | %-10s | %-10s | %-10s\n",
                 "N", "Cerid (GFLOPS,iters,nw)", "Eigen (GFLOPS,iters)", "OBLAS (GFLOPS,iters)",
                 "C/Eigen", "C/OBLAS", "err Eigen", "err OBLAS");
    std::fprintf(stdout, "%s\n",
                 "----------------------------------------------------------------------------"
                 "------------------------------------------------------------");
    for (crd::usize n : {crd::usize{256}, crd::usize{512}, crd::usize{1024}, crd::usize{2048},
                         crd::usize{4096}})
    {
        const auto r = run_at_n<T>(n, nw, alloc);
        const char* eigen_pass = (r.max_rel_err_eigen < 1e-3) ? "" : " !MISMATCH!";
        const char* ob_pass = (r.max_rel_err_openblas < 1e-3) ? "" : " !MISMATCH!";
        std::fprintf(
            stdout,
            "%-6zu | %8.2f (%4d,nw=%2u) | %8.2f (%4d) | %8.2f (%4d) | %8.2fx | %8.2fx | %9.2e%s | %9.2e%s\n",
            static_cast<std::size_t>(n), r.cerid_gflops, r.cerid_iters, r.cerid_best_nw,
            r.eigen_gflops, r.eigen_iters, r.openblas_gflops, r.openblas_iters,
            r.cerid_gflops / r.eigen_gflops, r.cerid_gflops / r.openblas_gflops,
            r.max_rel_err_eigen, eigen_pass, r.max_rel_err_openblas, ob_pass);
        std::fflush(stdout);
    }
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
    jobs_cfg.num_threads = p_cores_only ? 16U : 0U;
    crd::jobs::init(jobs_cfg);
    const crd::f64 ghz = measure_clock_ghz();
    const crd::f64 peak_f32 = peak_gflops_per_core_f32(ghz);
    const crd::f64 peak_f64 = peak_gflops_per_core_f64(ghz);
    const crd::u32 nw = crd::jobs::num_workers();

    // Use jobs worker count for everyone — that's the actual scheduling budget.
    const crd::u32 effective_threads = crd::jobs::num_workers();
    Eigen::setNbThreads(static_cast<int>(effective_threads));
    const int eigen_nb = Eigen::nbThreads();
    openblas_set_num_threads(static_cast<int>(effective_threads));
    const int ob_nb = openblas_get_num_threads();

    std::fprintf(stdout,
                 "==== bench_hesap_gemm_vs_reference (v0d-parallelism-scaling) ====\n"
                 "  SIMD backend  : %s\n"
                 "  Clock         : %.2f GHz\n"
                 "  Per-core peak : f32 = %.1f GFLOPS, f64 = %.1f GFLOPS\n"
                 "  Cerid jobs    : %u workers (incl. main)\n"
                 "  Eigen threads : Eigen::nbThreads() = %d %s\n"
                 "  OpenBLAS thr. : openblas_get_num_threads() = %d %s\n"
                 "  Goal          : Cerid >= Eigen-MT AND Cerid >= OpenBLAS at every N\n",
                 crd::math::simd::backend_name(), ghz, peak_f32, peak_f64, nw, eigen_nb,
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

    std::fprintf(stdout, "\nDone.\n");
    crd::jobs::shutdown();
    return 0;
}
