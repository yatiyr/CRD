// v14-k TT gates — TtTensor + tt_svd/tt_round/algebra/eval + maxvol + tt_cross.
// Oracle expectations frozen in ref_tt.inc (scripts/v14k_tt_oracle.py: tntorch
// 1.1.2 reference + numpy mirrors of the EXACT ported algorithms; ttpy is
// N/A-with-check — numpy.distutils removed on py3.12).
// Serial-only v1 (deliberate; see tt.hpp header) — the determinism gates
// (run-twice bit-identity for tt_svd and tt_cross) stay regardless.
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/tt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <limits>

using crd::hesap::tensor::MaxvolInfo;
using crd::hesap::tensor::maxvol;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;
using crd::hesap::tensor::tt_add;
using crd::hesap::tensor::tt_contract;
using crd::hesap::tensor::tt_cross;
using crd::hesap::tensor::tt_dot;
using crd::hesap::tensor::tt_eval;
using crd::hesap::tensor::tt_eval_lerp;
using crd::hesap::tensor::tt_eval_many;
using crd::hesap::tensor::tt_eval_workspace;
using crd::hesap::tensor::tt_hadamard;
using crd::hesap::tensor::tt_norm;
using crd::hesap::tensor::tt_round;
using crd::hesap::tensor::tt_svd;
using crd::hesap::tensor::TtCrossInfo;
using crd::hesap::tensor::TtCrossParams;
using crd::hesap::tensor::TtTensor;

namespace
{

#include "ref_tt.inc"

// deterministic integer-hash values (bit-exact vs the oracle: u32 wrap arith)
[[nodiscard]] crd::f64 hash_val(crd::u64 k, crd::u64 a, crd::u64 i, crd::u64 b) noexcept
{
    const crd::u32 h = (static_cast<crd::u32>(k) * 73856093U) ^ (static_cast<crd::u32>(a) * 19349663U) ^
                       (static_cast<crd::u32>(i) * 83492791U) ^ (static_cast<crd::u32>(b) * 2971215073U);
    return static_cast<crd::f64>(h) * (1.0 / 4294967296.0) - 0.5;
}

// counting allocator: forwards to a parent, counts every allocation-side call
// (the allocation-free-eval gate; grep found no existing counting pattern in
// tests/ — memory tests use MemoryStats, which TLSF also updates, but a call
// COUNT is the sharper gate here).
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    explicit CountingAllocator(crd::memory::IAllocator* parent) noexcept : m_parent(parent)
    {
        m_name = "CountingAllocator";
    }
    void* allocate(crd::usize size, crd::usize alignment) override
    {
        ++m_allocs;
        return m_parent->allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { m_parent->deallocate(p); }
    bool owns(const void* p) const noexcept override { return m_parent->owns(p); }
    void* reallocate(void* p, crd::usize old_size, crd::usize new_size, crd::usize alignment) override
    {
        ++m_allocs;
        return m_parent->reallocate(p, old_size, new_size, alignment);
    }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        ++m_allocs;
        return m_parent->try_allocate(size, alignment);
    }
    [[nodiscard]] crd::u64 alloc_calls() const noexcept { return m_allocs; }

private:
    crd::memory::IAllocator* m_parent;
    crd::u64 m_allocs = 0;
};

