// v14-i sparse-tensor gates: oracle .inc corpora (numpy dense-semantics
// reference), CSF==COO bit-identity, the {1,2,4,8,16} worker moat on MTTKRP +
// contraction, and the boundary adversaries (empty / single-nnz /
// dense-as-sparse / duplicate dedup / all-zeros mode slice).
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/sparse.hpp>
#include <crd/hesap/tensor/sparse_mttkrp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>

#include "ref_sparse.inc"

using crd::hesap::tensor::coo_reorder;
using crd::hesap::tensor::coo_to_csf;
using crd::hesap::tensor::contract_mode;
using crd::hesap::tensor::mttkrp;
using crd::hesap::tensor::mttkrp_coo;
using crd::hesap::tensor::reduce_max;
using crd::hesap::tensor::reduce_max_mode;
using crd::hesap::tensor::reduce_max_root;
using crd::hesap::tensor::reduce_sum;
using crd::hesap::tensor::reduce_sum_mode;
using crd::hesap::tensor::reduce_sum_root;
using crd::hesap::tensor::sparse_add;
using crd::hesap::tensor::sparse_add_dense;
using crd::hesap::tensor::sparse_mul;
using crd::hesap::tensor::sparse_mul_dense;
using crd::hesap::tensor::SparseCoo;
using crd::hesap::tensor::SparseCooBuilder;
using crd::hesap::tensor::SparseCsf;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;
using crd::hesap::tensor::to_dense;

namespace
{

[[nodiscard]] crd::f64 fb(crd::u64 bits)
{
    return std::bit_cast<crd::f64>(bits);
}

[[nodiscard]] bool near_rel(crd::f64 a, crd::f64 b, crd::f64 tol)
{
    const crd::f64 d = a > b ? a - b : b - a;
    const crd::f64 ab = b < 0.0 ? -b : b;
    return d <= tol * (ab > 1.0 ? ab : 1.0);
}

// oracle corpus descriptor (raw triplets + canonical expectation)
struct Corpus
{
    const char* name;
    crd::u32 rank;
    const crd::u64* shape;
    crd::u32 raw_nnz;
    const crd::u32* raw_idx; // triplet-major [raw_nnz][rank]
    const crd::u64* raw_val;
    crd::u32 nnz;
    const crd::u32* idx; // mode-major planes [rank][nnz]
    const crd::u64* val;
};

const Corpus kCorpora[] = {
    {"R3a", kSpR3aRank, kSpR3aShape, kSpR3aRawNnz, kSpR3aRawIdx, kSpR3aRawVal, kSpR3aNnz, kSpR3aIdx, kSpR3aVal},
    {"R3b", kSpR3bRank, kSpR3bShape, kSpR3bRawNnz, kSpR3bRawIdx, kSpR3bRawVal, kSpR3bNnz, kSpR3bIdx, kSpR3bVal},
    {"R4a", kSpR4aRank, kSpR4aShape, kSpR4aRawNnz, kSpR4aRawIdx, kSpR4aRawVal, kSpR4aNnz, kSpR4aIdx, kSpR4aVal},
    {"Fro", kSpFroRank, kSpFroShape, kSpFroRawNnz, kSpFroRawIdx, kSpFroRawVal, kSpFroNnz, kSpFroIdx, kSpFroVal},
    {"Dns", kSpDnsRank, kSpDnsShape, kSpDnsRawNnz, kSpDnsRawIdx, kSpDnsRawVal, kSpDnsNnz, kSpDnsIdx, kSpDnsVal},
};

void build_corpus(crd::memory::IAllocator* alloc, const Corpus& c, SparseCoo<crd::f64>& out)
{
    SparseCooBuilder<crd::f64> b(alloc, {c.shape, c.rank});
    b.reserve(c.raw_nnz);
    for (crd::u32 t = 0; t < c.raw_nnz; ++t)
    {
        b.add({c.raw_idx + static_cast<crd::usize>(t) * c.rank, c.rank}, fb(c.raw_val[t]));
    }
    REQUIRE(b.compress(out) == TensorStatus::Ok);
}

// convert an .inc bit array into an owned f64 buffer
void load_bits(crd::containers::Array<crd::f64>& dst, const crd::u64* src, crd::usize n)
{
    dst.resize_uninitialized(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        dst[i] = fb(src[i]);
    }
}

// recursive CSF expansion into level-ordered index planes + values
void expand_csf(const SparseCsf<crd::f64>& c, crd::u32 l, crd::u64 z, crd::u32* stackv, crd::u32* out_planes,
                crd::f64* out_val, crd::u64& w)
{
    stackv[l] = c.fids(l)[z];
    if (l == c.rank() - 1U)
    {
        for (crd::u32 m = 0; m < c.rank(); ++m)
        {
            out_planes[static_cast<crd::usize>(m) * c.nnz() + w] = stackv[m];
        }
        out_val[w] = c.val()[z];
        ++w;
        return;
    }
    for (crd::u32 t = c.fptr(l)[z]; t < c.fptr(l)[z + 1U]; ++t)
    {
        expand_csf(c, l + 1U, t, stackv, out_planes, out_val, w);
    }
}

// run mttkrp for `target` mode of corpus coo: builds the CSF rooted at the
// target, runs the parallel path with num_workers workers, and (optionally)
// the COO reference path; returns both in caller tensors.
void run_mttkrp(crd::memory::IAllocator* alloc, const SparseCoo<crd::f64>& coo, crd::u32 target,
                const TensorView<const crd::f64>* facs, crd::u64 r, Tensor<crd::f64>& out_csf,
                Tensor<crd::f64>& out_coo, crd::u32 num_workers)
{
    const crd::u32 rank = coo.rank();
    crd::u32 order[crd::hesap::tensor::kMaxRank];
    order[0] = target;
    crd::u32 o = 1;
    for (crd::u32 m = 0; m < rank; ++m)
    {
        if (m != target)
        {
            order[o++] = m;
        }
    }
    SparseCsf<crd::f64> csf(alloc);
    REQUIRE(coo_to_csf(coo, {order, rank}, csf) == TensorStatus::Ok);
    const crd::u64 oshape[2] = {coo.shape(target), r};
    out_csf = Tensor<crd::f64>(alloc, {oshape, 2});
    crd::containers::Array<TensorView<const crd::f64>> fspan(alloc);
    fspan.resize(rank);
    for (crd::u32 m = 0; m < rank; ++m)
    {
        fspan[m] = facs[m];
    }
    REQUIRE(mttkrp<crd::f64>(csf, {fspan.data(), rank}, out_csf.view(), alloc, num_workers) == TensorStatus::Ok);
    // COO reference path on the reordered tensor (its mode l = order[l])
    SparseCoo<crd::f64> re(alloc);
    REQUIRE(coo_reorder(coo, {order, rank}, re) == TensorStatus::Ok);
    crd::containers::Array<TensorView<const crd::f64>> fre(alloc);
    fre.resize(rank);
    for (crd::u32 l = 0; l < rank; ++l)
    {
        fre[l] = facs[order[l]];
    }
    out_coo = Tensor<crd::f64>(alloc, {oshape, 2});
    REQUIRE(mttkrp_coo<crd::f64>(re, {fre.data(), rank}, out_coo.view(), alloc) == TensorStatus::Ok);
}

crd::u64 count_bit_mismatch(const crd::f64* a, const crd::f64* b, crd::u64 n)
{
    crd::u64 mism = 0;
    for (crd::u64 i = 0; i < n; ++i)
    {
        if (std::bit_cast<crd::u64>(a[i]) != std::bit_cast<crd::u64>(b[i]))
        {
            ++mism;
        }
    }
    return mism;
}

crd::u64 count_tol_mismatch(const crd::f64* got, const crd::u64* want_bits, crd::u64 n, crd::f64 tol)
{
    crd::u64 mism = 0;
    for (crd::u64 i = 0; i < n; ++i)
    {
        if (!near_rel(got[i], fb(want_bits[i]), tol))
        {
            ++mism;
        }
    }
    return mism;
}

} // namespace

