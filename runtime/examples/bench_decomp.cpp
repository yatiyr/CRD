// v14-j decomposition wall-clock A/B: OUR cp_als / hooi / hooi_rand vs TensorLy
// 0.9.0 (numpy backend, matched 1-thread, same synthetic tensors — the input is
// an ELEMENTWISE exact formula so both languages construct bit-identical f64
// data). Peer rows: scripts/v14j_decomp_bench.py; boards ->
// docs/bench/2026-07-05-v14j-decomp.md. Harness build/crd_decomp_bench.sh
// (taskset -c 4, best-of-5).
#include <crd/hesap/tensor/decomp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using crd::hesap::tensor::cp_als;
using crd::hesap::tensor::CpInfo;
using crd::hesap::tensor::CpInit;
using crd::hesap::tensor::CpOptions;
using crd::hesap::tensor::DecompStatus;
using crd::hesap::tensor::hooi;
using crd::hesap::tensor::hooi_rand;
using crd::hesap::tensor::RandOptions;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorView;
using crd::hesap::tensor::TuckerInfo;
using crd::hesap::tensor::TuckerOptions;

namespace
{

double now_ms()
{
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count()) *
           1e-6;
}

// Cross-language-exact synthetic tensor: modular pseudo-noise + a decaying
// structured term, both pure elementwise f64 expressions (one add rounding).
void fill_bench(Tensor<crd::f64>& t, const crd::u64* shp, crd::u32 nd)
{
    static const crd::u64 mul[4] = {31U, 17U, 7U, 3U};
    crd::u64 idx[4] = {};
    crd::f64* p = t.data();
    const crd::u64 n = t.size();
    for (crd::u64 e = 0; e < n; ++e)
    {
        crd::u64 m = 0;
        crd::u64 s = 0;
        for (crd::u32 d = 0; d < nd; ++d)
        {
            m += idx[d] * mul[d];
            s += idx[d];
        }
        p[e] = static_cast<crd::f64>(m % 101U) / 50.5 - 1.0 + 2.0 / (1.0 + static_cast<crd::f64>(s));
        for (crd::u32 d = nd; d-- > 0U;)
        {
            if (++idx[d] < shp[d])
            {
                break;
            }
            idx[d] = 0;
        }
    }
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

void run_case(const char* tag, const crd::u64* shp, crd::u32 nd, crd::u64 cp_rank, crd::u64 tucker_rank)
{
    crd::memory::TlsfAllocator alloc(3ULL << 30); // 3 GB: 128^3 svd internals peak in the hundreds of MB
    Tensor<crd::f64> xt(&alloc, {shp, nd});
    fill_bench(xt, shp, nd);
    const TensorView<const crd::f64> x(xt.view());
    // ---- CP-ALS, svd init, 10 fixed sweeps ---------------------------------
    {
        Tensor<crd::f64> f[8] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                 Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                 Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        crd::containers::Array<crd::f64> w(&alloc);
        w.resize(cp_rank);
        CpOptions<crd::f64> opts;
        opts.max_iters = 10U;
        opts.tol = 0.0;
        opts.init = CpInit::Svd;
        CpInfo<crd::f64> info;
        const double ms = best_of(5, [&] {
            const DecompStatus st = cp_als<crd::f64>(x, cp_rank, {f, nd}, {w.data(), cp_rank}, info, &alloc, opts);
            if (st != DecompStatus::Ok)
            {
                std::printf("cp_als FAILED: %s\n", to_string(st));
            }
        });
        std::printf("%s cp_als      rank %llu iters 10 : %10.2f ms  fit %.12f\n", tag,
                    static_cast<unsigned long long>(cp_rank), ms, static_cast<double>(info.fit));
    }
    // ---- Tucker HOOI, exact svd, svd init, 5 fixed sweeps ------------------
    crd::u64 ranks[8];
    for (crd::u32 d = 0; d < nd; ++d)
    {
        ranks[d] = tucker_rank;
    }
    {
        Tensor<crd::f64> f[8] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                 Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                 Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> core(&alloc);
        TuckerOptions<crd::f64> topts;
        topts.max_iters = 5U;
        topts.tol = 0.0;
        TuckerInfo<crd::f64> info;
        const double ms = best_of(5, [&] {
            const DecompStatus st = hooi<crd::f64>(x, {ranks, nd}, {f, nd}, core, info, &alloc, topts);
            if (st != DecompStatus::Ok)
            {
                std::printf("hooi FAILED: %s\n", to_string(st));
            }
        });
        std::printf("%s hooi        rank %llu iters 5  : %10.2f ms  fit %.12f\n", tag,
                    static_cast<unsigned long long>(tucker_rank), ms, static_cast<double>(info.fit));
    }
    // ---- randomized Tucker (Philox rSVD), 1 sweep. power_iters = 4: in the
    // Gram-operator form an extra power step is a rows x rows product (near
    // free), where the classic form (and tensorly, q=2 default) pays two full
    // passes over A per step — the structural asymmetry the board shows.
    {
        Tensor<crd::f64> f[8] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                 Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                 Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> core(&alloc);
        TuckerOptions<crd::f64> topts;
        topts.max_iters = 1U;
        topts.tol = 0.0;
        RandOptions ropts;
        ropts.seed = 42U;
        ropts.power_iters = 4U;
        TuckerInfo<crd::f64> info;
        const double ms = best_of(5, [&] {
            const DecompStatus st = hooi_rand<crd::f64>(x, {ranks, nd}, {f, nd}, core, info, &alloc, ropts, topts);
            if (st != DecompStatus::Ok)
            {
                std::printf("hooi_rand FAILED: %s\n", to_string(st));
            }
        });
        std::printf("%s hooi_rand   rank %llu iters 1  : %10.2f ms  fit %.12f\n", tag,
                    static_cast<unsigned long long>(tucker_rank), ms, static_cast<double>(info.fit));
    }
}

} // namespace

int main()
{
    {
        const crd::u64 shp[3] = {64U, 64U, 64U};
        run_case("64x64x64   ", shp, 3U, 16U, 16U);
    }
    {
        const crd::u64 shp[4] = {32U, 32U, 32U, 32U};
        run_case("32^4       ", shp, 4U, 8U, 8U);
    }
    {
        const crd::u64 shp[3] = {128U, 128U, 128U};
        run_case("128x128x128", shp, 3U, 32U, 32U);
    }
    return 0;
}