// dense Hilbert 4D: A[i,j,k,l] = 1/(1+i+j+k+l) (formula bit-exact vs oracle)
void fill_hilbert(Tensor<crd::f64>& t) noexcept
{
    const crd::u64 n = kTtRefHilbertN;
    crd::f64* p = t.data();
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

// smooth gravitational-like kernel on the frozen grid (oracle formula order)
[[nodiscard]] crd::f64 smooth_axis(crd::u64 i) noexcept
{
    return -1.0 + 2.0 * static_cast<crd::f64>(i) / static_cast<crd::f64>(kTtRefSmoothN - 1U);
}

[[nodiscard]] crd::f64 smooth_f(crd::containers::ConstSpan<crd::u64> idx) noexcept
{
    crd::f64 s = kTtRefSmoothSoft;
    for (crd::u64 k = 0; k < idx.size(); ++k)
    {
        const crd::f64 x = smooth_axis(idx[k]);
        s += x * x;
    }
    return 1.0 / std::sqrt(s);
}

[[nodiscard]] crd::f64 rel_fro_err(const Tensor<crd::f64>& a, const Tensor<crd::f64>& b) noexcept
{
    crd::f64 num = 0.0;
    crd::f64 den = 0.0;
    for (crd::u64 i = 0; i < a.size(); ++i)
    {
        const crd::f64 d = a.data()[i] - b.data()[i];
        num += d * d;
        den += a.data()[i] * a.data()[i];
    }
    return std::sqrt(num / (den > 0.0 ? den : 1.0));
}

// build the frozen exact-low-rank TT (integer-hash cores, ranks 1,3,3,3,1)
[[nodiscard]] TtTensor<crd::f64> make_lowrank_tt(crd::memory::IAllocator* alloc)
{
    TtTensor<crd::f64> tt(alloc);
    const crd::u64 n = kTtRefLowrankN;
    const crd::u64 r = kTtRefLowrankRank;
    const crd::u64 shp[4] = {n, n, n, n};
    const crd::u64 rks[5] = {1U, r, r, r, 1U};
    REQUIRE(tt.init({shp, 4}, {rks, 5}) == TensorStatus::Ok);
    for (crd::u32 k = 0; k < 4U; ++k)
    {
        const crd::u64 rl = tt.rank(k);
        const crd::u64 rr = tt.rank(k + 1U);
        crd::f64* g = tt.core(k);
        for (crd::u64 a = 0; a < rl; ++a)
        {
            for (crd::u64 i = 0; i < n; ++i)
            {
                for (crd::u64 b = 0; b < rr; ++b)
                {
                    g[(a * n + i) * rr + b] = hash_val(k, a, i, b);
                }
            }
        }
    }
    return tt;
}

[[nodiscard]] bool tt_bit_equal(const TtTensor<crd::f64>& a, const TtTensor<crd::f64>& b) noexcept
{
    if (a.dims() != b.dims())
    {
        return false;
    }
    for (crd::u32 k = 0; k < a.dims(); ++k)
    {
        if (a.core_size(k) != b.core_size(k))
        {
            return false;
        }
        for (crd::u64 i = 0; i < a.core_size(k); ++i)
        {
            if (std::bit_cast<crd::u64>(a.core(k)[i]) != std::bit_cast<crd::u64>(b.core(k)[i]))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("tt: maxvol matches the frozen Goreinov-Oseledets oracle", "[v14k][tt][maxvol]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u64 nr = 16;
    const crd::u64 r = 4;
    crd::f64 a[16 * 4];
    for (crd::u64 i = 0; i < nr; ++i)
    {
        for (crd::u64 j = 0; j < r; ++j)
        {
            a[i * r + j] = hash_val(9U, i, j, 0U);
        }
    }
    crd::u64 rows[4] = {};
    crd::f64 coeff[16 * 4];
    MaxvolInfo info;
    REQUIRE(maxvol<crd::f64>(&alloc, a, nr, r, 1.05, 100U, {rows, 4}, coeff, &info) == TensorStatus::Ok);
    REQUIRE(info.converged);
    REQUIRE(!info.singular);
    REQUIRE(info.iters == static_cast<crd::u32>(kTtRefMaxvolIters));
    // frozen row SET (sorted; the oracle freezes the sorted set)
    crd::u64 sorted[4];
    for (crd::u64 i = 0; i < r; ++i)
    {
        sorted[i] = rows[i];
    }
    for (crd::u64 i = 1; i < r; ++i) // tiny insertion sort (no std::sort — house rule)
    {
        const crd::u64 v = sorted[i];
        crd::u64 p = i;
        while (p > 0U && sorted[p - 1U] > v)
        {
            sorted[p] = sorted[p - 1U];
            --p;
        }
        sorted[p] = v;
    }
    for (crd::u64 i = 0; i < r; ++i)
    {
        REQUIRE(sorted[i] == kTtRefMaxvolRows[i]);
    }
    // coefficient bound + identity on the selected rows
    crd::f64 maxc = 0.0;
    for (crd::u64 i = 0; i < nr * r; ++i)
    {
        const crd::f64 v = coeff[i] < 0.0 ? -coeff[i] : coeff[i];
        if (v > maxc)
        {
            maxc = v;
        }
    }
    REQUIRE(maxc <= 1.05 + 1e-12);
    for (crd::u64 s = 0; s < r; ++s)
    {
        for (crd::u64 j = 0; j < r; ++j)
        {
            const crd::f64 want = s == j ? 1.0 : 0.0;
            REQUIRE(std::abs(coeff[rows[s] * r + j] - want) < 1e-10);
        }
    }
    // boundary: square input -> identity selection; N < r -> BadInput
    crd::u64 rows4[4];
    REQUIRE(maxvol<crd::f64>(&alloc, a, 4U, 4U, 1.05, 100U, {rows4, 4}, nullptr, &info) == TensorStatus::Ok);
    for (crd::u64 i = 0; i < 4U; ++i)
    {
        REQUIRE(rows4[i] == i);
    }
    REQUIRE(maxvol<crd::f64>(&alloc, a, 3U, 4U, 1.05, 100U, {rows4, 4}, nullptr, &info) ==
            TensorStatus::BadInput);
}

TEST_CASE("tt: tt_svd hilbert - frozen ranks, error <= eps, per-point oracle, run-twice bits",
          "[v14k][tt][svd]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 n = kTtRefHilbertN;
    const crd::u64 shp[4] = {n, n, n, n};
    Tensor<crd::f64> dense(&alloc, {shp, 4});
    fill_hilbert(dense);
    const crd::u64* want_ranks[3] = {kTtRefHilbertRanksEm04, kTtRefHilbertRanksEm08, kTtRefHilbertRanksEm12};
    for (crd::u64 e = 0; e < 3U; ++e)
    {
        const crd::f64 eps = kTtRefHilbertEps[e];
        TtTensor<crd::f64> tt(&alloc);
        REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), eps, 0U, tt) == TensorStatus::Ok);
        INFO("eps " << eps);
        for (crd::u32 k = 1; k < 4U; ++k)
        {
            REQUIRE(tt.rank(k) == want_ranks[e][k - 1U]);
        }
        Tensor<crd::f64> rec(&alloc);
        REQUIRE(tt_contract<crd::f64>(&alloc, tt, rec) == TensorStatus::Ok);
        const crd::f64 err = rel_fro_err(dense, rec);
        REQUIRE(err <= eps); // the compression-error contract
    }
    // per-point values vs the frozen tntorch TT at eps=1e-8
    {
        TtTensor<crd::f64> tt(&alloc);
        REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), 1e-8, 0U, tt) == TensorStatus::Ok);
        crd::f64 work[64];
        REQUIRE(tt_eval_workspace(tt) <= 64U);
        for (crd::u64 p = 0; p < 8U; ++p)
        {
            const crd::f64 v = tt_eval<crd::f64>(tt, {kTtRefEvalIdxFlat + p * 4U, 4}, {work, 64});
            REQUIRE(std::abs(v - kTtRefEvalTt[p]) <= kTtRefEvalGate);
            REQUIRE(std::abs(v - kTtRefEvalTrue[p]) <= 1e-7);
        }
        // run-twice bit-identity (determinism gate; serial path)
        TtTensor<crd::f64> tt2(&alloc);
        REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), 1e-8, 0U, tt2) == TensorStatus::Ok);
        REQUIRE(tt_bit_equal(tt, tt2));
    }
}

