// v14-h batched-GEMM wall-clock A/B: OUR batched_gemm (serial + jobs-8T) vs
// native MKL cblas_dgemm_batch_strided (compiled in when CRD_BENCH_MKL is
// defined — the WSL harness build/crd_batched_bench.sh links mkl_rt). torch /
// numpy / MATLAB rows come from scripts/v14h_peers.py + the MATLAB -batch
// call; boards -> docs/bench/2026-07-05-v14h-batched-la.md.
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/batched.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

#ifdef CRD_BENCH_MKL
#include <mkl.h>
#endif

using crd::hesap::tensor::batched_gemm;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

namespace
{

double now_ms()
{
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count()) *
           1e-6;
}

template <typename Fn>
double best_of(int reps, Fn&& fn)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        const double t0 = now_ms();
        fn();
        const double t1 = now_ms();
        if (t1 - t0 < best)
        {
            best = t1 - t0;
        }
    }
    return best;
}

} // namespace

int main()
{
    const crd::u64 sizes[] = {4U, 6U, 8U, 16U};
    const crd::u64 batches[] = {10000U, 100000U};
    for (const crd::u64 n : sizes)
    {
        for (const crd::u64 bsz : batches)
        {
            crd::memory::TlsfAllocator alloc(5ULL << 29); // 2.5 GB: n=16 b=100k holds seven 204 MB tensors
            const crd::u64 shp[3] = {bsz, n, n};
            Tensor<crd::f64> a(&alloc, {shp, 3});
            Tensor<crd::f64> b(&alloc, {shp, 3});
            Tensor<crd::f64> c(&alloc, {shp, 3});
            {
                crd::hesap::stats::PhiloxRng rng(1407U, n * 1000U + bsz);
                for (crd::u64 i = 0; i < bsz * n * n; ++i)
                {
                    a.data()[i] = 2.0 * rng.next_f64() - 1.0;
                }
                for (crd::u64 i = 0; i < bsz * n * n; ++i)
                {
                    b.data()[i] = 2.0 * rng.next_f64() - 1.0;
                }
            }
            const double ours = best_of(5, [&] {
                (void)batched_gemm<crd::f64>(1.0, TensorView<const crd::f64>(a.view()),
                                             TensorView<const crd::f64>(b.view()), 0.0, c.view(), &alloc, 1U);
            });
            std::printf("[n=%2llu b=%6llu] ours_serial=%9.2fms",
                        static_cast<unsigned long long>(n), static_cast<unsigned long long>(bsz), ours);
#ifdef CRD_BENCH_MKL
            const double mkl = best_of(5, [&] {
                cblas_dgemm_batch_strided(CblasRowMajor, CblasNoTrans, CblasNoTrans, static_cast<MKL_INT>(n),
                                          static_cast<MKL_INT>(n), static_cast<MKL_INT>(n), 1.0, a.data(),
                                          static_cast<MKL_INT>(n), static_cast<MKL_INT>(n * n), b.data(),
                                          static_cast<MKL_INT>(n), static_cast<MKL_INT>(n * n), 0.0, c.data(),
                                          static_cast<MKL_INT>(n), static_cast<MKL_INT>(n * n),
                                          static_cast<MKL_INT>(bsz));
            });
            std::printf("  mkl_batch_strided=%9.2fms  ratio=%5.2fx", mkl, mkl / ours);
#endif
            std::printf("\n");
            std::fflush(stdout);
            // ---- batched Cholesky (SPD inputs rebuilt per rep: factor is in-place) ----
            Tensor<crd::f64> spd(&alloc, {shp, 3});
            {
                // spd = a a^T + n I per matrix (reuse `a`)
                for (crd::u64 i = 0; i < bsz; ++i)
                {
                    const crd::f64* m = a.data() + i * n * n;
                    crd::f64* d = spd.data() + i * n * n;
                    for (crd::u64 r = 0; r < n; ++r)
                    {
                        for (crd::u64 cc = 0; cc < n; ++cc)
                        {
                            crd::f64 s = r == cc ? static_cast<crd::f64>(n) : 0.0;
                            for (crd::u64 p = 0; p < n; ++p)
                            {
                                s += m[r * n + p] * m[cc * n + p];
                            }
                            d[r * n + cc] = s;
                        }
                    }
                }
            }
            Tensor<crd::f64> work(&alloc, {shp, 3});
            crd::containers::Array<crd::i32> info(&alloc);
            info.resize(bsz);
            const crd::u64 total = bsz * n * n;
            const double ours_chol = best_of(5, [&] {
                for (crd::u64 e = 0; e < total; ++e)
                {
                    work.data()[e] = spd.data()[e];
                }
                (void)crd::hesap::tensor::batched_cholesky_factor<crd::f64>(
                    work.view(), {info.data(), static_cast<crd::usize>(bsz)}, 1U);
            });
            double copy_cost = best_of(5, [&] {
                for (crd::u64 e = 0; e < total; ++e)
                {
                    work.data()[e] = spd.data()[e];
                }
            });
            std::printf("[n=%2llu b=%6llu] ours_chol=%9.2fms (copy %6.2fms)",
                        static_cast<unsigned long long>(n), static_cast<unsigned long long>(bsz),
                        ours_chol - copy_cost, copy_cost);
#ifdef CRD_BENCH_MKL
            const double mkl_chol = best_of(5, [&] {
                for (crd::u64 e = 0; e < total; ++e)
                {
                    work.data()[e] = spd.data()[e];
                }
                // this MKL predates ?potrf_batch_strided — the honest row is
                // the LAPACKE loop (what MKL consumers without the batch API,
                // and torch internally, actually run)
                for (crd::u64 i = 0; i < bsz; ++i)
                {
                    (void)LAPACKE_dpotrf(LAPACK_ROW_MAJOR, 'L', static_cast<MKL_INT>(n),
                                         work.data() + i * n * n, static_cast<MKL_INT>(n));
                }
            });
            std::printf("  mkl_potrf_loop=%9.2fms  ratio=%5.2fx", mkl_chol - copy_cost,
                        (mkl_chol - copy_cost) / (ours_chol - copy_cost));
#endif
            std::printf("\n");
            std::fflush(stdout);
            // ---- batched LU (general matrices: reuse `a`) ----
            crd::containers::Array<crd::i32> piv(&alloc);
            piv.resize(bsz * n);
            const double ours_lu = best_of(5, [&] {
                for (crd::u64 e = 0; e < total; ++e)
                {
                    work.data()[e] = a.data()[e];
                }
                (void)crd::hesap::tensor::batched_lu_factor<crd::f64>(
                    work.view(), {piv.data(), static_cast<crd::usize>(bsz * n)},
                    {info.data(), static_cast<crd::usize>(bsz)}, 1U);
            });
            std::printf("[n=%2llu b=%6llu] ours_lu  =%9.2fms (copy %6.2fms)",
                        static_cast<unsigned long long>(n), static_cast<unsigned long long>(bsz),
                        ours_lu - copy_cost, copy_cost);
#ifdef CRD_BENCH_MKL
            crd::containers::Array<MKL_INT> mpiv(&alloc);
            mpiv.resize(n);
            const double mkl_lu = best_of(5, [&] {
                for (crd::u64 e = 0; e < total; ++e)
                {
                    work.data()[e] = a.data()[e];
                }
                for (crd::u64 i = 0; i < bsz; ++i)
                {
                    (void)LAPACKE_dgetrf(LAPACK_ROW_MAJOR, static_cast<MKL_INT>(n), static_cast<MKL_INT>(n),
                                         work.data() + i * n * n, static_cast<MKL_INT>(n), mpiv.data());
                }
            });
            std::printf("  mkl_getrf_loop=%9.2fms  ratio=%5.2fx", mkl_lu - copy_cost,
                        (mkl_lu - copy_cost) / (ours_lu - copy_cost));
#endif
            std::printf("\n");
            std::fflush(stdout);
            // ---- batched small-SVD ----
            Tensor<crd::f64> u(&alloc, {shp, 3});
            Tensor<crd::f64> v(&alloc, {shp, 3});
            crd::containers::Array<crd::f64> sig(&alloc);
            sig.resize(bsz * n);
            const double ours_svd = best_of(5, [&] {
                (void)crd::hesap::tensor::batched_svd_small<crd::f64>(
                    TensorView<const crd::f64>(a.view()), u.view(),
                    {sig.data(), static_cast<crd::usize>(bsz * n)}, v.view(),
                    {info.data(), static_cast<crd::usize>(bsz)}, 30U, 1U);
            });
            std::printf("[n=%2llu b=%6llu] ours_svd =%9.2fms", static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(bsz), ours_svd);
#ifdef CRD_BENCH_MKL
            crd::containers::Array<crd::f64> mwork(&alloc);
            mwork.resize(n * n + 2U * n);
            const double mkl_svd = best_of(5, [&] {
                for (crd::u64 i = 0; i < bsz; ++i)
                {
                    for (crd::u64 e = 0; e < n * n; ++e)
                    {
                        work.data()[i * n * n + e] = a.data()[i * n * n + e];
                    }
                }
                for (crd::u64 i = 0; i < bsz; ++i)
                {
                    (void)LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'A', static_cast<MKL_INT>(n),
                                         static_cast<MKL_INT>(n), work.data() + i * n * n,
                                         static_cast<MKL_INT>(n), sig.data() + i * n, u.data() + i * n * n,
                                         static_cast<MKL_INT>(n), v.data() + i * n * n,
                                         static_cast<MKL_INT>(n));
                }
            });
            std::printf("  mkl_gesdd_loop=%9.2fms  ratio=%5.2fx", mkl_svd, mkl_svd / ours_svd);
#endif
            std::printf("\n");
            std::fflush(stdout);
        }
    }
    return 0;
}
