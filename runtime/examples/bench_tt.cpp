// v14-k TT wall-clock: OUR tt_svd / tt_add+tt_round / tt_cross / tt_eval vs
// tntorch (scripts/v14k_tt_peers.py runs the matched problems; ttpy is
// N/A-with-check — numpy.distutils removed on py3.12). Plus the flagship
// v13-interp-style LUT demo: a 6D two-body gravitational kernel TT-crossed
// from FUNCTION EVALUATIONS (never materialized during the build), then
// compared against direct multilinear interpolation of the MATERIALIZED
// 16^6 grid (134 MB) in the eval hot path.
// Boards -> docs/bench/2026-07-05-v14k-tt.md. Pinned harness:
// build/crd_tt_bench.sh (taskset -c 4, 1T, best-of-5).
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/tt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

using crd::hesap::stats::PhiloxRng;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::tt_add;
using crd::hesap::tensor::tt_contract;
using crd::hesap::tensor::tt_cross;
using crd::hesap::tensor::tt_eval;
using crd::hesap::tensor::tt_eval_lerp;
using crd::hesap::tensor::tt_eval_many;
using crd::hesap::tensor::tt_eval_workspace;
using crd::hesap::tensor::tt_round;
using crd::hesap::tensor::tt_svd;
using crd::hesap::tensor::TtCrossInfo;
using crd::hesap::tensor::TtCrossParams;
using crd::hesap::tensor::TtTensor;

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

// matched problems (identical formulas in scripts/v14k_tt_peers.py)
void fill_hilbert4(crd::f64* p, crd::u64 n)
{
    for (crd::u64 i = 0; i < n; ++i)
    {
        for (crd::u64 j = 0; j < n; ++j)
        {
            for (crd::u64 k = 0; k < n; ++k)
            {
                for (crd::u64 l = 0; l < n; ++l)
                {
                    p[((i * n + j) * n + k) * n + l] = 1.0 / (1.0 + static_cast<crd::f64>(i + j + k + l));
                }
            }
        }
    }
}

crd::f64 axis(crd::u64 i, crd::u64 n)
{
    return -1.0 + 2.0 * static_cast<crd::f64>(i) / static_cast<crd::f64>(n - 1U);
}

crd::f64 smooth4(crd::containers::ConstSpan<crd::u64> idx, crd::u64 n)
{
    crd::f64 s = 0.1;
    for (crd::u64 k = 0; k < idx.size(); ++k)
    {
        const crd::f64 x = axis(idx[k], n);
        s += x * x;
    }
    return 1.0 / std::sqrt(s);
}

// the 6D LUT kernel: SEPARATED two-body gravitational potential — the
// ephemeris regime (bodies never collide; no softening): r1 in [-1,-0.2]^3,
// r2 in [0.2,1]^3, f = 1/|r1 - r2|. (The overlapping-domain softened variant
// is NOT TT-compressible at rank 12 — tntorch fails it identically, val_err
// 4.4e-1 vs our 4.3e-1, same budget — recorded on the board.)
crd::f64 sep_axis1(crd::u64 i)
{
    return -1.0 + 0.8 * static_cast<crd::f64>(i) / 15.0;
}

crd::f64 sep_axis2(crd::u64 i)
{
    return 0.2 + 0.8 * static_cast<crd::f64>(i) / 15.0;
}

crd::f64 grav6(crd::containers::ConstSpan<crd::u64> idx)
{
    crd::f64 s = 0.0;
    for (crd::u64 k = 0; k < 3U; ++k)
    {
        const crd::f64 d = sep_axis1(idx[k]) - sep_axis2(idx[k + 3U]);
        s += d * d;
    }
    return 1.0 / std::sqrt(s);
}