TEST_CASE("tt: exact low-rank recovery is EXACT + algebra gates (add/hadamard/dot/norm/round)",
          "[v14k][tt][algebra]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    TtTensor<crd::f64> lr = make_lowrank_tt(&alloc);
    Tensor<crd::f64> dense(&alloc);
    REQUIRE(tt_contract<crd::f64>(&alloc, lr, dense) == TensorStatus::Ok);
    // frozen Frobenius norm of the constructed tensor (order-tolerant gate)
    crd::f64 nrm = 0.0;
    for (crd::u64 i = 0; i < dense.size(); ++i)
    {
        nrm += dense.data()[i] * dense.data()[i];
    }
    nrm = std::sqrt(nrm);
    REQUIRE(std::abs(nrm - kTtRefLowrankNorm) <= 1e-12 * kTtRefLowrankNorm);
    // tt_svd recovers the construction ranks EXACTLY, error ~ machine
    TtTensor<crd::f64> tt(&alloc);
    REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), 1e-10, 0U, tt) == TensorStatus::Ok);
    for (crd::u32 k = 1; k < 4U; ++k)
    {
        REQUIRE(tt.rank(k) == kTtRefLowrankRank);
    }
    Tensor<crd::f64> rec(&alloc);
    REQUIRE(tt_contract<crd::f64>(&alloc, tt, rec) == TensorStatus::Ok);
    REQUIRE(rel_fro_err(dense, rec) <= 1e-13);
    // add: structural ranks double; round(1e-10) collapses back; values == 2A
    TtTensor<crd::f64> sum(&alloc);
    REQUIRE(tt_add<crd::f64>(&alloc, lr, lr, sum) == TensorStatus::Ok);
    for (crd::u32 k = 1; k < 4U; ++k)
    {
        REQUIRE(sum.rank(k) == 2U * kTtRefLowrankRank);
    }
    REQUIRE(tt_round<crd::f64>(&alloc, sum, 1e-10, 0U) == TensorStatus::Ok);
    for (crd::u32 k = 1; k < 4U; ++k)
    {
        REQUIRE(sum.rank(k) == kTtRefLowrankRank);
    }
    Tensor<crd::f64> sum_dense(&alloc);
    REQUIRE(tt_contract<crd::f64>(&alloc, sum, sum_dense) == TensorStatus::Ok);
    crd::f64 worst = 0.0;
    for (crd::u64 i = 0; i < dense.size(); ++i)
    {
        const crd::f64 d = sum_dense.data()[i] - 2.0 * dense.data()[i];
        if (d * d > worst)
        {
            worst = d * d;
        }
    }
    REQUIRE(worst <= 1e-24);
    // hadamard: elementwise square, structural rank product
    TtTensor<crd::f64> had(&alloc);
    REQUIRE(tt_hadamard<crd::f64>(&alloc, lr, lr, had) == TensorStatus::Ok);
    for (crd::u32 k = 1; k < 4U; ++k)
    {
        REQUIRE(had.rank(k) == kTtRefLowrankRank * kTtRefLowrankRank);
    }
    Tensor<crd::f64> had_dense(&alloc);
    REQUIRE(tt_contract<crd::f64>(&alloc, had, had_dense) == TensorStatus::Ok);
    worst = 0.0;
    for (crd::u64 i = 0; i < dense.size(); ++i)
    {
        const crd::f64 d = had_dense.data()[i] - dense.data()[i] * dense.data()[i];
        if (d * d > worst)
        {
            worst = d * d;
        }
    }
    REQUIRE(worst <= 1e-24);
    // dot + norm vs dense reductions
    crd::f64 dot = 0.0;
    REQUIRE(tt_dot<crd::f64>(&alloc, lr, sum, dot) == TensorStatus::Ok);
    crd::f64 want_dot = 0.0;
    for (crd::u64 i = 0; i < dense.size(); ++i)
    {
        want_dot += dense.data()[i] * (2.0 * dense.data()[i]);
    }
    REQUIRE(std::abs(dot - want_dot) <= 1e-12 * std::abs(want_dot));
    crd::f64 tnorm = 0.0;
    REQUIRE(tt_norm<crd::f64>(&alloc, lr, tnorm) == TensorStatus::Ok);
    REQUIRE(std::abs(tnorm - nrm) <= 1e-12 * nrm);
}

TEST_CASE("tt: rounding tolerance honored - hilbert fine TT rounded to 1e-4", "[v14k][tt][round]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 n = kTtRefHilbertN;
    const crd::u64 shp[4] = {n, n, n, n};
    Tensor<crd::f64> dense(&alloc, {shp, 4});
    fill_hilbert(dense);
    TtTensor<crd::f64> tt(&alloc);
    REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), 1e-12, 0U, tt) == TensorStatus::Ok);
    REQUIRE(tt_round<crd::f64>(&alloc, tt, 1e-4, 0U) == TensorStatus::Ok);
    for (crd::u32 k = 1; k < 4U; ++k) // frozen: same ranks as direct tt_svd@1e-4
    {
        REQUIRE(tt.rank(k) == kTtRefHilbertRanksEm04[k - 1U]);
    }
    Tensor<crd::f64> rec(&alloc);
    REQUIRE(tt_contract<crd::f64>(&alloc, tt, rec) == TensorStatus::Ok);
    REQUIRE(rel_fro_err(dense, rec) <= 1e-4); // error <= tol * ||A|| (gated)
    // rank-cap path: max_rank = 3 must clamp every interior rank
    TtTensor<crd::f64> capped(&alloc);
    REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), 1e-12, 3U, capped) == TensorStatus::Ok);
    for (crd::u32 k = 1; k < 4U; ++k)
    {
        REQUIRE(capped.rank(k) <= 3U);
    }
}

TEST_CASE("tt: smooth kernel tt_svd matches the frozen oracle", "[v14k][tt][svd]")
{
    crd::memory::TlsfAllocator alloc(1U << 27);
    const crd::u64 n = kTtRefSmoothN;
    const crd::u64 shp[4] = {n, n, n, n};
    Tensor<crd::f64> dense(&alloc, {shp, 4});
    crd::u64 ix[4];
    for (ix[0] = 0; ix[0] < n; ++ix[0])
    {
        for (ix[1] = 0; ix[1] < n; ++ix[1])
        {
            for (ix[2] = 0; ix[2] < n; ++ix[2])
            {
                for (ix[3] = 0; ix[3] < n; ++ix[3])
                {
                    dense.data()[((ix[0] * n + ix[1]) * n + ix[2]) * n + ix[3]] = smooth_f({ix, 4});
                }
            }
        }
    }
    TtTensor<crd::f64> tt(&alloc);
    REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), kTtRefSmoothSvdEps, 0U, tt) == TensorStatus::Ok);
    for (crd::u32 k = 1; k < 4U; ++k)
    {
        REQUIRE(tt.rank(k) == kTtRefSmoothSvdRanks[k - 1U]);
    }
    Tensor<crd::f64> rec(&alloc);
    REQUIRE(tt_contract<crd::f64>(&alloc, tt, rec) == TensorStatus::Ok);
    REQUIRE(rel_fro_err(dense, rec) <= kTtRefSmoothSvdEps);
}

