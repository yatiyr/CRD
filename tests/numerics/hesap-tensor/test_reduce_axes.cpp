// v14-c close gates: general axis-set reductions.
//   Correctness: integer-valued tensors (every fold order exact in f64) vs a
//   scalar reference reducer for EVERY axes_mask of a rank-4 tensor across all
//   five ops — this covers the VERTICAL / ROW / GENERAL dispatch paths and
//   proves the axis MAPPING NumPy-equivalent. argminmax first-wins ties;
//   cumsum-along-axis vs manual prefix; strided (permuted-view) sources.
//   Moat: {1,2,4,8,16} workers bit-identical to serial for vertical + row.

#include <crd/hesap/tensor/reduce_axes.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>

using namespace crd::hesap::tensor;

namespace
{

// Scalar reference: reduce axes_mask of v by op, canonical logical order.
void ref_reduce(const TensorView<const crd::f64>& v, crd::u32 mask, ReduceOp op, crd::f64* out,
                crd::u64 out_count) noexcept
{
    const crd::u32 r = v.rank();
    crd::u64 red_count = 1;
    for (crd::u32 d = 0; d < r; ++d)
    {
        if ((mask & (1U << d)) != 0U)
        {
            red_count *= v.shape(d);
        }
    }
    bool seen[64] = {}; // small out_counts in this test
    v.for_each(
        [&](const crd::u64* idx, const crd::f64& val)
        {
            crd::u64 o = 0;
            for (crd::u32 d = 0; d < r; ++d)
            {
                if ((mask & (1U << d)) == 0U)
                {
                    o = o * v.shape(d) + idx[d];
                }
            }
            if (!seen[o])
            {
                out[o] = val;
                seen[o] = true;
            }
            else
            {
                switch (op)
                {
                    case ReduceOp::Sum:
                    case ReduceOp::Mean:
                        out[o] += val;
                        break;
                    case ReduceOp::Prod:
                        out[o] *= val;
                        break;
                    case ReduceOp::Min:
                        out[o] = val < out[o] ? val : out[o];
                        break;
                    case ReduceOp::Max:
                        out[o] = out[o] < val ? val : out[o];
                        break;
                }
            }
        });
    if (op == ReduceOp::Mean)
    {
        for (crd::u64 o = 0; o < out_count; ++o)
        {
            out[o] /= static_cast<crd::f64>(red_count);
        }
    }
}

} // namespace

TEST_CASE("v14-c axes: every mask x every op on a rank-4 tensor (exact)", "[hesap][tensor][v14][reduce][axes]")
{
    crd::memory::TlsfAllocator alloc(1U << 22U);
    const crd::u64 shape[] = {3U, 4U, 2U, 5U}; // 120 elements
    Tensor<crd::f64> t(&alloc, shape);
    for (crd::u64 i = 0; i < t.size(); ++i)
    {
        t.data()[i] = static_cast<crd::f64>((i * 37U) % 101U) - 50.0; // integer-valued: exact
    }
    TensorView<const crd::f64> v = t.view();

    // Prod runs on its own small-valued tensor: |products| must stay < 2^53
    // for the exactness gate (50^15 would not).
    Tensor<crd::f64> tp(&alloc, shape);
    for (crd::u64 i = 0; i < tp.size(); ++i)
    {
        const crd::f64 m[4] = {-2.0, -1.0, 1.0, 2.0};
        tp.data()[i] = m[(i * 7U) % 4U];
    }
    TensorView<const crd::f64> vp = tp.view();

    const ReduceOp ops[] = {ReduceOp::Sum, ReduceOp::Prod, ReduceOp::Min, ReduceOp::Max, ReduceOp::Mean};
    for (crd::u32 mask = 1U; mask < 16U; ++mask)
    {
        // kept shape + dst
        crd::u64 kshape[kMaxRank];
        crd::u32 nk = 0;
        crd::u64 outs = 1;
        for (crd::u32 d = 0; d < 4U; ++d)
        {
            if ((mask & (1U << d)) == 0U)
            {
                kshape[nk++] = shape[d];
                outs *= shape[d];
            }
        }
        Tensor<crd::f64> dst(&alloc, {kshape, nk});
        crd::f64 ref[64];
        for (const ReduceOp op : ops)
        {
            const TensorView<const crd::f64>& src = op == ReduceOp::Prod ? vp : v;
            INFO("mask " << mask << " op " << static_cast<int>(op));
            REQUIRE(reduce_axes(op, src, mask, dst.view()) == TensorStatus::Ok);
            ref_reduce(src, mask, op, ref, outs);
            for (crd::u64 o = 0; o < outs; ++o)
            {
                INFO("out " << o);
                CHECK(dst.data()[o] == ref[o]);
            }
        }
    }

    // strided source (permuted view) exercises the GENERAL path for a mask
    // that would otherwise be vertical.
    const crd::u32 order[] = {3U, 1U, 2U, 0U};
    TensorView<const crd::f64> pv = v.permute(order);
    const crd::u64 ks2[] = {4U, 2U, 3U};
    Tensor<crd::f64> d2(&alloc, ks2);
    REQUIRE(reduce_axes(ReduceOp::Sum, pv, 0x1U, d2.view()) == TensorStatus::Ok);
    crd::f64 ref2[24];
    ref_reduce(pv, 0x1U, ReduceOp::Sum, ref2, 24U);
    for (crd::u64 o = 0; o < 24U; ++o)
    {
        CHECK(d2.data()[o] == ref2[o]);
    }

    // status contract
    CHECK(reduce_axes(ReduceOp::Sum, v, 0U, d2.view()) == TensorStatus::BadInput);
    CHECK(reduce_axes(ReduceOp::Sum, v, 0x1U, d2.view()) == TensorStatus::ShapeMismatch);
}

