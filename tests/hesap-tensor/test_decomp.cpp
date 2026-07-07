// v14-j tensor-decomposition gates. Oracle rows are FROZEN tensorly 0.9.0 /
// verified-numpy results (scripts/v14j_decomp_oracle.py -> ref_decomp.inc);
// factor gates ride fit + reconstruction error + subspace angles (factors only
// match up to sign/permutation, never raw bits). Determinism gates: run-twice
// and the {1,2,4,8,16} worker moat, bit-identical; the randomized variants are
// deterministic-randomized (Philox keyed draws) and gated the same way.
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/decomp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>

#include "ref_decomp.inc"

using crd::hesap::tensor::cp_als;
using crd::hesap::tensor::cp_als_generic;
using crd::hesap::tensor::cp_reconstruct;
using crd::hesap::tensor::CpInfo;
using crd::hesap::tensor::CpInit;
using crd::hesap::tensor::CpOptions;
using crd::hesap::tensor::DecompStatus;
using crd::hesap::tensor::DenseMttkrp;
using crd::hesap::tensor::hooi;
using crd::hesap::tensor::hooi_rand;
using crd::hesap::tensor::hosvd;
using crd::hesap::tensor::hosvd_rand;
using crd::hesap::tensor::RandOptions;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorView;
using crd::hesap::tensor::tucker_reconstruct;
using crd::hesap::tensor::TuckerInfo;
using crd::hesap::tensor::TuckerOptions;

namespace
{

[[nodiscard]] TensorView<const crd::f64> ref_tensor(crd::u32 idx)
{
    return TensorView<const crd::f64>::contiguous(refdecomp::kTensorData + refdecomp::kTensorOffset[idx],
                                                  {refdecomp::kTensorShape[idx], refdecomp::kTensorNdim[idx]});
}

[[nodiscard]] crd::f64 rel_err(TensorView<const crd::f64> x, const Tensor<crd::f64>& xhat)
{
    crd::f64 num = 0.0;
    crd::f64 den = 0.0;
    const crd::f64* h = xhat.data();
    const crd::f64* d = x.data(); // ref tensors are contiguous row-major
    const crd::u64 n = x.size();
    for (crd::u64 i = 0; i < n; ++i)
    {
        num += (d[i] - h[i]) * (d[i] - h[i]);
        den += d[i] * d[i];
    }
    return std::sqrt(num) / std::sqrt(den > 0.0 ? den : 1.0);
}

[[nodiscard]] crd::f64 max_abs(const crd::f64* p, crd::u64 n)
{
    crd::f64 m = 0.0;
    for (crd::u64 i = 0; i < n; ++i)
    {
        const crd::f64 a = p[i] < 0.0 ? -p[i] : p[i];
        if (a > m)
        {
            m = a;
        }
    }
    return m;
}

} // namespace

TEST_CASE("decomp: CP-ALS svd-init fit gates - every frozen tensorly row", "[v14j][decomp][cp]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    for (crd::u32 row = 0; row < refdecomp::kNumCpRows; ++row)
    {
        const crd::u32 ti = refdecomp::kCpTensor[row];
        const crd::u64 rank = refdecomp::kCpRank[row];
        const TensorView<const crd::f64> x = ref_tensor(ti);
        const crd::u32 nd = x.rank();
        Tensor<crd::f64> factors[8] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                       Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                       Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        crd::containers::Array<crd::f64> weights(&alloc);
        weights.resize(rank);
        CpOptions<crd::f64> opts;
        opts.max_iters = refdecomp::kCpIters[row];
        opts.tol = 0.0; // fixed budget - the oracle ran exactly this many sweeps
        opts.init = CpInit::Svd;
        CpInfo<crd::f64> info;
        REQUIRE(cp_als<crd::f64>(x, rank, {factors, nd}, {weights.data(), rank}, info, &alloc, opts) ==
                DecompStatus::Ok);
        REQUIRE(info.iters == refdecomp::kCpIters[row]);
        INFO("row " << row << " tensor " << ti << " rank " << rank << " fit " << info.fit << " tl "
                    << refdecomp::kCpFitTl[row]);
        REQUIRE(info.fit >= refdecomp::kCpFitTl[row] - 1e-6);
        REQUIRE(info.rec_error <= refdecomp::kCpRecErrTl[row] + 1e-6);
        // identity error vs true reconstruction: consistent (and gates cp_reconstruct)
        Tensor<crd::f64> xhat(&alloc);
        REQUIRE(cp_reconstruct<crd::f64>({weights.data(), rank}, {factors, nd}, xhat, &alloc) == DecompStatus::Ok);
        const crd::f64 direct = rel_err(x, xhat);
        REQUIRE(direct <= refdecomp::kCpRecErrTl[row] + 1e-6);
        const crd::f64 gap = direct > info.rec_error ? direct - info.rec_error : info.rec_error - direct;
        REQUIRE(gap <= 1e-7 + 1e-4 * direct);
    }
}