TEST_CASE("tt: tt_cross builds the smooth kernel from evaluations within the frozen budget",
          "[v14k][tt][cross]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 n = kTtRefSmoothN;
    const crd::u64 shp[4] = {n, n, n, n};
    TtCrossParams<crd::f64> params;
    params.max_rank = kTtRefCrossRank;
    params.max_sweeps = static_cast<crd::u32>(kTtRefCrossSweeps);
    params.tol = kTtRefCrossTol;
    params.val_size = kTtRefCrossValSize;
    params.seed = 42U;
    TtTensor<crd::f64> tt(&alloc);
    TtCrossInfo<crd::f64> info;
    crd::u64 evals_expected = 0; // per sweep pair: sum fibers L2R(0..d-2) + R2L(d-1..1) + final j=0
    {
        const crd::u64 rs[5] = {1U, kTtRefCrossRank, kTtRefCrossRank, kTtRefCrossRank, 1U};
        for (crd::u32 j = 0; j < 3U; ++j)
        {
            evals_expected += rs[j] * n * rs[j + 1U];
        }
        for (crd::u32 j = 3U; j >= 1U; --j)
        {
            evals_expected += rs[j] * n * rs[j + 1U];
        }
        evals_expected += rs[0] * n * rs[1];
    }
    REQUIRE(tt_cross<crd::f64>(&alloc, {shp, 4}, smooth_f, params, tt, &info) == TensorStatus::Ok);
    REQUIRE(info.converged);
    REQUIRE(info.sweeps <= static_cast<crd::u32>(kTtRefCrossSweeps));
    REQUIRE(info.evals == evals_expected * info.sweeps);
    REQUIRE(evals_expected == kTtRefCrossMirrorEvals); // budget parity with the oracle mirror
    // probe accuracy against TRUTH at the frozen indices
    crd::f64 work[64];
    REQUIRE(tt_eval_workspace(tt) <= 64U);
    crd::f64 worst = 0.0;
    crd::f64 scale = 0.0;
    for (crd::u64 p = 0; p < 16U; ++p)
    {
        const crd::f64 v = tt_eval<crd::f64>(tt, {kTtRefCrossProbeIdxFlat + p * 4U, 4}, {work, 64});
        const crd::f64 d = std::abs(v - kTtRefCrossProbeTrue[p]);
        if (d > worst)
        {
            worst = d;
        }
        const crd::f64 t = std::abs(kTtRefCrossProbeTrue[p]);
        if (t > scale)
        {
            scale = t;
        }
    }
    REQUIRE(worst / scale <= kTtRefCrossGate);
    // determinism: run-twice bit-identity (Philox-seeded starts, serial path)
    TtTensor<crd::f64> tt2(&alloc);
    TtCrossInfo<crd::f64> info2;
    REQUIRE(tt_cross<crd::f64>(&alloc, {shp, 4}, smooth_f, params, tt2, &info2) == TensorStatus::Ok);
    REQUIRE(info2.converged == info.converged);
    REQUIRE(info2.sweeps == info.sweeps);
    REQUIRE(std::bit_cast<crd::u64>(info2.val_error) == std::bit_cast<crd::u64>(info.val_error));
    REQUIRE(tt_bit_equal(tt, tt2));
    // bounded iteration: an impossible tol on a tiny budget must NOT converge
    // (status stays Ok; NotConverged rides the info struct — tt.hpp header)
    TtCrossParams<crd::f64> hard = params;
    hard.max_rank = 2U;
    hard.max_sweeps = 2U;
    hard.tol = 1e-14;
    TtTensor<crd::f64> tt3(&alloc);
    TtCrossInfo<crd::f64> info3;
    REQUIRE(tt_cross<crd::f64>(&alloc, {shp, 4}, smooth_f, hard, tt3, &info3) == TensorStatus::Ok);
    REQUIRE(!info3.converged);
    REQUIRE(info3.sweeps == 2U);
}

