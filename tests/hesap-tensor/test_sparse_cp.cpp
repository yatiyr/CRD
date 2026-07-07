// v14-i x v14-j integration glue gates: sparse CP-ALS through the MTTKRP seam.
// The seam-parity gate runs cp_als_generic twice on the SAME pre-initialized
// factors — once through DenseMttkrp, once through the CSF functor on the
// dense-as-sparse twin — and requires matching fits (not bitwise: the two
// kernels order their accumulations differently by design).
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/sparse_cp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>

using crd::hesap::tensor::CpInfo;
using crd::hesap::tensor::CpOptions;
using crd::hesap::tensor::DecompStatus;
using crd::hesap::tensor::DenseMttkrp;
using crd::hesap::tensor::SparseCoo;
using crd::hesap::tensor::SparseCooBuilder;
using crd::hesap::tensor::SparseCpMttkrp;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;
using crd::hesap::tensor::cp_als_generic;
using crd::hesap::tensor::cp_als_sparse;

namespace
{

void init_factors(crd::containers::Span<Tensor<crd::f64>> factors, const crd::u64* shape, crd::u32 nd,
                  crd::u64 rank, crd::memory::IAllocator* alloc)
{
    for (crd::u32 m = 0; m < nd; ++m)
    {
        const crd::u64 fs[2] = {shape[m], rank};
        factors[m] = Tensor<crd::f64>(alloc, {fs, 2U});
        crd::hesap::stats::PhiloxRng rng(97U, m);
        for (crd::u64 e = 0; e < shape[m] * rank; ++e)
        {
            factors[m].data()[e] = rng.next_f64() + 0.1;
        }
    }
}

} // namespace

TEST_CASE("sparse-cp: the seam parity gate - CSF functor matches DenseMttkrp fits", "[v14i][v14j][sparse][cp]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u64 shape[3] = {12U, 10U, 8U};
    const crd::u64 rank = 4U;
    // dense random tensor + its sparse twin
    Tensor<crd::f64> x(&alloc, {shape, 3U});
    crd::hesap::stats::PhiloxRng rng(91U, 0U);
    for (crd::u64 e = 0; e < 12U * 10U * 8U; ++e)
    {
        x.data()[e] = 2.0 * rng.next_f64() - 1.0;
    }
    SparseCooBuilder<crd::f64> bld(&alloc, {shape, 3U});
    for (crd::u32 i = 0; i < 12U; ++i)
    {
        for (crd::u32 j = 0; j < 10U; ++j)
        {
            for (crd::u32 k = 0; k < 8U; ++k)
            {
                const crd::u32 idx[3] = {i, j, k};
                bld.add({idx, 3U}, x.data()[(i * 10U + j) * 8U + k]);
            }
        }
    }
    SparseCoo<crd::f64> coo(&alloc);
    REQUIRE(bld.compress(coo) == TensorStatus::Ok);

    crd::f64 nrm2 = 0.0;
    for (crd::u64 e = 0; e < 12U * 10U * 8U; ++e)
    {
        nrm2 = std::fma(x.data()[e], x.data()[e], nrm2);
    }
    CpOptions<crd::f64> opts;
    opts.max_iters = 25U;
    opts.tol = 0.0; // fixed budget: identical iteration counts both paths

    Tensor<crd::f64> fd[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    Tensor<crd::f64> fs[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
    init_factors({fd, 3U}, shape, 3U, rank, &alloc);
    init_factors({fs, 3U}, shape, 3U, rank, &alloc);
    crd::f64 wd[4];
    crd::f64 ws[4];
    CpInfo<crd::f64> id;
    CpInfo<crd::f64> is;

    DenseMttkrp<crd::f64> dm(TensorView<const crd::f64>(x.view()), &alloc);
    REQUIRE(cp_als_generic<crd::f64>({shape, 3U}, nrm2, dm, rank, {fd, 3U}, {wd, 4U}, id, &alloc, opts) ==
            DecompStatus::Ok);
    SparseCpMttkrp<crd::f64> sm(coo, &alloc);
    REQUIRE(cp_als_generic<crd::f64>({shape, 3U}, nrm2, sm, rank, {fs, 3U}, {ws, 4U}, is, &alloc, opts) ==
            DecompStatus::Ok);
    REQUIRE(id.iters == is.iters);
    REQUIRE(std::fabs(id.fit - is.fit) < 1e-9);
}

TEST_CASE("sparse-cp: cp_als_sparse runs, converges direction, and is run-twice bit-identical",
          "[v14i][v14j][sparse][cp]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u64 shape[3] = {30U, 20U, 10U};
    const crd::u64 rank = 6U;
    // random sparse tensor: 600 nnz
    SparseCooBuilder<crd::f64> bld(&alloc, {shape, 3U});
    crd::hesap::stats::PhiloxRng rng(93U, 0U);
    for (crd::u32 t = 0; t < 600U; ++t)
    {
        const crd::u32 idx[3] = {static_cast<crd::u32>(rng.next_below(30U)),
                                 static_cast<crd::u32>(rng.next_below(20U)),
                                 static_cast<crd::u32>(rng.next_below(10U))};
        bld.add({idx, 3U}, 2.0 * rng.next_f64() - 1.0);
    }
    SparseCoo<crd::f64> coo(&alloc);
    REQUIRE(bld.compress(coo) == TensorStatus::Ok);

    CpOptions<crd::f64> opts;
    opts.max_iters = 15U;
    opts.tol = 0.0;
    crd::f64 fit1 = -1.0;
    crd::u64 fbits[3] = {};
    for (int round = 0; round < 2; ++round)
    {
        Tensor<crd::f64> f[3] = {Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc), Tensor<crd::f64>(&alloc)};
        init_factors({f, 3U}, shape, 3U, rank, &alloc);
        crd::f64 w[6];
        CpInfo<crd::f64> info;
        REQUIRE(cp_als_sparse<crd::f64>(coo, rank, {f, 3U}, {w, 6U}, info, &alloc, opts) == DecompStatus::Ok);
        REQUIRE(info.fit >= 0.0);
        REQUIRE(info.fit <= 1.0);
        if (round == 0)
        {
            fit1 = info.fit;
            for (crd::u32 m = 0; m < 3U; ++m)
            {
                fbits[m] = std::bit_cast<crd::u64>(f[m].data()[0]);
            }
        }
        else
        {
            REQUIRE(std::bit_cast<crd::u64>(fit1) == std::bit_cast<crd::u64>(info.fit));
            for (crd::u32 m = 0; m < 3U; ++m)
            {
                REQUIRE(std::bit_cast<crd::u64>(f[m].data()[0]) == fbits[m]);
            }
        }
    }
}