TEST_CASE("decomp: Tucker HOSVD and HOOI gates - true reconstruction vs the frozen board", "[v14j][decomp][tucker]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    for (crd::u32 row = 0; row < refdecomp::kNumTuckerRows; ++row)
    {
        const crd::u32 ti = refdecomp::kTuckerTensor[row];
        const TensorView<const crd::f64> x = ref_tensor(ti);
        const crd::u32 nd = x.rank();
        Tensor<crd::f64> factors[8] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                       Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                       Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> core(&alloc);
        TuckerInfo<crd::f64> hinfo;
        REQUIRE(hosvd<crd::f64>(x, {refdecomp::kTuckerRanks[row], nd}, {factors, nd}, core, &alloc, &hinfo) ==
                DecompStatus::Ok);
        Tensor<crd::f64> xhat(&alloc);
        REQUIRE(tucker_reconstruct<crd::f64>(core, {factors, nd}, xhat, &alloc) == DecompStatus::Ok);
        const crd::f64 err_hosvd = rel_err(x, xhat);
        INFO("row " << row << " tensor " << ti << " hosvd " << err_hosvd << " ref "
                    << refdecomp::kTuckerRecErrHosvd[row]);
        REQUIRE(err_hosvd <= refdecomp::kTuckerRecErrHosvd[row] + 1e-6);
        if (refdecomp::kTuckerRecErrHosvd[row] < 1e-10) // exact-recovery rows: machine-precision gate
        {
            REQUIRE(err_hosvd < 1e-12);
        }
        // HOOI at the frozen iteration budget
        TuckerOptions<crd::f64> topts;
        topts.max_iters = refdecomp::kTuckerIters[row];
        topts.tol = 0.0;
        TuckerInfo<crd::f64> info;
        REQUIRE(hooi<crd::f64>(x, {refdecomp::kTuckerRanks[row], nd}, {factors, nd}, core, info, &alloc, topts) ==
                DecompStatus::Ok);
        REQUIRE(tucker_reconstruct<crd::f64>(core, {factors, nd}, xhat, &alloc) == DecompStatus::Ok);
        const crd::f64 err_hooi = rel_err(x, xhat);
        INFO("row " << row << " hooi " << err_hooi << " tl " << refdecomp::kTuckerRecErrHooiTl[row]);
        REQUIRE(err_hooi <= refdecomp::kTuckerRecErrHooiTl[row] + 1e-6);
        if (refdecomp::kTuckerRecErrHooiTl[row] < 1e-10)
        {
            REQUIRE(err_hooi < 1e-12);
        }
    }
}