namespace
{

// One tt_cross run of f = scale * g over a full grid of up to four modes, and the worst |TT - f| over that grid
// relative to the peak |f| (a NaN anywhere makes `finite` false).
template <typename T> struct CrossScaleRun
{
    TtCrossInfo<T> info;
    crd::f64 worst_rel = 0.0;
    crd::f64 peak = 0.0;
    bool finite = true;
    bool ok = false;
};

template <typename T, typename G>
CrossScaleRun<T> cross_scale_run(crd::memory::IAllocator* alloc, const crd::u64* shp, crd::u32 d, crd::u64 rank, T tol,
                                 crd::u64 val_size, crd::u32 sweeps, T scale, const G& g, TtTensor<T>& tt)
{
    TtCrossParams<T> params;
    params.max_rank = rank;
    params.max_sweeps = sweeps;
    params.tol = tol;
    params.val_size = val_size;
    params.seed = 7U;
    const auto f = [&](crd::containers::ConstSpan<crd::u64> ix) noexcept
    {
        return scale * static_cast<T>(g(ix.data()));
    };
    CrossScaleRun<T> out;
    out.ok = tt_cross<T>(alloc, {shp, d}, f, params, tt, &out.info) == TensorStatus::Ok;
    if (!out.ok)
    {
        return out;
    }
    T work[64];
    crd::u64 ix[4] = {0U, 0U, 0U, 0U};
    crd::f64 worst = 0.0;
    for (;;)
    {
        const T truth = f({ix, d});
        const crd::f64 e =
            std::abs(static_cast<crd::f64>(tt_eval<T>(tt, {ix, d}, {work, 64})) - static_cast<crd::f64>(truth));
        const bool e_finite = e <= std::numeric_limits<crd::f64>::max();
        out.finite = out.finite && e_finite;
        worst = e > worst ? e : worst;
        const crd::f64 t = std::abs(static_cast<crd::f64>(truth));
        out.peak = t > out.peak ? t : out.peak;
        crd::u32 k = 0;
        for (; k < d; ++k)
        {
            if (++ix[k] < shp[k])
            {
                break;
            }
            ix[k] = 0U;
        }
        if (k == d)
        {
            break;
        }
    }
    out.worst_rel = out.peak > 0.0 ? worst / out.peak : worst;
    return out;
}

[[nodiscard]] crd::f64 cross_cos2(const crd::u64* i) noexcept
{
    return std::cos(0.01 * (static_cast<crd::f64>(i[0]) + 2.0 * static_cast<crd::f64>(i[1])));
}

[[nodiscard]] crd::f64 cross_sin3(const crd::u64* i) noexcept
{
    return std::sin(0.05 * static_cast<crd::f64>(i[0] + i[1] + i[2]) + 0.3);
}

[[nodiscard]] crd::f64 cross_sin4(const crd::u64* i) noexcept
{
    return std::sin(0.3 * static_cast<crd::f64>(i[0] + i[1] + i[2] + i[3]));
}

[[nodiscard]] crd::f64 cross_lin4(const crd::u64* i) noexcept
{
    return 1.0 + static_cast<crd::f64>(i[0]) + static_cast<crd::f64>(i[1] * i[2]) + 0.5 * static_cast<crd::f64>(i[3]);
}

[[nodiscard]] crd::f64 cross_one(const crd::u64*) noexcept
{
    return 1.0;
}

// the gate of a run: status Ok, converged with val_error below tol, finite, and within `gate` of f over the grid
template <typename T> void require_cross_accurate(const CrossScaleRun<T>& run, T tol, crd::f64 gate)
{
    REQUIRE(run.ok);
    REQUIRE(run.finite);
    REQUIRE(run.info.converged);
    REQUIRE(run.info.val_error < tol);
    REQUIRE(run.worst_rel <= gate);
}

} // namespace

TEST_CASE("tt: tt_cross stays finite and accurate on large fiber blocks near the edges of the square-safe range",
          "[v14k][tt][cross][scale]")
{
    // Each value is inside the range where one square is finite, but the Householder sum alpha^2 + dot(tail) over the
    // block's rows (400, 512 and 256 here) overflows; base gave a NaN TT on all three.
    crd::memory::TlsfAllocator alloc(1U << 28);
    {
        const crd::u64 shp[2] = {400U, 400U};
        TtTensor<crd::f64> tt(&alloc);
        require_cross_accurate(cross_scale_run<crd::f64>(&alloc, shp, 2U, 4U, 1e-10, 64U, 10U, 1.2e153, cross_cos2, tt),
                               1e-10, kTtRefCrossGate);
    }
    {
        const crd::u64 shp[3] = {64U, 64U, 64U};
        TtTensor<crd::f64> tt(&alloc);
        require_cross_accurate(cross_scale_run<crd::f64>(&alloc, shp, 3U, 8U, 1e-10, 64U, 10U, 1e153, cross_sin3, tt),
                               1e-10, kTtRefCrossGate);
    }
    {
        const crd::u64 shp[2] = {256U, 256U};
        TtTensor<crd::f32> tt(&alloc);
        require_cross_accurate(cross_scale_run<crd::f32>(&alloc, shp, 2U, 2U, 1e-4F, 64U, 10U, 2e18F, cross_one, tt),
                               1e-4F, 1e-5);
    }
    {
        // 200 validation values of 1.3e153: the plain reference sum of squares overflows
        const crd::u64 shp[3] = {8U, 8U, 8U};
        TtTensor<crd::f64> tt(&alloc);
        require_cross_accurate(cross_scale_run<crd::f64>(&alloc, shp, 3U, 4U, 1e-10, 200U, 10U, 1.3e153, cross_one, tt),
                               1e-10, kTtRefCrossGate);
    }
}

TEST_CASE("tt: tt_cross reports its validation error honestly far below the square-safe range",
          "[v14k][tt][cross][scale]")
{
    // The validation differences are about eps times the values, so their squares underflowed some 16 decades above
    // the reference's own limit and base reported val_error 0 at these scales (5.7e-16 at scale 1).
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 shp[4] = {9U, 9U, 9U, 9U};
    TtTensor<crd::f64> base_tt(&alloc);
    const CrossScaleRun<crd::f64> unit =
        cross_scale_run<crd::f64>(&alloc, shp, 4U, 8U, 1e-10, 64U, 10U, 1.0, cross_sin4, base_tt);
    require_cross_accurate(unit, 1e-10, kTtRefCrossGate);
    REQUIRE(unit.info.val_error > 0.0);
    for (const crd::f64 scale : {1.5e-153, 1e-150, 1e-200, 1e-305})
    {
        INFO("scale " << scale);
        TtTensor<crd::f64> tt(&alloc);
        const CrossScaleRun<crd::f64> run =
            cross_scale_run<crd::f64>(&alloc, shp, 4U, 8U, 1e-10, 64U, 10U, scale, cross_sin4, tt);
        require_cross_accurate(run, 1e-10, kTtRefCrossGate);
        // the same few rounding errors relative to the values, so within a small factor of the scale-1 error
        REQUIRE(run.info.val_error >= 0.25 * unit.info.val_error);
        REQUIRE(run.info.val_error <= 4.0 * unit.info.val_error);
    }
}