TEST_CASE("sparse: builder dedups and canonicalizes every oracle corpus", "[v14i][sparse]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    for (const Corpus& c : kCorpora)
    {
        INFO(c.name);
        SparseCoo<crd::f64> coo(&alloc);
        build_corpus(&alloc, c, coo);
        REQUIRE(coo.rank() == c.rank);
        REQUIRE(coo.nnz() == c.nnz);
        crd::u64 mism = 0;
        for (crd::u32 m = 0; m < c.rank; ++m)
        {
            for (crd::u32 e = 0; e < c.nnz; ++e)
            {
                if (coo.idx(m)[e] != c.idx[static_cast<crd::usize>(m) * c.nnz + e])
                {
                    ++mism;
                }
            }
        }
        REQUIRE(mism == 0U);
        // duplicate summation is insertion-ordered on BOTH sides -> exact bits
        mism = 0;
        for (crd::u32 e = 0; e < c.nnz; ++e)
        {
            if (std::bit_cast<crd::u64>(coo.val()[e]) != c.val[e])
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("sparse: csf tree mirrors the coo exactly across mode orders", "[v14i][sparse]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const Corpus& c3 = kCorpora[0];
    const Corpus& c4 = kCorpora[2];
    const crd::u32 orders3[][3] = {{0U, 1U, 2U}, {1U, 2U, 0U}, {2U, 0U, 1U}, {2U, 1U, 0U}};
    const crd::u32 orders4[][4] = {{0U, 1U, 2U, 3U}, {2U, 0U, 1U, 3U}, {3U, 2U, 1U, 0U}};
    for (const Corpus* c : {&c3, &c4})
    {
        SparseCoo<crd::f64> coo(&alloc);
        build_corpus(&alloc, *c, coo);
        const crd::u32 rank = c->rank;
        const crd::u32 norders = rank == 3U ? 4U : 3U;
        for (crd::u32 oi = 0; oi < norders; ++oi)
        {
            const crd::u32* order = rank == 3U ? orders3[oi] : orders4[oi];
            INFO(c->name << " order " << oi);
            SparseCsf<crd::f64> csf(&alloc);
            REQUIRE(coo_to_csf(coo, {order, rank}, csf) == TensorStatus::Ok);
            REQUIRE(csf.nnz() == coo.nnz());
            REQUIRE(csf.nodes(rank - 1U) == coo.nnz());
            SparseCoo<crd::f64> re(&alloc);
            REQUIRE(coo_reorder(coo, {order, rank}, re) == TensorStatus::Ok);
            crd::containers::Array<crd::u32> planes(&alloc);
            crd::containers::Array<crd::f64> vals(&alloc);
            planes.resize(static_cast<crd::usize>(rank) * csf.nnz());
            vals.resize(static_cast<crd::usize>(csf.nnz()));
            crd::u32 stackv[crd::hesap::tensor::kMaxRank];
            crd::u64 w = 0;
            for (crd::u64 z = 0; z < csf.nodes(0); ++z)
            {
                expand_csf(csf, 0U, z, stackv, planes.data(), vals.data(), w);
            }
            REQUIRE(w == csf.nnz());
            crd::u64 mism = 0;
            for (crd::u32 l = 0; l < rank; ++l)
            {
                for (crd::u64 e = 0; e < csf.nnz(); ++e)
                {
                    if (planes[static_cast<crd::usize>(l) * csf.nnz() + e] != re.idx(l)[e])
                    {
                        ++mism;
                    }
                }
            }
            mism += count_bit_mismatch(vals.data(), re.val(), csf.nnz());
            REQUIRE(mism == 0U);
        }
    }
}

TEST_CASE("sparse: mttkrp matches the oracle on every corpus and mode with csf-coo bit identity",
          "[v14i][sparse][mttkrp]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    struct MttCase
    {
        const Corpus* c;
        crd::u32 fac_r;
        const crd::u64* facs[4];
        crd::u32 mode;
        const crd::u64* want;
    };
    const MttCase cases[] = {
        {&kCorpora[0], kSpR3aFacR, {kSpR3aFac0, kSpR3aFac1, kSpR3aFac2, nullptr}, 0U, kSpR3aMtt0},
        {&kCorpora[0], kSpR3aFacR, {kSpR3aFac0, kSpR3aFac1, kSpR3aFac2, nullptr}, 1U, kSpR3aMtt1},
        {&kCorpora[0], kSpR3aFacR, {kSpR3aFac0, kSpR3aFac1, kSpR3aFac2, nullptr}, 2U, kSpR3aMtt2},
        {&kCorpora[2], kSpR4aFacR, {kSpR4aFac0, kSpR4aFac1, kSpR4aFac2, kSpR4aFac3}, 0U, kSpR4aMtt0},
        {&kCorpora[2], kSpR4aFacR, {kSpR4aFac0, kSpR4aFac1, kSpR4aFac2, kSpR4aFac3}, 2U, kSpR4aMtt2},
        {&kCorpora[3], kSpFroFacR, {kSpFroFac0, kSpFroFac1, kSpFroFac2, nullptr}, 0U, kSpFroMtt0},
        {&kCorpora[3], kSpFroFacR, {kSpFroFac0, kSpFroFac1, kSpFroFac2, nullptr}, 1U, kSpFroMtt1},
        {&kCorpora[4], kSpDnsFacR, {kSpDnsFac0, kSpDnsFac1, kSpDnsFac2, nullptr}, 0U, kSpDnsMtt0},
    };
    for (const MttCase& mc : cases)
    {
        INFO(mc.c->name << " mode " << mc.mode);
        SparseCoo<crd::f64> coo(&alloc);
        build_corpus(&alloc, *mc.c, coo);
        const crd::u32 rank = mc.c->rank;
        const crd::u64 r = mc.fac_r;
        crd::containers::Array<crd::f64> fbuf[4] = {
            crd::containers::Array<crd::f64>(&alloc), crd::containers::Array<crd::f64>(&alloc),
            crd::containers::Array<crd::f64>(&alloc), crd::containers::Array<crd::f64>(&alloc)};
        TensorView<const crd::f64> facs[4];
        for (crd::u32 m = 0; m < rank; ++m)
        {
            load_bits(fbuf[m], mc.facs[m], static_cast<crd::usize>(mc.c->shape[m] * r));
            const crd::u64 fshape[2] = {mc.c->shape[m], r};
            facs[m] = TensorView<const crd::f64>::contiguous(fbuf[m].data(), {fshape, 2});
        }
        Tensor<crd::f64> out_csf;
        Tensor<crd::f64> out_coo;
        run_mttkrp(&alloc, coo, mc.mode, facs, r, out_csf, out_coo, 1U);
        const crd::u64 osz = mc.c->shape[mc.mode] * r;
        REQUIRE(count_tol_mismatch(out_csf.data(), mc.want, osz, 1e-14) == 0U);
        REQUIRE(count_bit_mismatch(out_csf.data(), out_coo.data(), osz) == 0U);
    }
}

TEST_CASE("sparse: mode-n contraction matches the oracle including the scatter mode-0 path",
          "[v14i][sparse][ttm]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    struct TtmCase
    {
        const Corpus* c;
        crd::u32 mode;
        crd::u32 f;
        const crd::u64* u;
        const crd::u64* want;
    };
    const TtmCase cases[] = {
        {&kCorpora[0], 0U, kSpR3aTtmF0, kSpR3aTtmU0, kSpR3aTtm0},
        {&kCorpora[0], 1U, kSpR3aTtmF1, kSpR3aTtmU1, kSpR3aTtm1},
        {&kCorpora[2], 3U, kSpR4aTtmF3, kSpR4aTtmU3, kSpR4aTtm3},
        {&kCorpora[4], 2U, kSpDnsTtmF2, kSpDnsTtmU2, kSpDnsTtm2},
    };
    for (const TtmCase& tc : cases)
    {
        INFO(tc.c->name << " mode " << tc.mode);
        SparseCoo<crd::f64> coo(&alloc);
        build_corpus(&alloc, *tc.c, coo);
        const crd::u32 rank = tc.c->rank;
        crd::containers::Array<crd::f64> ubuf(&alloc);
        load_bits(ubuf, tc.u, static_cast<crd::usize>(tc.c->shape[tc.mode]) * tc.f);
        const crd::u64 ushape[2] = {tc.c->shape[tc.mode], tc.f};
        const TensorView<const crd::f64> u =
            TensorView<const crd::f64>::contiguous(ubuf.data(), {ushape, 2});
        crd::u64 oshape[crd::hesap::tensor::kMaxRank];
        crd::u64 osz = 1;
        for (crd::u32 m = 0; m < rank; ++m)
        {
            oshape[m] = m == tc.mode ? tc.f : tc.c->shape[m];
            osz *= oshape[m];
        }
        Tensor<crd::f64> out(&alloc, {oshape, rank});
        REQUIRE(contract_mode<crd::f64>(coo, tc.mode, u, out.view(), &alloc, 1U) == TensorStatus::Ok);
        REQUIRE(count_tol_mismatch(out.data(), tc.want, osz, 1e-14) == 0U);
    }
}

TEST_CASE("sparse: elementwise add mul and dense hadamard match the oracle exactly", "[v14i][sparse][ew]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    SparseCoo<crd::f64> a(&alloc);
    SparseCoo<crd::f64> b(&alloc);
    build_corpus(&alloc, kCorpora[0], a);
    build_corpus(&alloc, kCorpora[1], b);
    const crd::u32 rank = a.rank();
    // union add
    {
        SparseCoo<crd::f64> s(&alloc);
        REQUIRE(sparse_add(a, b, s) == TensorStatus::Ok);
        REQUIRE(s.nnz() == kSpEwAddNnz);
        crd::u64 mism = 0;
        for (crd::u32 m = 0; m < rank; ++m)
        {
            for (crd::u32 e = 0; e < kSpEwAddNnz; ++e)
            {
                if (s.idx(m)[e] != kSpEwAddIdx[static_cast<crd::usize>(m) * kSpEwAddNnz + e])
                {
                    ++mism;
                }
            }
        }
        for (crd::u32 e = 0; e < kSpEwAddNnz; ++e)
        {
            if (std::bit_cast<crd::u64>(s.val()[e]) != kSpEwAddVal[e])
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
    // intersection mul
    {
        SparseCoo<crd::f64> p(&alloc);
        REQUIRE(sparse_mul(a, b, p) == TensorStatus::Ok);
        REQUIRE(p.nnz() == kSpEwMulNnz);
        crd::u64 mism = 0;
        for (crd::u32 m = 0; m < rank; ++m)
        {
            for (crd::u32 e = 0; e < kSpEwMulNnz; ++e)
            {
                if (p.idx(m)[e] != kSpEwMulIdx[static_cast<crd::usize>(m) * kSpEwMulNnz + e])
                {
                    ++mism;
                }
            }
        }
        for (crd::u32 e = 0; e < kSpEwMulNnz; ++e)
        {
            if (std::bit_cast<crd::u64>(p.val()[e]) != kSpEwMulVal[e])
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
    // sparse x dense hadamard + sparse + dense -> dense
    crd::containers::Array<crd::f64> dbuf(&alloc);
    const crd::u64 dsize = a.size();
    load_bits(dbuf, kSpEwDenseD, static_cast<crd::usize>(dsize));
    const TensorView<const crd::f64> d =
        TensorView<const crd::f64>::contiguous(dbuf.data(), a.shape());
    {
        SparseCoo<crd::f64> h(&alloc);
        REQUIRE(sparse_mul_dense(a, d, h) == TensorStatus::Ok);
        REQUIRE(h.nnz() == a.nnz());
        crd::u64 mism = 0;
        for (crd::u64 e = 0; e < h.nnz(); ++e)
        {
            if (std::bit_cast<crd::u64>(h.val()[e]) != kSpEwMulDenseVal[e])
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
    {
        Tensor<crd::f64> out(&alloc, a.shape());
        REQUIRE(sparse_add_dense(a, d, out.view()) == TensorStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < dsize; ++i)
        {
            if (std::bit_cast<crd::u64>(out.data()[i]) != kSpEwAddDenseOut[i])
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
        // to_dense round trip: out - d must equal the scatter of a
        Tensor<crd::f64> dn(&alloc, a.shape());
        REQUIRE(to_dense(a, dn.view()) == TensorStatus::Ok);
        crd::u64 nz = 0;
        for (crd::u64 i = 0; i < dsize; ++i)
        {
            if (dn.data()[i] != 0.0)
            {
                ++nz;
            }
        }
        REQUIRE(nz == a.nnz());
    }
}

TEST_CASE("sparse: reductions match the oracle with dense semantics", "[v14i][sparse][reduce]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // R3a: totals + every mode
    {
        SparseCoo<crd::f64> a(&alloc);
        build_corpus(&alloc, kCorpora[0], a);
        crd::f64 s = 0.0;
        REQUIRE(reduce_sum(a, s) == TensorStatus::Ok);
        REQUIRE(near_rel(s, fb(kSpR3aSumTotal[0]), 1e-14));
        crd::f64 mx = 0.0;
        REQUIRE(reduce_max(a, mx) == TensorStatus::Ok);
        REQUIRE(std::bit_cast<crd::u64>(mx) == kSpR3aMaxTotal[0]);
        const crd::u64* sums[3] = {kSpR3aSumMode0, kSpR3aSumMode1, kSpR3aSumMode2};
        const crd::u64* maxs[3] = {kSpR3aMaxMode0, kSpR3aMaxMode1, kSpR3aMaxMode2};
        for (crd::u32 m = 0; m < 3U; ++m)
        {
            INFO("mode " << m);
            crd::containers::Array<crd::f64> out(&alloc);
            out.resize(static_cast<crd::usize>(a.shape(m)));
            REQUIRE(reduce_sum_mode(a, m, {out.data(), out.size()}) == TensorStatus::Ok);
            REQUIRE(count_tol_mismatch(out.data(), sums[m], a.shape(m), 1e-14) == 0U);
            REQUIRE(reduce_max_mode(a, m, {out.data(), out.size()}, &alloc) == TensorStatus::Ok);
            crd::u64 mism = 0;
            for (crd::u64 i = 0; i < a.shape(m); ++i)
            {
                if (std::bit_cast<crd::u64>(out[static_cast<crd::usize>(i)]) != maxs[m][i])
                {
                    ++mism;
                }
            }
            REQUIRE(mism == 0U);
            // CSF root-mode reductions (the precomputed-boundary path):
            // root the tree at mode m, gate against the same oracle rows
            crd::u32 order[3] = {m, m == 0U ? 1U : 0U, m == 2U ? 1U : 2U};
            SparseCsf<crd::f64> csf(&alloc);
            REQUIRE(coo_to_csf(a, {order, 3U}, csf) == TensorStatus::Ok);
            REQUIRE(reduce_sum_root(csf, {out.data(), out.size()}) == TensorStatus::Ok);
            REQUIRE(count_tol_mismatch(out.data(), sums[m], a.shape(m), 1e-14) == 0U);
            REQUIRE(reduce_max_root(csf, {out.data(), out.size()}) == TensorStatus::Ok);
            mism = 0;
            for (crd::u64 i = 0; i < a.shape(m); ++i)
            {
                if (std::bit_cast<crd::u64>(out[static_cast<crd::usize>(i)]) != maxs[m][i])
                {
                    ++mism;
                }
            }
            REQUIRE(mism == 0U);
            crd::f64 st = 0.0;
            REQUIRE(reduce_sum(csf, st) == TensorStatus::Ok);
            REQUIRE(near_rel(st, fb(kSpR3aSumTotal[0]), 1e-14));
        }
    }
    // R4a: total + selected modes
    {
        SparseCoo<crd::f64> a(&alloc);
        build_corpus(&alloc, kCorpora[2], a);
        crd::f64 s = 0.0;
        REQUIRE(reduce_sum(a, s) == TensorStatus::Ok);
        REQUIRE(near_rel(s, fb(kSpR4aSumTotal[0]), 1e-14));
        crd::containers::Array<crd::f64> out(&alloc);
        out.resize(static_cast<crd::usize>(a.shape(1)));
        REQUIRE(reduce_sum_mode(a, 1U, {out.data(), out.size()}) == TensorStatus::Ok);
        REQUIRE(count_tol_mismatch(out.data(), kSpR4aSumMode1, a.shape(1), 1e-14) == 0U);
        out.resize(static_cast<crd::usize>(a.shape(2)));
        REQUIRE(reduce_max_mode(a, 2U, {out.data(), out.size()}, &alloc) == TensorStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < a.shape(2); ++i)
        {
            if (std::bit_cast<crd::u64>(out[static_cast<crd::usize>(i)]) != kSpR4aMaxMode2[i])
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
    // Dns: all-negative full cube -> max_total is negative (no implicit zeros)
    {
        SparseCoo<crd::f64> a(&alloc);
        build_corpus(&alloc, kCorpora[4], a);
        crd::f64 mx = 0.0;
        REQUIRE(reduce_max(a, mx) == TensorStatus::Ok);
        REQUIRE(std::bit_cast<crd::u64>(mx) == kSpDnsMaxTotal[0]);
        REQUIRE(mx < 0.0);
        crd::containers::Array<crd::f64> out(&alloc);
        out.resize(static_cast<crd::usize>(a.shape(0)));
        REQUIRE(reduce_max_mode(a, 0U, {out.data(), out.size()}, &alloc) == TensorStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < a.shape(0); ++i)
        {
            if (std::bit_cast<crd::u64>(out[static_cast<crd::usize>(i)]) != kSpDnsMaxMode0[i])
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("sparse: all-zeros mode-0 slice stays zero through mttkrp and reductions", "[v14i][sparse]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    SparseCoo<crd::f64> a(&alloc);
    build_corpus(&alloc, kCorpora[3], a); // Fro: mode-0 index 7 has NO entries
    for (crd::u64 e = 0; e < a.nnz(); ++e)
    {
        REQUIRE(a.idx(0)[e] != 7U);
    }
    // reductions: slice 7 exactly zero on both boards
    crd::containers::Array<crd::f64> out(&alloc);
    out.resize(static_cast<crd::usize>(a.shape(0)));
    REQUIRE(reduce_sum_mode(a, 0U, {out.data(), out.size()}) == TensorStatus::Ok);
    REQUIRE(out[7] == 0.0);
    REQUIRE(count_tol_mismatch(out.data(), kSpFroSumMode0, a.shape(0), 1e-14) == 0U);
    REQUIRE(reduce_max_mode(a, 0U, {out.data(), out.size()}, &alloc) == TensorStatus::Ok);
    REQUIRE(out[7] == 0.0);
    crd::u64 mism = 0;
    for (crd::u64 i = 0; i < a.shape(0); ++i)
    {
        if (std::bit_cast<crd::u64>(out[static_cast<crd::usize>(i)]) != kSpFroMaxMode0[i])
        {
            ++mism;
        }
    }
    REQUIRE(mism == 0U);
    // CSF root reductions: the absent root fiber's slot stays exactly zero
    {
        const crd::u32 orderf[3] = {0U, 1U, 2U};
        SparseCsf<crd::f64> csf(&alloc);
        REQUIRE(coo_to_csf(a, {orderf, 3U}, csf) == TensorStatus::Ok);
        REQUIRE(reduce_sum_root(csf, {out.data(), out.size()}) == TensorStatus::Ok);
        REQUIRE(out[7] == 0.0);
        REQUIRE(count_tol_mismatch(out.data(), kSpFroSumMode0, a.shape(0), 1e-14) == 0U);
        REQUIRE(reduce_max_root(csf, {out.data(), out.size()}) == TensorStatus::Ok);
        REQUIRE(out[7] == 0.0);
    }
    // mttkrp: output row 7 is exactly zero (the oracle gate in the mttkrp case
    // already covers the values; this pins the empty-fiber row bits)
    crd::containers::Array<crd::f64> fbuf[3] = {crd::containers::Array<crd::f64>(&alloc),
                                                crd::containers::Array<crd::f64>(&alloc),
                                                crd::containers::Array<crd::f64>(&alloc)};
    TensorView<const crd::f64> facs[3];
    const crd::u64* srcs[3] = {kSpFroFac0, kSpFroFac1, kSpFroFac2};
    for (crd::u32 m = 0; m < 3U; ++m)
    {
        load_bits(fbuf[m], srcs[m], static_cast<crd::usize>(a.shape(m)) * kSpFroFacR);
        const crd::u64 fshape[2] = {a.shape(m), kSpFroFacR};
        facs[m] = TensorView<const crd::f64>::contiguous(fbuf[m].data(), {fshape, 2});
    }
    Tensor<crd::f64> oc;
    Tensor<crd::f64> oo;
    run_mttkrp(&alloc, a, 0U, facs, kSpFroFacR, oc, oo, 1U);
    for (crd::u64 q = 0; q < kSpFroFacR; ++q)
    {
        REQUIRE(std::bit_cast<crd::u64>(oc.data()[7U * kSpFroFacR + q]) == 0U);
    }
}

TEST_CASE("sparse: the 1 2 4 8 16 worker moat on mttkrp and contraction", "[v14i][sparse][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    // philox-generated random rank-3 tensor, big enough to fan out tasks
    const crd::u64 shape[3] = {60U, 50U, 40U};
    SparseCooBuilder<crd::f64> b(&alloc, {shape, 3});
    crd::hesap::stats::PhiloxRng rng(20260705U, 5U);
    b.reserve(8000U);
    for (crd::u32 t = 0; t < 8000U; ++t)
    {
        const crd::u32 idx[3] = {rng.next_u32() % 60U, rng.next_u32() % 50U, rng.next_u32() % 40U};
        b.add({idx, 3U}, 2.0 * rng.next_f64() - 1.0);
    }
    SparseCoo<crd::f64> coo(&alloc);
    REQUIRE(b.compress(coo) == TensorStatus::Ok);
    const crd::u64 r = 16;
    crd::containers::Array<crd::f64> fbuf[3] = {crd::containers::Array<crd::f64>(&alloc),
                                                crd::containers::Array<crd::f64>(&alloc),
                                                crd::containers::Array<crd::f64>(&alloc)};
    TensorView<const crd::f64> facs[3];
    for (crd::u32 m = 0; m < 3U; ++m)
    {
        fbuf[m].resize_uninitialized(static_cast<crd::usize>(shape[m] * r));
        for (crd::usize i = 0; i < fbuf[m].size(); ++i)
        {
            fbuf[m][i] = 2.0 * rng.next_f64() - 1.0;
        }
        const crd::u64 fshape[2] = {shape[m], r};
        facs[m] = TensorView<const crd::f64>::contiguous(fbuf[m].data(), {fshape, 2});
    }
    // serial baselines (workers = 1)
    Tensor<crd::f64> mtt_ser;
    Tensor<crd::f64> mtt_coo;
    run_mttkrp(&alloc, coo, 0U, facs, r, mtt_ser, mtt_coo, 1U);
    REQUIRE(count_bit_mismatch(mtt_ser.data(), mtt_coo.data(), shape[0] * r) == 0U); // csf==coo at scale
    const crd::u64 t1shape[3] = {60U, 16U, 40U};
    Tensor<crd::f64> ttm1_ser(&alloc, {t1shape, 3});
    REQUIRE(contract_mode<crd::f64>(coo, 1U, facs[1], ttm1_ser.view(), &alloc, 1U) == TensorStatus::Ok);
    const crd::u64 t0shape[3] = {16U, 50U, 40U};
    Tensor<crd::f64> ttm0_ser(&alloc, {t0shape, 3});
    REQUIRE(contract_mode<crd::f64>(coo, 0U, facs[0], ttm0_ser.view(), &alloc, 1U) == TensorStatus::Ok);
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        Tensor<crd::f64> mtt_par;
        Tensor<crd::f64> mtt_coo2;
        run_mttkrp(&alloc, coo, 0U, facs, r, mtt_par, mtt_coo2, 0U);
        Tensor<crd::f64> ttm1_par(&alloc, {t1shape, 3});
        const TensorStatus s1 = contract_mode<crd::f64>(coo, 1U, facs[1], ttm1_par.view(), &alloc, 0U);
        Tensor<crd::f64> ttm0_par(&alloc, {t0shape, 3});
        const TensorStatus s0 = contract_mode<crd::f64>(coo, 0U, facs[0], ttm0_par.view(), &alloc, 0U);
        crd::jobs::shutdown();
        REQUIRE(s1 == TensorStatus::Ok);
        REQUIRE(s0 == TensorStatus::Ok);
        INFO("workers " << nw);
        REQUIRE(count_bit_mismatch(mtt_par.data(), mtt_ser.data(), shape[0] * r) == 0U);
        REQUIRE(count_bit_mismatch(ttm1_par.data(), ttm1_ser.data(), 60U * 16U * 40U) == 0U);
        REQUIRE(count_bit_mismatch(ttm0_par.data(), ttm0_ser.data(), 16U * 50U * 40U) == 0U);
    }
}

TEST_CASE("sparse: f32 kernels keep the csf-coo bit identity", "[v14i][sparse]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u64 shape[3] = {12U, 9U, 7U};
    SparseCooBuilder<crd::f32> b(&alloc, {shape, 3});
    crd::hesap::stats::PhiloxRng rng(7U, 3U);
    for (crd::u32 t = 0; t < 300U; ++t)
    {
        const crd::u32 idx[3] = {rng.next_u32() % 12U, rng.next_u32() % 9U, rng.next_u32() % 7U};
        b.add({idx, 3U}, static_cast<crd::f32>(2.0 * rng.next_f64() - 1.0));
    }
    SparseCoo<crd::f32> coo(&alloc);
    REQUIRE(b.compress(coo) == TensorStatus::Ok);
    const crd::u64 r = 10; // exercises the masked Vec8f tail (10 = 8 + 2)
    crd::containers::Array<crd::f32> fbuf[3] = {crd::containers::Array<crd::f32>(&alloc),
                                                crd::containers::Array<crd::f32>(&alloc),
                                                crd::containers::Array<crd::f32>(&alloc)};
    TensorView<const crd::f32> facs[3];
    for (crd::u32 m = 0; m < 3U; ++m)
    {
        fbuf[m].resize_uninitialized(static_cast<crd::usize>(shape[m] * r));
        for (crd::usize i = 0; i < fbuf[m].size(); ++i)
        {
            fbuf[m][i] = static_cast<crd::f32>(2.0 * rng.next_f64() - 1.0);
        }
        const crd::u64 fshape[2] = {shape[m], r};
        facs[m] = TensorView<const crd::f32>::contiguous(fbuf[m].data(), {fshape, 2});
    }
    const crd::u32 order[3] = {0U, 1U, 2U};
    SparseCsf<crd::f32> csf(&alloc);
    REQUIRE(coo_to_csf(coo, {order, 3U}, csf) == TensorStatus::Ok);
    const crd::u64 oshape[2] = {12U, r};
    Tensor<crd::f32> oc(&alloc, {oshape, 2});
    Tensor<crd::f32> oo(&alloc, {oshape, 2});
    crd::containers::Array<TensorView<const crd::f32>> fspan(&alloc);
    fspan.resize(3U);
    for (crd::u32 m = 0; m < 3U; ++m)
    {
        fspan[m] = facs[m];
    }
    REQUIRE(mttkrp<crd::f32>(csf, {fspan.data(), 3U}, oc.view(), &alloc, 1U) == TensorStatus::Ok);
    REQUIRE(mttkrp_coo<crd::f32>(coo, {fspan.data(), 3U}, oo.view(), &alloc) == TensorStatus::Ok);
    crd::u64 mism = 0;
    for (crd::u64 i = 0; i < 12U * r; ++i)
    {
        if (std::bit_cast<crd::u32>(oc.data()[i]) != std::bit_cast<crd::u32>(oo.data()[i]))
        {
            ++mism;
        }
    }
    REQUIRE(mism == 0U);
}

TEST_CASE("sparse: boundary adversaries - empty single-nnz duplicates and rejections", "[v14i][sparse]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u64 shape[3] = {5U, 4U, 3U};
    // empty tensor
    {
        SparseCooBuilder<crd::f64> b(&alloc, {shape, 3});
        SparseCoo<crd::f64> coo(&alloc);
        REQUIRE(b.compress(coo) == TensorStatus::Ok);
        REQUIRE(coo.nnz() == 0U);
        const crd::u32 order[3] = {0U, 1U, 2U};
        SparseCsf<crd::f64> csf(&alloc);
        REQUIRE(coo_to_csf(coo, {order, 3U}, csf) == TensorStatus::Ok);
        REQUIRE(csf.nodes(0) == 0U);
        crd::containers::Array<crd::f64> fbuf(&alloc);
        fbuf.resize(5U * 4U); // max dim * r zeros are fine for empty factors
        const crd::u64 r = 4;
        TensorView<const crd::f64> facs[3];
        for (crd::u32 m = 0; m < 3U; ++m)
        {
            const crd::u64 fshape[2] = {shape[m], r};
            facs[m] = TensorView<const crd::f64>::contiguous(fbuf.data(), {fshape, 2});
        }
        crd::containers::Array<TensorView<const crd::f64>> fspan(&alloc);
        fspan.resize(3U);
        for (crd::u32 m = 0; m < 3U; ++m)
        {
            fspan[m] = facs[m];
        }
        const crd::u64 oshape[2] = {5U, r};
        Tensor<crd::f64> out(&alloc, {oshape, 2});
        REQUIRE(mttkrp<crd::f64>(csf, {fspan.data(), 3U}, out.view(), &alloc, 1U) == TensorStatus::Ok);
        for (crd::u64 i = 0; i < 5U * r; ++i)
        {
            REQUIRE(std::bit_cast<crd::u64>(out.data()[i]) == 0U);
        }
        crd::f64 s = -1.0;
        REQUIRE(reduce_sum(coo, s) == TensorStatus::Ok);
        REQUIRE(s == 0.0);
        crd::f64 mx = -1.0;
        REQUIRE(reduce_max(coo, mx) == TensorStatus::Ok); // implicit zeros exist
        REQUIRE(mx == 0.0);
        SparseCoo<crd::f64> sum(&alloc);
        REQUIRE(sparse_add(coo, coo, sum) == TensorStatus::Ok);
        REQUIRE(sum.nnz() == 0U);
        SparseCoo<crd::f64> prod(&alloc);
        REQUIRE(sparse_mul(coo, coo, prod) == TensorStatus::Ok);
        REQUIRE(prod.nnz() == 0U);
    }
    // zero-size tensor: max is undefined (numpy errors) -> BadInput
    {
        const crd::u64 zshape[2] = {0U, 3U};
        SparseCooBuilder<crd::f64> b(&alloc, {zshape, 2});
        SparseCoo<crd::f64> coo(&alloc);
        REQUIRE(b.compress(coo) == TensorStatus::Ok);
        crd::f64 mx = 0.0;
        REQUIRE(reduce_max(coo, mx) == TensorStatus::BadInput);
    }
    // single nonzero, negative value: max sees the implicit zeros
    {
        SparseCooBuilder<crd::f64> b(&alloc, {shape, 3});
        const crd::u32 idx[3] = {2U, 1U, 2U};
        b.add({idx, 3U}, -0.75);
        SparseCoo<crd::f64> coo(&alloc);
        REQUIRE(b.compress(coo) == TensorStatus::Ok);
        REQUIRE(coo.nnz() == 1U);
        crd::f64 s = 0.0;
        REQUIRE(reduce_sum(coo, s) == TensorStatus::Ok);
        REQUIRE(s == -0.75);
        crd::f64 mx = -1.0;
        REQUIRE(reduce_max(coo, mx) == TensorStatus::Ok);
        REQUIRE(mx == 0.0);
        // mttkrp of one nnz: row 2 = v * (F1 row 1) hadamard (F2 row 2)
        const crd::u64 r = 4;
        crd::containers::Array<crd::f64> fbuf[3] = {crd::containers::Array<crd::f64>(&alloc),
                                                    crd::containers::Array<crd::f64>(&alloc),
                                                    crd::containers::Array<crd::f64>(&alloc)};
        TensorView<const crd::f64> facs[3];
        crd::hesap::stats::PhiloxRng rng(9U, 1U);
        for (crd::u32 m = 0; m < 3U; ++m)
        {
            fbuf[m].resize_uninitialized(static_cast<crd::usize>(shape[m] * r));
            for (crd::usize i = 0; i < fbuf[m].size(); ++i)
            {
                fbuf[m][i] = 2.0 * rng.next_f64() - 1.0;
            }
            const crd::u64 fshape[2] = {shape[m], r};
            facs[m] = TensorView<const crd::f64>::contiguous(fbuf[m].data(), {fshape, 2});
        }
        Tensor<crd::f64> oc;
        Tensor<crd::f64> oo;
        run_mttkrp(&alloc, coo, 0U, facs, r, oc, oo, 1U);
        for (crd::u64 q = 0; q < r; ++q)
        {
            const crd::f64 want = -0.75 * fbuf[2][2U * r + q] * fbuf[1][1U * r + q];
            REQUIRE(near_rel(oc.data()[2U * r + q], want, 1e-15));
        }
        for (crd::u64 i = 0; i < r; ++i) // untouched rows exactly zero
        {
            REQUIRE(std::bit_cast<crd::u64>(oc.data()[i]) == 0U);
        }
    }
    // duplicate triplets: exact insertion-order dedup sum
    {
        SparseCooBuilder<crd::f64> b(&alloc, {shape, 3});
        const crd::u32 i0[3] = {1U, 1U, 1U};
        const crd::u32 i1[3] = {0U, 0U, 0U};
        b.add({i0, 3U}, 1.0);
        b.add({i1, 3U}, 2.0);
        b.add({i0, 3U}, 1.0);
        b.add({i0, 3U}, 1.0);
        SparseCoo<crd::f64> coo(&alloc);
        REQUIRE(b.compress(coo) == TensorStatus::Ok);
        REQUIRE(coo.nnz() == 2U);
        REQUIRE(coo.val()[0] == 2.0);
        REQUIRE(coo.val()[1] == 3.0);
        REQUIRE(coo.idx(0)[0] == 0U);
        REQUIRE(coo.idx(0)[1] == 1U);
    }
    // status rejections
    {
        SparseCooBuilder<crd::f64> b(&alloc, {shape, 3});
        const crd::u32 idx[3] = {1U, 2U, 1U};
        b.add({idx, 3U}, 1.5);
        SparseCoo<crd::f64> coo(&alloc);
        REQUIRE(b.compress(coo) == TensorStatus::Ok);
        const crd::u32 bad_order[3] = {0U, 0U, 2U};
        SparseCsf<crd::f64> csf(&alloc);
        REQUIRE(coo_to_csf(coo, {bad_order, 3U}, csf) == TensorStatus::BadInput);
        const crd::u32 order[3] = {0U, 1U, 2U};
        REQUIRE(coo_to_csf(coo, {order, 3U}, csf) == TensorStatus::Ok);
        const crd::u64 r = 4;
        crd::containers::Array<crd::f64> fbuf(&alloc);
        fbuf.resize(4U * 5U);
        crd::containers::Array<TensorView<const crd::f64>> fspan(&alloc);
        fspan.resize(3U);
        for (crd::u32 m = 0; m < 3U; ++m)
        {
            const crd::u64 fshape[2] = {shape[m], r};
            fspan[m] = TensorView<const crd::f64>::contiguous(fbuf.data(), {fshape, 2});
        }
        const crd::u64 oshape[2] = {5U, r};
        Tensor<crd::f64> out(&alloc, {oshape, 2});
        // wrong factor width
        {
            crd::containers::Array<TensorView<const crd::f64>> fbad(&alloc);
            fbad.resize(3U);
            for (crd::u32 m = 0; m < 3U; ++m)
            {
                const crd::u64 fshape[2] = {shape[m], r + 1U};
                fbad[m] = TensorView<const crd::f64>::contiguous(fbuf.data(), {fshape, 2});
            }
            REQUIRE(mttkrp<crd::f64>(csf, {fbad.data(), 3U}, out.view(), &alloc, 1U) ==
                    TensorStatus::ShapeMismatch);
        }
        // null scratch
        REQUIRE(mttkrp<crd::f64>(csf, {fspan.data(), 3U}, out.view(), nullptr, 1U) == TensorStatus::BadInput);
        // contraction: mode out of range + shape mismatch
        const crd::u64 ushape[2] = {4U, 3U};
        const TensorView<const crd::f64> u = TensorView<const crd::f64>::contiguous(fbuf.data(), {ushape, 2});
        const crd::u64 tshape[3] = {5U, 3U, 3U};
        Tensor<crd::f64> tout(&alloc, {tshape, 3});
        REQUIRE(contract_mode<crd::f64>(coo, 3U, u, tout.view(), &alloc, 1U) == TensorStatus::BadInput);
        REQUIRE(contract_mode<crd::f64>(coo, 1U, u, tout.view(), &alloc, 1U) == TensorStatus::Ok);
        const crd::u64 wshape[3] = {5U, 2U, 3U};
        Tensor<crd::f64> wout(&alloc, {wshape, 3});
        REQUIRE(contract_mode<crd::f64>(coo, 1U, u, wout.view(), &alloc, 1U) == TensorStatus::ShapeMismatch);
        // elementwise shape mismatch
        const crd::u64 shape2[3] = {5U, 4U, 2U};
        SparseCooBuilder<crd::f64> b2(&alloc, {shape2, 3});
        SparseCoo<crd::f64> coo2(&alloc);
        REQUIRE(b2.compress(coo2) == TensorStatus::Ok);
        SparseCoo<crd::f64> so(&alloc);
        REQUIRE(sparse_add(coo, coo2, so) == TensorStatus::ShapeMismatch);
        REQUIRE(sparse_mul(coo, coo2, so) == TensorStatus::ShapeMismatch);
        // reduce span-size mismatch
        crd::containers::Array<crd::f64> rout(&alloc);
        rout.resize(3U);
        REQUIRE(reduce_sum_mode(coo, 0U, {rout.data(), 3U}) == TensorStatus::BadInput);
    }
}