TEST_CASE("decomp: lrnoise Tucker factor subspaces match the generating factors", "[v14j][decomp][tucker]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const TensorView<const crd::f64> x = ref_tensor(1U); // lrnoise, CP rank 6 + 1e-3 noise
    const crd::u64 ranks[3] = {6U, 6U, 6U};
    Tensor<crd::f64> factors[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    Tensor<crd::f64> core(&alloc);
    TuckerInfo<crd::f64> info;
    TuckerOptions<crd::f64> topts;
    topts.max_iters = 20U;
    topts.tol = 0.0;
    REQUIRE(hooi<crd::f64>(x, {ranks, 3U}, {factors, 3U}, core, info, &alloc, topts) == DecompStatus::Ok);
    const double* qs[3] = {refdecomp::kLrQ0, refdecomp::kLrQ1, refdecomp::kLrQ2};
    const crd::u64 qrows[3] = {refdecomp::kLrQ0Rows, refdecomp::kLrQ1Rows, refdecomp::kLrQ2Rows};
    for (crd::u32 n = 0; n < 3U; ++n)
    {
        // M = Q^T U (6x6); sigma_min(M) = cos(largest principal angle between
        // the recovered factor subspace and the generating one.
        const crd::u64 r = 6U;
        crd::f64 m[36];
        crd::f64 v[36];
        crd::f64 sig[6];
        const crd::f64* u = factors[n].data();
        for (crd::u64 a = 0; a < r; ++a)
        {
            for (crd::u64 b = 0; b < r; ++b)
            {
                crd::f64 s = 0.0;
                for (crd::u64 i = 0; i < qrows[n]; ++i)
                {
                    s += qs[n][i * r + a] * u[i * r + b];
                }
                m[a * r + b] = s;
            }
        }
        crd::i32 jinfo = -1;
        crd::hesap::tensor::batcheddetail::svd_scalar_sweeps(m, v, r, 60U, &jinfo);
        crd::hesap::tensor::batcheddetail::svd_finalize_one(m, v, sig, r);
        INFO("mode " << n << " sigma_min " << sig[5]);
        REQUIRE(sig[5] >= 0.99);
    }
}

TEST_CASE("decomp: randomized HOSVD error bounds vs exact and the frozen reference", "[v14j][decomp][rand]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    for (crd::u32 row = 0; row < refdecomp::kNumRandRows; ++row)
    {
        const crd::u32 ti = refdecomp::kRandTensor[row];
        const TensorView<const crd::f64> x = ref_tensor(ti);
        const crd::u32 nd = x.rank();
        Tensor<crd::f64> factors[8] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                       Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc),
                                       Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> core(&alloc);
        RandOptions ropts;
        ropts.oversample = refdecomp::kRandOversample[row];
        ropts.power_iters = refdecomp::kRandPower[row];
        ropts.seed = 42U;
        REQUIRE(hosvd_rand<crd::f64>(x, {refdecomp::kRandRanks[row], nd}, {factors, nd}, core, &alloc, ropts) ==
                DecompStatus::Ok);
        Tensor<crd::f64> xhat(&alloc);
        REQUIRE(tucker_reconstruct<crd::f64>(core, {factors, nd}, xhat, &alloc) == DecompStatus::Ok);
        const crd::f64 err = rel_err(x, xhat);
        INFO("row " << row << " rand " << err << " exact " << refdecomp::kRandRecErrExact[row] << " ref "
                    << refdecomp::kRandRecErrRef[row]);
        if (refdecomp::kRandRecErrExact[row] > 1e-9)
        {
            // the HMT power-iteration claim: randomized within 5% of exact
            REQUIRE(err <= 1.05 * refdecomp::kRandRecErrExact[row]);
        }
        else
        {
            REQUIRE(err < 1e-12); // exact-multilinear-rank input: exact recovery
        }
    }
    // tall-rows adversary: a 300x6x6 tensor sends mode 0 through the CLASSIC
    // HMT path (rows > kRsvdGramRows — the Gram matrix must never be formed)
    // while modes 1/2 ride the Gram-operator path; gate vs the exact kernel.
    {
        const crd::u64 shp[3] = {300U, 6U, 6U};
        Tensor<crd::f64> xt(&alloc, {shp, 3U});
        {
            crd::hesap::stats::PhiloxRng rng(555U, 0U);
            for (crd::u64 i = 0; i < xt.size(); ++i)
            {
                xt.data()[i] = 2.0 * rng.next_f64() - 1.0;
            }
        }
        const TensorView<const crd::f64> xv(xt.view());
        const crd::u64 ranks[3] = {4U, 4U, 4U};
        Tensor<crd::f64> fe[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> ce(&alloc);
        REQUIRE(hosvd<crd::f64>(xv, {ranks, 3U}, {fe, 3U}, ce, &alloc) == DecompStatus::Ok);
        Tensor<crd::f64> xhat(&alloc);
        REQUIRE(tucker_reconstruct<crd::f64>(ce, {fe, 3U}, xhat, &alloc) == DecompStatus::Ok);
        const crd::f64 err_exact = rel_err(xv, xhat);
        Tensor<crd::f64> fr[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> cr(&alloc);
        RandOptions ropts;
        ropts.seed = 42U;
        REQUIRE(hosvd_rand<crd::f64>(xv, {ranks, 3U}, {fr, 3U}, cr, &alloc, ropts) == DecompStatus::Ok);
        REQUIRE(tucker_reconstruct<crd::f64>(cr, {fr, 3U}, xhat, &alloc) == DecompStatus::Ok);
        const crd::f64 err_rand = rel_err(xv, xhat);
        INFO("tall-rows exact " << err_exact << " rand " << err_rand);
        REQUIRE(err_rand <= 1.05 * err_exact);
        // run-twice determinism through BOTH dispatch paths
        Tensor<crd::f64> f2[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> c2(&alloc);
        REQUIRE(hosvd_rand<crd::f64>(xv, {ranks, 3U}, {f2, 3U}, c2, &alloc, ropts) == DecompStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u32 n = 0; n < 3U; ++n)
        {
            for (crd::u64 e = 0; e < fr[n].size(); ++e)
            {
                if (std::bit_cast<crd::u64>(fr[n].data()[e]) != std::bit_cast<crd::u64>(f2[n].data()[e]))
                {
                    ++mism;
                }
            }
        }
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("decomp: run-twice determinism - identical bits on every surface", "[v14j][decomp][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const TensorView<const crd::f64> x = ref_tensor(1U);
    const crd::u64 rank = 6U;
    Tensor<crd::f64> f1[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    Tensor<crd::f64> f2[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    crd::containers::Array<crd::f64> w1(&alloc);
    crd::containers::Array<crd::f64> w2(&alloc);
    w1.resize(rank);
    w2.resize(rank);
    CpOptions<crd::f64> opts;
    opts.max_iters = 10U;
    opts.tol = 0.0;
    opts.init = CpInit::Random;
    opts.seed = 7U;
    CpInfo<crd::f64> i1;
    CpInfo<crd::f64> i2;
    REQUIRE(cp_als<crd::f64>(x, rank, {f1, 3U}, {w1.data(), rank}, i1, &alloc, opts) == DecompStatus::Ok);
    REQUIRE(cp_als<crd::f64>(x, rank, {f2, 3U}, {w2.data(), rank}, i2, &alloc, opts) == DecompStatus::Ok);
    crd::u64 mism = 0;
    for (crd::u32 n = 0; n < 3U; ++n)
    {
        for (crd::u64 e = 0; e < f1[n].size(); ++e)
        {
            if (std::bit_cast<crd::u64>(f1[n].data()[e]) != std::bit_cast<crd::u64>(f2[n].data()[e]))
            {
                ++mism;
            }
        }
    }
    for (crd::u64 r = 0; r < rank; ++r)
    {
        if (std::bit_cast<crd::u64>(w1[r]) != std::bit_cast<crd::u64>(w2[r]))
        {
            ++mism;
        }
    }
    REQUIRE(mism == 0U);
    REQUIRE(std::bit_cast<crd::u64>(i1.fit) == std::bit_cast<crd::u64>(i2.fit));
    // randomized tucker: same seed, run twice
    const crd::u64 ranks[3] = {6U, 6U, 6U};
    Tensor<crd::f64> g1[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    Tensor<crd::f64> g2[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    Tensor<crd::f64> c1(&alloc);
    Tensor<crd::f64> c2(&alloc);
    RandOptions ropts;
    ropts.seed = 42U;
    REQUIRE(hosvd_rand<crd::f64>(x, {ranks, 3U}, {g1, 3U}, c1, &alloc, ropts) == DecompStatus::Ok);
    REQUIRE(hosvd_rand<crd::f64>(x, {ranks, 3U}, {g2, 3U}, c2, &alloc, ropts) == DecompStatus::Ok);
    mism = 0;
    for (crd::u32 n = 0; n < 3U; ++n)
    {
        for (crd::u64 e = 0; e < g1[n].size(); ++e)
        {
            if (std::bit_cast<crd::u64>(g1[n].data()[e]) != std::bit_cast<crd::u64>(g2[n].data()[e]))
            {
                ++mism;
            }
        }
    }
    for (crd::u64 e = 0; e < c1.size(); ++e)
    {
        if (std::bit_cast<crd::u64>(c1.data()[e]) != std::bit_cast<crd::u64>(c2.data()[e]))
        {
            ++mism;
        }
    }
    REQUIRE(mism == 0U);
}

TEST_CASE("decomp: the 1-2-4-8-16 worker moat - bit-identical decompositions", "[v14j][decomp][moat]")
{
    // 96x74x74 f64 = 4.2 MB: crosses the permute MT-dispatch threshold, so the
    // unfolding machinery actually runs multithreaded tiles under the pool.
    crd::memory::TlsfAllocator alloc(1U << 28);
    const crd::u64 shp[3] = {96U, 74U, 74U};
    Tensor<crd::f64> xt(&alloc, {shp, 3U});
    {
        crd::hesap::stats::PhiloxRng rng(123U, 0U);
        for (crd::u64 i = 0; i < xt.size(); ++i)
        {
            xt.data()[i] = 2.0 * rng.next_f64() - 1.0;
        }
    }
    const TensorView<const crd::f64> x(xt.view());
    const crd::u64 rank = 8U;
    const crd::u64 ranks[3] = {8U, 8U, 8U};
    // serial baselines (no pool)
    Tensor<crd::f64> fs[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    crd::containers::Array<crd::f64> ws(&alloc);
    ws.resize(rank);
    CpOptions<crd::f64> copts;
    copts.max_iters = 2U;
    copts.tol = 0.0;
    copts.init = CpInit::Random;
    copts.seed = 9U;
    CpInfo<crd::f64> cinfo;
    REQUIRE(cp_als<crd::f64>(x, rank, {fs, 3U}, {ws.data(), rank}, cinfo, &alloc, copts) == DecompStatus::Ok);
    Tensor<crd::f64> gs[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    Tensor<crd::f64> cs(&alloc);
    TuckerInfo<crd::f64> tinfo;
    TuckerOptions<crd::f64> topts;
    topts.max_iters = 2U;
    topts.tol = 0.0;
    RandOptions ropts;
    ropts.seed = 42U;
    REQUIRE(hooi_rand<crd::f64>(x, {ranks, 3U}, {gs, 3U}, cs, tinfo, &alloc, ropts, topts) == DecompStatus::Ok);
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        Tensor<crd::f64> fp[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        crd::containers::Array<crd::f64> wp(&alloc);
        wp.resize(rank);
        CpInfo<crd::f64> ci;
        const DecompStatus s1 = cp_als<crd::f64>(x, rank, {fp, 3U}, {wp.data(), rank}, ci, &alloc, copts);
        Tensor<crd::f64> gp[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> cp(&alloc);
        TuckerInfo<crd::f64> ti;
        const DecompStatus s2 = hooi_rand<crd::f64>(x, {ranks, 3U}, {gp, 3U}, cp, ti, &alloc, ropts, topts);
        crd::jobs::shutdown();
        REQUIRE(s1 == DecompStatus::Ok);
        REQUIRE(s2 == DecompStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u32 n = 0; n < 3U; ++n)
        {
            for (crd::u64 e = 0; e < fs[n].size(); ++e)
            {
                if (std::bit_cast<crd::u64>(fp[n].data()[e]) != std::bit_cast<crd::u64>(fs[n].data()[e]))
                {
                    ++mism;
                }
            }
            for (crd::u64 e = 0; e < gs[n].size(); ++e)
            {
                if (std::bit_cast<crd::u64>(gp[n].data()[e]) != std::bit_cast<crd::u64>(gs[n].data()[e]))
                {
                    ++mism;
                }
            }
        }
        for (crd::u64 r = 0; r < rank; ++r)
        {
            if (std::bit_cast<crd::u64>(wp[r]) != std::bit_cast<crd::u64>(ws[r]))
            {
                ++mism;
            }
        }
        for (crd::u64 e = 0; e < cs.size(); ++e)
        {
            if (std::bit_cast<crd::u64>(cp.data()[e]) != std::bit_cast<crd::u64>(cs.data()[e]))
            {
                ++mism;
            }
        }
        INFO("workers " << nw);
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("decomp: MTTKRP seam - cp_als_generic with an explicit functor bit-matches cp_als", "[v14j][decomp][seam]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const TensorView<const crd::f64> x = ref_tensor(0U);
    const crd::u64 rank = 4U;
    const crd::u32 nd = x.rank();
    CpOptions<crd::f64> opts;
    opts.max_iters = 8U;
    opts.tol = 0.0;
    opts.init = CpInit::Random;
    opts.seed = 5U;
    // path A: the dense entry point
    Tensor<crd::f64> fa[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    crd::containers::Array<crd::f64> wa(&alloc);
    wa.resize(rank);
    CpInfo<crd::f64> ia;
    REQUIRE(cp_als<crd::f64>(x, rank, {fa, 3U}, {wa.data(), rank}, ia, &alloc, opts) == DecompStatus::Ok);
    // path B: the seam - caller-owned functor + pre-initialized factors into the core
    Tensor<crd::f64> fb[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    for (crd::u32 n = 0; n < nd; ++n)
    {
        const crd::u64 fshape[2] = {x.shape(n), rank};
        REQUIRE(fb[n].resize({fshape, 2U}) == crd::hesap::tensor::TensorStatus::Ok);
        for (crd::u64 e = 0; e < x.shape(n) * rank; ++e) // the same keyed init cp_als(Random) performs
        {
            fb[n].data()[e] = crd::hesap::tensor::decompdetail::philox_uniform<crd::f64>(opts.seed, n, e);
        }
    }
    DenseMttkrp<crd::f64> mttkrp(x, &alloc);
    crd::f64 xnorm2 = 0.0;
    x.for_each([&xnorm2](const crd::u64*, const crd::f64& v) { xnorm2 += v * v; });
    crd::containers::Array<crd::f64> wb(&alloc);
    wb.resize(rank);
    CpInfo<crd::f64> ib;
    REQUIRE(cp_als_generic<crd::f64, DenseMttkrp<crd::f64>>({refdecomp::kTensorShape[0], nd}, xnorm2, mttkrp, rank,
                                                            {fb, 3U}, {wb.data(), rank}, ib, &alloc,
                                                            opts) == DecompStatus::Ok);
    crd::u64 mism = 0;
    for (crd::u32 n = 0; n < nd; ++n)
    {
        for (crd::u64 e = 0; e < fa[n].size(); ++e)
        {
            if (std::bit_cast<crd::u64>(fa[n].data()[e]) != std::bit_cast<crd::u64>(fb[n].data()[e]))
            {
                ++mism;
            }
        }
    }
    for (crd::u64 r = 0; r < rank; ++r)
    {
        if (std::bit_cast<crd::u64>(wa[r]) != std::bit_cast<crd::u64>(wb[r]))
        {
            ++mism;
        }
    }
    REQUIRE(mism == 0U);
    REQUIRE(std::bit_cast<crd::u64>(ia.fit) == std::bit_cast<crd::u64>(ib.fit));
}

TEST_CASE("decomp: boundary adversaries - zeros, bad input, bounded iteration", "[v14j][decomp][boundary]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    // all-zeros tensor: every entry point returns Ok with a finite exact answer
    const crd::u64 shp[3] = {6U, 5U, 4U};
    Tensor<crd::f64> zt = Tensor<crd::f64>::zeros(&alloc, {shp, 3U});
    const TensorView<const crd::f64> z(zt.view());
    {
        Tensor<crd::f64> f[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        crd::containers::Array<crd::f64> w(&alloc);
        w.resize(3U);
        CpInfo<crd::f64> info;
        REQUIRE(cp_als<crd::f64>(z, 3U, {f, 3U}, {w.data(), 3U}, info, &alloc) == DecompStatus::Ok);
        REQUIRE(info.converged);
        REQUIRE(info.fit == 1.0);
        for (crd::u64 r = 0; r < 3U; ++r)
        {
            REQUIRE(w[r] == 0.0);
        }
    }
    {
        const crd::u64 ranks[3] = {2U, 2U, 2U};
        Tensor<crd::f64> f[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        Tensor<crd::f64> core(&alloc);
        TuckerInfo<crd::f64> info;
        REQUIRE(hosvd<crd::f64>(z, {ranks, 3U}, {f, 3U}, core, &alloc, &info) == DecompStatus::Ok);
        REQUIRE(info.fit == 1.0);
        REQUIRE(max_abs(core.data(), core.size()) == 0.0);
        TuckerInfo<crd::f64> hi;
        REQUIRE(hooi<crd::f64>(z, {ranks, 3U}, {f, 3U}, core, hi, &alloc) == DecompStatus::Ok);
        REQUIRE(hi.converged);
    }
    // bad input statuses
    const TensorView<const crd::f64> x = ref_tensor(0U);
    {
        Tensor<crd::f64> f[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        crd::containers::Array<crd::f64> w(&alloc);
        w.resize(4U);
        CpInfo<crd::f64> info;
        REQUIRE(cp_als<crd::f64>(x, 0U, {f, 3U}, {w.data(), 0U}, info, &alloc) == DecompStatus::BadInput);
        REQUIRE(cp_als<crd::f64>(x, 4U, {f, 2U}, {w.data(), 4U}, info, &alloc) == DecompStatus::BadInput);
        REQUIRE(cp_als<crd::f64>(x, 4U, {f, 3U}, {w.data(), 3U}, info, &alloc) == DecompStatus::BadInput);
        const crd::u64 ranks_bad[3] = {13U, 5U, 4U}; // 13 > I_0 = 12
        Tensor<crd::f64> core(&alloc);
        REQUIRE(hosvd<crd::f64>(x, {ranks_bad, 3U}, {f, 3U}, core, &alloc) == DecompStatus::BadInput);
        const crd::u64 ranks_zero[3] = {0U, 5U, 4U};
        REQUIRE(hosvd<crd::f64>(x, {ranks_zero, 3U}, {f, 3U}, core, &alloc) == DecompStatus::BadInput);
    }
    // bounded iteration: budget exhausted with tol > 0 reports NotConverged
    {
        Tensor<crd::f64> f[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        crd::containers::Array<crd::f64> w(&alloc);
        w.resize(5U);
        CpOptions<crd::f64> opts;
        opts.max_iters = 2U;
        opts.tol = 1e-30;
        CpInfo<crd::f64> info;
        REQUIRE(cp_als<crd::f64>(x, 5U, {f, 3U}, {w.data(), 5U}, info, &alloc, opts) == DecompStatus::NotConverged);
        REQUIRE(info.iters == 2U);
        REQUIRE(!info.converged);
        const crd::u64 ranks[3] = {3U, 3U, 3U};
        Tensor<crd::f64> core(&alloc);
        TuckerInfo<crd::f64> ti;
        TuckerOptions<crd::f64> topts;
        topts.max_iters = 1U;
        topts.tol = 1e-30;
        REQUIRE(hooi<crd::f64>(x, {ranks, 3U}, {f, 3U}, core, ti, &alloc, topts) == DecompStatus::NotConverged);
        REQUIRE(ti.iters == 1U);
    }
    // convergence: a generous budget with a sane tol converges on lrnoise
    {
        const TensorView<const crd::f64> xl = ref_tensor(1U);
        Tensor<crd::f64> f[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        crd::containers::Array<crd::f64> w(&alloc);
        w.resize(6U);
        CpOptions<crd::f64> opts;
        opts.max_iters = 100U;
        opts.tol = 1e-10;
        crd::containers::Array<crd::f64> hist(&alloc);
        hist.resize(100U, -1.0);
        opts.fit_history = {hist.data(), 100U};
        CpInfo<crd::f64> info;
        REQUIRE(cp_als<crd::f64>(xl, 6U, {f, 3U}, {w.data(), 6U}, info, &alloc, opts) == DecompStatus::Ok);
        REQUIRE(info.converged);
        REQUIRE(info.iters < 100U);
        REQUIRE(hist[info.iters - 1U] == info.fit); // history filled through the last sweep
        for (crd::u32 i = 1; i < info.iters; ++i)   // ALS fit is monotone (up to roundoff)
        {
            REQUIRE(hist[i] >= hist[i - 1U] - 1e-12);
        }
    }
    // the batched sketch fill is bit-equal to the keyed per-index definition
    {
        crd::containers::Array<crd::f64> batch(&alloc);
        const crd::u64 count = 700U; // crosses the 256-sample chunk boundary twice
        batch.resize(count);
        crd::hesap::tensor::decompdetail::philox_gaussian_fill<crd::f64>(42U, 7U, batch.data(), count);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < count; ++i)
        {
            const crd::f64 keyed = crd::hesap::tensor::decompdetail::philox_gaussian<crd::f64>(42U, 7U, i);
            if (std::bit_cast<crd::u64>(batch[i]) != std::bit_cast<crd::u64>(keyed))
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
    // fit floor sanity: rank-1 weights positive, factors unit-norm
    {
        Tensor<crd::f64> f[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        crd::containers::Array<crd::f64> w(&alloc);
        w.resize(1U);
        CpOptions<crd::f64> opts;
        opts.max_iters = 30U;
        opts.tol = 0.0;
        CpInfo<crd::f64> info;
        REQUIRE(cp_als<crd::f64>(x, 1U, {f, 3U}, {w.data(), 1U}, info, &alloc, opts) == DecompStatus::Ok);
        REQUIRE(w[0] > 0.0);
        for (crd::u32 n = 0; n < 3U; ++n)
        {
            crd::f64 ss = 0.0;
            for (crd::u64 i = 0; i < f[n].shape(0); ++i)
            {
                ss += f[n].data()[i] * f[n].data()[i];
            }
            REQUIRE(std::sqrt(ss) > 0.9999);
            REQUIRE(std::sqrt(ss) < 1.0001);
        }
    }
}