TEST_CASE("v14-c axes: argmin/argmax along an axis (first-wins) + cumsum-axis", "[hesap][tensor][v14][reduce][axes]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    const crd::u64 shape[] = {3U, 5U};
    Tensor<crd::f64> t(&alloc, shape);
    const crd::f64 vals[15] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9};
    for (crd::u64 i = 0; i < 15U; ++i)
    {
        t.data()[i] = vals[i];
    }
    TensorView<const crd::f64> v = t.view();

    const crd::u64 s3[] = {3U};
    Tensor<crd::i64> am(&alloc, s3);
    REQUIRE(reduce_argmin_axis(v, 1U, am.view()) == TensorStatus::Ok);
    CHECK(am.data()[0] == 1); // first '1' at col 1 (tie with col 3 -> first wins)
    CHECK(am.data()[1] == 1);
    CHECK(am.data()[2] == 0);
    REQUIRE(reduce_argmax_axis(v, 1U, am.view()) == TensorStatus::Ok);
    CHECK(am.data()[0] == 4);
    CHECK(am.data()[1] == 0);
    CHECK(am.data()[2] == 2); // 9 at cols 2 and 4 -> first wins

    const crd::u64 s5[] = {5U};
    Tensor<crd::i64> am0(&alloc, s5);
    REQUIRE(reduce_argmax_axis(v, 0U, am0.view()) == TensorStatus::Ok);
    CHECK(am0.data()[0] == 1); // col {3,9,5} -> idx 1
    CHECK(am0.data()[4] == 2); // col {5,3,9} -> idx 2

    // cumsum along both axes vs manual prefix (exact integers)
    Tensor<crd::f64> cs(&alloc, shape);
    REQUIRE(reduce_cumsum_axis(v, 1U, cs.view()) == TensorStatus::Ok);
    for (crd::u64 i = 0; i < 3U; ++i)
    {
        crd::f64 acc = 0.0;
        for (crd::u64 j = 0; j < 5U; ++j)
        {
            acc += vals[i * 5U + j];
            CHECK(cs.data()[i * 5U + j] == acc);
        }
    }
    REQUIRE(reduce_cumsum_axis(v, 0U, cs.view()) == TensorStatus::Ok);
    for (crd::u64 j = 0; j < 5U; ++j)
    {
        crd::f64 acc = 0.0;
        for (crd::u64 i = 0; i < 3U; ++i)
        {
            acc += vals[i * 5U + j];
            CHECK(cs.data()[i * 5U + j] == acc);
        }
    }
}

TEST_CASE("v14-c axes: the {1,2,4,8,16} moat on vertical + row paths", "[hesap][tensor][v14][reduce][axes][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 26U);
    const crd::u64 shape[] = {512U, 8192U}; // vertical: reduce axis 0 -> 8192 outs (2 blocks)
    Tensor<crd::f64> t(&alloc, shape);
    for (crd::u64 i = 0; i < t.size(); ++i)
    {
        t.data()[i] = static_cast<crd::f64>((i * 2654435761ULL) % 100000ULL) * 1e-3 - 50.0;
    }
    TensorView<const crd::f64> v = t.view();

    const crd::u64 sv[] = {8192U};
    const crd::u64 sr[] = {512U};
    Tensor<crd::f64> outv_s(&alloc, sv);
    Tensor<crd::f64> outr_s(&alloc, sr);
    REQUIRE(reduce_axes(ReduceOp::Sum, v, 0x1U, outv_s.view()) == TensorStatus::Ok); // vertical serial
    REQUIRE(reduce_axes(ReduceOp::Sum, v, 0x2U, outr_s.view()) == TensorStatus::Ok); // row serial

    Tensor<crd::f64> outv(&alloc, sv);
    Tensor<crd::f64> outr(&alloc, sr);
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        REQUIRE(reduce_axes(ReduceOp::Sum, v, 0x1U, outv.view()) == TensorStatus::Ok);
        REQUIRE(reduce_axes(ReduceOp::Sum, v, 0x2U, outr.view()) == TensorStatus::Ok);
        crd::jobs::shutdown();
        INFO("workers " << nw);
        for (crd::u64 o = 0; o < 8192U; o += 111U)
        {
            CHECK(std::bit_cast<crd::u64>(outv.data()[o]) == std::bit_cast<crd::u64>(outv_s.data()[o]));
        }
        for (crd::u64 o = 0; o < 512U; ++o)
        {
            CHECK(std::bit_cast<crd::u64>(outr.data()[o]) == std::bit_cast<crd::u64>(outr_s.data()[o]));
        }
    }
}