void print_ranks(const char* tag, const TtTensor<crd::f64>& tt)
{
    std::printf("%s ranks=[", tag);
    for (crd::u32 k = 1; k < tt.dims(); ++k)
    {
        std::printf("%llu%s", static_cast<unsigned long long>(tt.rank(k)), k + 1U < tt.dims() ? "," : "");
    }
    std::printf("] params=%llu\n", static_cast<unsigned long long>(tt.param_count()));
}

} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1ULL << 30); // 1 GB bench arena
    std::printf("== v14-k TT bench (ours, 1T, best-of-5) ==\n");

    // ---- tt_svd: hilbert 20^4 @1e-8, smooth 16^4 @1e-10, smooth 32^4 @1e-8 --
    {
        const crd::u64 n = 20;
        const crd::u64 shp[4] = {n, n, n, n};
        Tensor<crd::f64> dense(&alloc, {shp, 4});
        fill_hilbert4(dense.data(), n);
        TtTensor<crd::f64> tt(&alloc);
        const double ms = best_of(5, [&] {
            TtTensor<crd::f64> t(&alloc);
            (void)tt_svd<crd::f64>(&alloc, dense.view(), 1e-8, 0U, t);
            tt = static_cast<TtTensor<crd::f64>&&>(t);
        });
        std::printf("[tt_svd hilbert 20^4 eps=1e-8]   %8.2f ms  ", ms);
        print_ranks("", tt);
    }
    for (const crd::u64 n : {16ULL, 32ULL})
    {
        const crd::f64 eps = n == 16ULL ? 1e-10 : 1e-8;
        const crd::u64 shp[4] = {n, n, n, n};
        Tensor<crd::f64> dense(&alloc, {shp, 4});
        {
            crd::u64 ix[4];
            for (ix[0] = 0; ix[0] < n; ++ix[0])
            {
                for (ix[1] = 0; ix[1] < n; ++ix[1])
                {
                    for (ix[2] = 0; ix[2] < n; ++ix[2])
                    {
                        for (ix[3] = 0; ix[3] < n; ++ix[3])
                        {
                            dense.data()[((ix[0] * n + ix[1]) * n + ix[2]) * n + ix[3]] =
                                smooth4({ix, 4}, n);
                        }
                    }
                }
            }
        }
        TtTensor<crd::f64> tt(&alloc);
        const double ms = best_of(5, [&] {
            TtTensor<crd::f64> t(&alloc);
            (void)tt_svd<crd::f64>(&alloc, dense.view(), eps, 0U, t);
            tt = static_cast<TtTensor<crd::f64>&&>(t);
        });
        std::printf("[tt_svd smooth %2llu^4 eps=%g] %8.2f ms  ", static_cast<unsigned long long>(n), eps, ms);
        print_ranks("", tt);

        if (n == 16ULL)
        {
            // ---- tt_add + tt_round (matched with tntorch (t+t).round_tt) ----
            const double rms = best_of(5, [&] {
                TtTensor<crd::f64> sum(&alloc);
                (void)tt_add<crd::f64>(&alloc, tt, tt, sum);
                (void)tt_round<crd::f64>(&alloc, sum, 1e-10, 0U);
            });
            std::printf("[tt_add+tt_round smooth 16^4 @1e-10] %8.2f ms\n", rms);

            // ---- eval throughput: 1M random indices on the 16^4 TT ----------
            const crd::u64 npts = 1000000;
            crd::containers::Array<crd::u64> idx(&alloc);
            idx.resize(npts * 4U);
            crd::containers::Array<crd::f64> out(&alloc);
            out.resize(npts);
            {
                PhiloxRng rng(2026U, 7U);
                for (crd::u64 i = 0; i < npts * 4U; ++i)
                {
                    idx.data()[i] = rng.next_below(n);
                }
            }
            crd::f64 work[64];
            const double ems = best_of(5, [&] {
                (void)tt_eval_many<crd::f64>(tt, {idx.data(), npts * 4U}, {out.data(), npts}, {work, 64});
            });
            std::printf("[tt_eval_many smooth 16^4, 1M pts] %8.2f ms  (%.1f ns/pt)\n", ems,
                        ems * 1e6 / static_cast<double>(npts));
        }
    }

    // ---- tt_cross smooth 4D (the frozen oracle budget) ----------------------
    {
        const crd::u64 n = 16;
        const crd::u64 shp[4] = {n, n, n, n};
        TtCrossParams<crd::f64> params;
        params.max_rank = 10;
        params.max_sweeps = 6;
        params.tol = 1e-9;
        params.val_size = 200;
        params.seed = 42;
        const auto f = [n](crd::containers::ConstSpan<crd::u64> ix) noexcept { return smooth4(ix, n); };
        TtCrossInfo<crd::f64> info;
        TtTensor<crd::f64> tt(&alloc);
        const double ms = best_of(5, [&] {
            TtTensor<crd::f64> t(&alloc);
            (void)tt_cross<crd::f64>(&alloc, {shp, 4}, f, params, t, &info);
            tt = static_cast<TtTensor<crd::f64>&&>(t);
        });
        std::printf("[tt_cross smooth 16^4 r=10] %8.2f ms  val_err=%.3e evals=%llu sweeps=%u\n", ms,
                    info.val_error, static_cast<unsigned long long>(info.evals), info.sweeps);
    }

    // ---- the 6D LUT demo ----------------------------------------------------
    {
        const crd::u64 n = 16;
        const crd::u64 d = 6;
        const crd::u64 shp[6] = {n, n, n, n, n, n};
        const crd::u64 dense_elems = 16777216; // 16^6
        const auto f = [](crd::containers::ConstSpan<crd::u64> ix) noexcept { return grav6(ix); };
        TtCrossParams<crd::f64> params;
        params.max_rank = 12;
        params.max_sweeps = 6;
        params.tol = 1e-3; // the LUT regime: kernel error below the 16-pt grid's
                           // own multilinear interpolation error (~4e-3)
        params.val_size = 500;
        params.seed = 42;
        TtCrossInfo<crd::f64> info;
        TtTensor<crd::f64> tt(&alloc);
        const double cms = best_of(3, [&] {
            TtTensor<crd::f64> t(&alloc);
            (void)tt_cross<crd::f64>(&alloc, {shp, 6}, f, params, t, &info);
            tt = static_cast<TtTensor<crd::f64>&&>(t);
        });
        std::printf("[LUT6 tt_cross 16^6 r=12] %8.2f ms  val_err=%.3e evals=%llu sweeps=%u\n", cms,
                    info.val_error, static_cast<unsigned long long>(info.evals), info.sweeps);
        std::printf("[LUT6 compression] dense=%llu params=%llu ratio=%.0fx (%.1f KB vs %.1f MB)\n",
                    static_cast<unsigned long long>(dense_elems),
                    static_cast<unsigned long long>(tt.param_count()),
                    static_cast<double>(dense_elems) / static_cast<double>(tt.param_count()),
                    static_cast<double>(tt.param_count()) * 8.0 / 1024.0,
                    static_cast<double>(dense_elems) * 8.0 / 1024.0 / 1024.0);
        // accuracy vs truth on 100k random grid indices
        {
            PhiloxRng rng(5150U, 0U);
            crd::f64 work[64];
            crd::f64 worst = 0.0;
            for (crd::u64 p = 0; p < 100000U; ++p)
            {
                crd::u64 ix[6];
                for (crd::u64 k = 0; k < d; ++k)
                {
                    ix[k] = rng.next_below(n);
                }
                const crd::f64 v = tt_eval<crd::f64>(tt, {ix, 6}, {work, 64});
                const crd::f64 t = grav6({ix, 6});
                const crd::f64 e = std::abs(v - t) / std::abs(t);
                if (e > worst)
                {
                    worst = e;
                }
            }
            std::printf("[LUT6 accuracy] max rel err vs truth over 100k grid pts: %.3e\n", worst);
        }
        // materialize the dense grid (the direct-LUT peer)
        Tensor<crd::f64> dense(&alloc, {shp, 6});
        {
            crd::u64 ix[6] = {};
            for (crd::u64 flat = 0; flat < dense_elems; ++flat)
            {
                dense.data()[flat] = grav6({ix, 6});
                for (crd::u64 k = d; k-- > 0U;)
                {
                    if (++ix[k] < n)
                    {
                        break;
                    }
                    ix[k] = 0;
                }
            }
        }
        // eval hot path: 1M continuous points, TT-lerp vs direct 64-corner
        // multilinear interpolation of the materialized grid; plus 1M raw
        // grid-index evals both sides (the dense gather trivially wins that
        // row — stated on the board; the CONTINUOUS interp is the v13 regime).
        const crd::u64 npts = 1000000;
        crd::containers::Array<crd::u64> i0s(&alloc);
        crd::containers::Array<crd::f64> ws(&alloc);
        crd::containers::Array<crd::f64> outs(&alloc);
        i0s.resize(npts * d);
        ws.resize(npts * d);
        outs.resize(npts);
        {
            PhiloxRng rng(99U, 1U);
            for (crd::u64 i = 0; i < npts * d; ++i)
            {
                i0s.data()[i] = rng.next_below(n - 1U);
                ws.data()[i] = rng.next_f64();
            }
        }
        crd::f64 work[64];
        volatile crd::f64 sink = 0.0;
        const double lerp_ms = best_of(5, [&] {
            crd::f64 acc = 0.0;
            for (crd::u64 p = 0; p < npts; ++p)
            {
                acc += tt_eval_lerp<crd::f64>(tt, {i0s.data() + p * d, 6}, {ws.data() + p * d, 6},
                                              {work, 64});
            }
            sink = acc;
        });
        const crd::u64 st5 = 1;
        const crd::u64 st4 = n;
        const crd::u64 st3 = n * n;
        const crd::u64 st2 = n * n * n;
        const crd::u64 st1 = n * n * n * n;
        const crd::u64 st0 = n * n * n * n * n;
        const crd::u64 strides[6] = {st0, st1, st2, st3, st4, st5};
        const double dint_ms = best_of(5, [&] {
            crd::f64 acc = 0.0;
            const crd::f64* g = dense.data();
            for (crd::u64 p = 0; p < npts; ++p)
            {
                const crd::u64* i0 = i0s.data() + p * d;
                const crd::f64* w = ws.data() + p * d;
                crd::u64 base = 0;
                for (crd::u64 k = 0; k < d; ++k)
                {
                    base += i0[k] * strides[k];
                }
                crd::f64 v = 0.0;
                for (crd::u32 c = 0; c < 64U; ++c) // 2^6 corners
                {
                    crd::f64 wt = 1.0;
                    crd::u64 off = base;
                    for (crd::u64 k = 0; k < d; ++k)
                    {
                        if (((c >> k) & 1U) != 0U)
                        {
                            wt *= w[k];
                            off += strides[k];
                        }
                        else
                        {
                            wt *= 1.0 - w[k];
                        }
                    }
                    v += wt * g[off];
                }
                acc += v;
            }
            sink = acc;
        });
        std::printf("[LUT6 eval 1M continuous pts] tt_eval_lerp %8.2f ms (%.1f ns/pt)  "
                    "dense 64-corner interp %8.2f ms (%.1f ns/pt)  ratio %.2fx\n",
                    lerp_ms, lerp_ms * 1e6 / static_cast<double>(npts), dint_ms,
                    dint_ms * 1e6 / static_cast<double>(npts), dint_ms / lerp_ms);
        // raw grid-index eval, both sides (honesty row)
        crd::containers::Array<crd::u64> gidx(&alloc);
        gidx.resize(npts * d);
        {
            PhiloxRng rng(99U, 2U);
            for (crd::u64 i = 0; i < npts * d; ++i)
            {
                gidx.data()[i] = rng.next_below(n);
            }
        }
        const double gev_ms = best_of(5, [&] {
            (void)tt_eval_many<crd::f64>(tt, {gidx.data(), npts * d}, {outs.data(), npts}, {work, 64});
        });
        const double ggather_ms = best_of(5, [&] {
            crd::f64 acc = 0.0;
            const crd::f64* g = dense.data();
            for (crd::u64 p = 0; p < npts; ++p)
            {
                const crd::u64* ix = gidx.data() + p * d;
                crd::u64 off = 0;
                for (crd::u64 k = 0; k < d; ++k)
                {
                    off += ix[k] * strides[k];
                }
                acc += g[off];
            }
            sink = acc;
        });
        std::printf("[LUT6 eval 1M grid indices] tt_eval_many %8.2f ms (%.1f ns/pt)  "
                    "dense gather %8.2f ms (%.1f ns/pt)\n",
                    gev_ms, gev_ms * 1e6 / static_cast<double>(npts), ggather_ms,
                    ggather_ms * 1e6 / static_cast<double>(npts));
        (void)sink;
    }
    return 0;
}