TEST_CASE("tt: tt_cross is accurate from subnormal values to 1e300", "[v14k][tt][cross][scale]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 shp[4] = {6U, 6U, 6U, 6U};
    for (const crd::f64 scale : {1e-310, 1e-305, 1e-200, 1e-160, 1e160, 1e200, 1e300})
    {
        INFO("f64 scale " << scale);
        TtTensor<crd::f64> tt(&alloc);
        const CrossScaleRun<crd::f64> run =
            cross_scale_run<crd::f64>(&alloc, shp, 4U, 4U, 1e-9, 64U, 4U, scale, cross_lin4, tt);
        require_cross_accurate(run, 1e-9, kTtRefCrossGate);
        REQUIRE(run.info.val_error > 0.0);
    }
    for (const crd::f32 scale : {1e-36F, 1e-30F, 1e-19F, 1e19F, 1e30F})
    {
        INFO("f32 scale " << scale);
        TtTensor<crd::f32> tt(&alloc);
        const CrossScaleRun<crd::f32> run =
            cross_scale_run<crd::f32>(&alloc, shp, 4U, 4U, 1e-4F, 64U, 4U, scale, cross_lin4, tt);
        require_cross_accurate(run, 1e-4F, 1e-5);
        REQUIRE(run.info.val_error > 0.0F);
    }
    {
        // f32 subnormal values carry about 17 bits (the smallest subnormal is 4e-6 of the peak here), so the gate is
        // that precision, not the normal-range 1e-5
        TtTensor<crd::f32> tt(&alloc);
        const CrossScaleRun<crd::f32> run =
            cross_scale_run<crd::f32>(&alloc, shp, 4U, 4U, 1e-4F, 64U, 4U, 1e-41F, cross_lin4, tt);
        const crd::f64 grain = static_cast<crd::f64>(std::numeric_limits<crd::f32>::denorm_min()) / run.peak;
        require_cross_accurate(run, 1e-4F, 16.0 * grain);
    }
}

TEST_CASE("tt: tt_cross of f scaled by an exact power of two is the scale-1 TT scaled exactly",
          "[v14k][tt][cross][scale]")
{
    // Householder QR, maxvol and the interpolation cores commute exactly with a power-of-two scaling as long as no
    // value is subnormal, so the range guard must give cores 1..d-1 bit for bit, the first core scaled exactly, and the
    // same validation error. A tol that is never met runs every sweep on both sides.
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 shp[4] = {9U, 9U, 9U, 9U};
    const auto check = [&]<typename T>(T tol, int k)
    {
        INFO("2^" << k);
        TtTensor<T> one(&alloc);
        TtTensor<T> scaled(&alloc);
        const CrossScaleRun<T> a = cross_scale_run<T>(&alloc, shp, 4U, 6U, tol, 64U, 3U, T(1), cross_sin4, one);
        const CrossScaleRun<T> b =
            cross_scale_run<T>(&alloc, shp, 4U, 6U, tol, 64U, 3U, std::ldexp(T(1), k), cross_sin4, scaled);
        REQUIRE(a.ok);
        REQUIRE(b.ok);
        REQUIRE(b.info.sweeps == a.info.sweeps);
        REQUIRE(b.info.val_error == a.info.val_error);
        for (crd::u32 c = 0; c < 4U; ++c)
        {
            REQUIRE(scaled.core_size(c) == one.core_size(c));
            bool same = true;
            for (crd::u64 i = 0; i < one.core_size(c); ++i)
            {
                const T want = c == 0U ? std::ldexp(one.core(c)[i], k) : one.core(c)[i];
                same = same && scaled.core(c)[i] == want;
            }
            REQUIRE(same);
        }
    };
    // scales that keep every value and every product of the evaluation chain normal (subnormals break exactness)
    for (const int k : {-900, -600, -300, 300, 600, 900})
    {
        check(1e-300, k);
    }
    for (const int k : {-100, -80, 80, 100})
    {
        check(1e-30F, k);
    }
}

TEST_CASE("tt: the tt_cross range guard vouches only for a factor it can prove", "[v14k][tt][cross][scale]")
{
    using crd::hesap::tensor::ttdetail::cross_r_vouches;
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::usize rows = 400U;
    const auto vouches = [&](crd::f64 s0, crd::f64 s1)
    {
        crd::hesap::dense::QR<crd::f64> qr(&alloc, rows, 2U);
        crd::hesap::dense::Matrix<crd::f64>& p = qr.packed();
        for (crd::usize i = 0; i < rows; ++i)
        {
            p.at(i, 0) = s0 * std::cos((0.37 * static_cast<crd::f64>(i)) + 0.1);
            p.at(i, 1) = s1 * std::sin((0.11 * static_cast<crd::f64>(i)) + 0.2);
        }
        crd::hesap::dense::factor_qr(qr);
        return cross_r_vouches<crd::f64>(p.data(), p.ld(), qr.taus().data(), rows, 2U);
    };
    REQUIRE(vouches(1.0, 1.0));
    REQUIRE(vouches(1e-100, 1e100));
    REQUIRE(vouches(1.0, 0.0));        // an exactly zero column skips its reflector
    REQUIRE(!vouches(0.0, 0.0));       // nothing to vouch for; the redo leaves a zero block as it is
    REQUIRE(vouches(1e153, 1.0));      // the squares overflow; the QR rescales that column itself
    REQUIRE(!vouches(1.5e307, 1.0));   // the column norm itself leaves the range
    REQUIRE(!vouches(1e-160, 1e-160)); // the squares underflow
    // a column 2^-530 below the other: its R_jj is below the applied-reflector floor (the QR rescales that column
    // itself and stays orthogonal, but this test does not rely on it)
    REQUIRE(!vouches(std::ldexp(1.0, -530), 1.0));
}

TEST_CASE("tt: evaluation is allocation-free and the batch evaluator bit-matches", "[v14k][tt][eval]")
{
    crd::memory::TlsfAllocator tlsf(1U << 26);
    CountingAllocator alloc(&tlsf);
    TtTensor<crd::f64> lr = make_lowrank_tt(&alloc);
    const crd::u64 n = kTtRefLowrankN;
    // index batch (deterministic)
    const crd::u64 npts = 64;
    crd::u64 idx_flat[64 * 4];
    for (crd::u64 p = 0; p < npts; ++p)
    {
        for (crd::u64 k = 0; k < 4U; ++k)
        {
            idx_flat[p * 4U + k] = (p * 7U + k * 3U + p * p) % n;
        }
    }
    crd::f64 work[16];
    REQUIRE(tt_eval_workspace(lr) == 2U * kTtRefLowrankRank);
    crd::f64 out_many[64];
    const crd::u64 allocs_before = alloc.alloc_calls();
    for (crd::u64 p = 0; p < npts; ++p) // single-point path
    {
        out_many[p] = tt_eval<crd::f64>(lr, {idx_flat + p * 4U, 4}, {work, 16});
    }
    crd::f64 out_batch[64];
    REQUIRE(tt_eval_many<crd::f64>(lr, {idx_flat, 64 * 4}, {out_batch, 64}, {work, 16}) == TensorStatus::Ok);
    crd::f64 lerp_probe = 0.0;
    {
        const crd::u64 i0[4] = {1U, 2U, 3U, 4U};
        const crd::f64 w[4] = {0.25, 0.5, 0.75, 0.125};
        lerp_probe = tt_eval_lerp<crd::f64>(lr, {i0, 4}, {w, 4}, {work, 16});
    }
    REQUIRE(alloc.alloc_calls() == allocs_before); // THE allocation-free gate
    REQUIRE(lerp_probe != 0.0);                    // (and the value is used below)
    crd::u64 mism = 0;
    for (crd::u64 p = 0; p < npts; ++p)
    {
        if (std::bit_cast<crd::u64>(out_many[p]) != std::bit_cast<crd::u64>(out_batch[p]))
        {
            ++mism;
        }
    }
    REQUIRE(mism == 0U);
    // tt_eval matches the dense element exactly at machine precision
    Tensor<crd::f64> dense(&alloc);
    REQUIRE(tt_contract<crd::f64>(&alloc, lr, dense) == TensorStatus::Ok);
    for (crd::u64 p = 0; p < npts; ++p)
    {
        const crd::u64* ix = idx_flat + p * 4U;
        const crd::f64 want = dense.data()[((ix[0] * n + ix[1]) * n + ix[2]) * n + ix[3]];
        REQUIRE(std::abs(out_many[p] - want) <= 1e-13);
    }
    // tt_eval_lerp == dense multilinear interpolation (the LUT contract):
    // the interpolation functional is a tensor product of per-dim linear maps
    crd::hesap::stats::PhiloxRng rng(77U, 0U);
    for (crd::u32 trial = 0; trial < 32U; ++trial)
    {
        crd::u64 i0[4];
        crd::f64 w[4];
        for (crd::u64 k = 0; k < 4U; ++k)
        {
            i0[k] = rng.next_below(n - 1U);
            w[k] = rng.next_f64();
        }
        const crd::f64 got = tt_eval_lerp<crd::f64>(lr, {i0, 4}, {w, 4}, {work, 16});
        crd::f64 want = 0.0; // 2^4 corner expansion over the dense grid
        for (crd::u32 c = 0; c < 16U; ++c)
        {
            crd::f64 wt = 1.0;
            crd::u64 ix[4];
            for (crd::u64 k = 0; k < 4U; ++k)
            {
                const bool hi = ((c >> k) & 1U) != 0U;
                wt *= hi ? w[k] : 1.0 - w[k];
                ix[k] = i0[k] + (hi ? 1U : 0U);
            }
            want += wt * dense.data()[((ix[0] * n + ix[1]) * n + ix[2]) * n + ix[3]];
        }
        REQUIRE(std::abs(got - want) <= 1e-12);
    }
    // status adversary: undersized workspace / mismatched batch sizes
    crd::f64 tiny[2];
    REQUIRE(tt_eval_many<crd::f64>(lr, {idx_flat, 64 * 4}, {out_batch, 64}, {tiny, 2}) ==
            TensorStatus::BadInput);
    REQUIRE(tt_eval_many<crd::f64>(lr, {idx_flat, 63U}, {out_batch, 64}, {work, 16}) ==
            TensorStatus::BadInput);
}

TEST_CASE("tt: f32 instantiation smoke - svd/eval/round on a small low-rank tensor", "[v14k][tt][f32]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u64 m = 8U;
    const crd::u64 shp[3] = {m, m, m};
    Tensor<crd::f32> dense(&alloc, {shp, 3});
    for (crd::u64 i = 0; i < m; ++i) // rank-2 construction
    {
        for (crd::u64 j = 0; j < m; ++j)
        {
            for (crd::u64 k = 0; k < m; ++k)
            {
                dense.data()[(i * m + j) * m + k] = static_cast<crd::f32>(
                    hash_val(0U, 0U, i, 0U) * hash_val(1U, 0U, j, 0U) * hash_val(2U, 0U, k, 0U) +
                    hash_val(3U, 1U, i, 0U) * hash_val(4U, 1U, j, 0U) * hash_val(5U, 1U, k, 0U));
            }
        }
    }
    TtTensor<crd::f32> tt(&alloc);
    REQUIRE(tt_svd<crd::f32>(&alloc, dense.view(), 1e-5F, 0U, tt) == TensorStatus::Ok);
    REQUIRE(tt.rank(1) == 2U);
    REQUIRE(tt.rank(2) == 2U);
    crd::f32 work[8];
    const crd::u64 ix[3] = {3U, 1U, 6U};
    const crd::f32 v = tt_eval<crd::f32>(tt, {ix, 3}, {work, 8});
    REQUIRE(std::abs(v - dense.data()[(3U * m + 1U) * m + 6U]) <= 1e-5F);
    REQUIRE(tt_round<crd::f32>(&alloc, tt, 1e-4F, 0U) == TensorStatus::Ok);
    REQUIRE(tt.rank(1) <= 2U);
}

TEST_CASE("tt: boundary adversaries - d=1, rank-1 separable, size-1 modes, statuses", "[v14k][tt][boundary]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // ---- d = 1: a vector is its own TT ------------------------------------
    {
        const crd::u64 shp[1] = {7U};
        Tensor<crd::f64> v(&alloc, {shp, 1});
        for (crd::u64 i = 0; i < 7U; ++i)
        {
            v.data()[i] = hash_val(1U, 2U, i, 3U);
        }
        TtTensor<crd::f64> tt(&alloc);
        REQUIRE(tt_svd<crd::f64>(&alloc, v.view(), 1e-12, 0U, tt) == TensorStatus::Ok);
        REQUIRE(tt.dims() == 1U);
        REQUIRE(tt.rank(1) == 1U);
        crd::f64 work[2];
        for (crd::u64 i = 0; i < 7U; ++i)
        {
            const crd::u64 ix[1] = {i};
            REQUIRE(std::bit_cast<crd::u64>(tt_eval<crd::f64>(tt, {ix, 1}, {work, 2})) ==
                    std::bit_cast<crd::u64>(v.data()[i]));
        }
        TtTensor<crd::f64> sum(&alloc);
        REQUIRE(tt_add<crd::f64>(&alloc, tt, tt, sum) == TensorStatus::Ok);
        const crd::u64 ix0[1] = {3U};
        REQUIRE(std::abs(tt_eval<crd::f64>(sum, {ix0, 1}, {work, 2}) - 2.0 * v.data()[3]) <= 1e-15);
        // cross on d=1 is exact after one sweep (core = raw values)
        TtCrossParams<crd::f64> params;
        params.max_rank = 1U;
        params.max_sweeps = 2U;
        params.tol = 1e-14;
        params.val_size = 16U;
        TtTensor<crd::f64> ct(&alloc);
        TtCrossInfo<crd::f64> info;
        const auto f1 = [&v](crd::containers::ConstSpan<crd::u64> ix) noexcept
        { return v.data()[ix[0]]; };
        REQUIRE(tt_cross<crd::f64>(&alloc, {shp, 1}, f1, params, ct, &info) == TensorStatus::Ok);
        REQUIRE(info.converged);
        REQUIRE(info.sweeps == 1U);
        for (crd::u64 i = 0; i < 7U; ++i)
        {
            const crd::u64 ix[1] = {i};
            REQUIRE(std::bit_cast<crd::u64>(tt_eval<crd::f64>(ct, {ix, 1}, {work, 2})) ==
                    std::bit_cast<crd::u64>(v.data()[i]));
        }
    }
    // ---- rank-1 separable tensor: tt_svd finds ranks (1,1) exactly --------
    {
        const crd::u64 m = 6U;
        const crd::u64 shp[3] = {m, m, m};
        Tensor<crd::f64> dense(&alloc, {shp, 3});
        for (crd::u64 i = 0; i < m; ++i)
        {
            for (crd::u64 j = 0; j < m; ++j)
            {
                for (crd::u64 k = 0; k < m; ++k)
                {
                    dense.data()[(i * m + j) * m + k] =
                        hash_val(0U, 0U, i, 0U) * hash_val(1U, 0U, j, 0U) * hash_val(2U, 0U, k, 0U);
                }
            }
        }
        TtTensor<crd::f64> tt(&alloc);
        REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), 1e-10, 0U, tt) == TensorStatus::Ok);
        REQUIRE(tt.rank(1) == 1U);
        REQUIRE(tt.rank(2) == 1U);
        Tensor<crd::f64> rec(&alloc);
        REQUIRE(tt_contract<crd::f64>(&alloc, tt, rec) == TensorStatus::Ok);
        REQUIRE(rel_fro_err(dense, rec) <= 1e-13);
    }
    // ---- size-1 modes --------------------------------------------------
    {
        const crd::u64 shp[3] = {4U, 1U, 5U};
        Tensor<crd::f64> dense(&alloc, {shp, 3});
        for (crd::u64 i = 0; i < 20U; ++i)
        {
            dense.data()[i] = hash_val(5U, i, i + 1U, 2U);
        }
        TtTensor<crd::f64> tt(&alloc);
        REQUIRE(tt_svd<crd::f64>(&alloc, dense.view(), 1e-12, 0U, tt) == TensorStatus::Ok);
        Tensor<crd::f64> rec(&alloc);
        REQUIRE(tt_contract<crd::f64>(&alloc, tt, rec) == TensorStatus::Ok);
        REQUIRE(rel_fro_err(dense, rec) <= 1e-13);
        crd::f64 work[32];
        REQUIRE(tt_eval_workspace(tt) <= 32U);
        const crd::u64 ix[3] = {2U, 0U, 3U};
        REQUIRE(std::abs(tt_eval<crd::f64>(tt, {ix, 3}, {work, 32}) - dense.data()[(2U * 1U + 0U) * 5U + 3U]) <=
                1e-13);
    }
    // ---- statuses ---------------------------------------------------------
    {
        const crd::u64 shp[2] = {4U, 4U};
        Tensor<crd::f64> dense(&alloc, {shp, 2});
        for (crd::u64 i = 0; i < 16U; ++i)
        {
            dense.data()[i] = static_cast<crd::f64>(i);
        }
        TtTensor<crd::f64> tt(&alloc);
        // non-contiguous view -> NotContiguous
        const TensorView<const crd::f64> flipped = dense.view().flip(0);
        REQUIRE(tt_svd<crd::f64>(&alloc, flipped, 1e-10, 0U, tt) == TensorStatus::NotContiguous);
        // bad init ranks -> BadInput
        const crd::u64 rks_bad[3] = {1U, 3U, 2U};
        REQUIRE(tt.init({shp, 2}, {rks_bad, 3}) == TensorStatus::BadInput);
        // shape mismatch in algebra -> ShapeMismatch
        TtTensor<crd::f64> a(&alloc);
        TtTensor<crd::f64> b(&alloc);
        const crd::u64 shp_a[2] = {4U, 4U};
        const crd::u64 shp_b[2] = {4U, 5U};
        const crd::u64 rks1[3] = {1U, 2U, 1U};
        REQUIRE(a.init({shp_a, 2}, {rks1, 3}) == TensorStatus::Ok);
        REQUIRE(b.init({shp_b, 2}, {rks1, 3}) == TensorStatus::Ok);
        for (crd::u32 k = 0; k < 2U; ++k)
        {
            for (crd::u64 i = 0; i < a.core_size(k); ++i)
            {
                a.core(k)[i] = 0.5;
            }
            for (crd::u64 i = 0; i < b.core_size(k); ++i)
            {
                b.core(k)[i] = 0.5;
            }
        }
        TtTensor<crd::f64> c(&alloc);
        REQUIRE(tt_add<crd::f64>(&alloc, a, b, c) == TensorStatus::ShapeMismatch);
        REQUIRE(tt_hadamard<crd::f64>(&alloc, a, b, c) == TensorStatus::ShapeMismatch);
        crd::f64 dot = 0.0;
        REQUIRE(tt_dot<crd::f64>(&alloc, a, b, dot) == TensorStatus::ShapeMismatch);
    }
}
