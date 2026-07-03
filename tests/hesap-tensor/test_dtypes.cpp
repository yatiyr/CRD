// v14-a dtype gates (ADR-0096 §2/§4):
//   - f16/bf16/fp8-e4m3fn/e5m2 converts BIT-EXACT vs the ml_dtypes reference
//     corpus (scripts/v14a_dtypes_corpus.py; NaN payloads compared as
//     sign+class, everything else exact bits) + exhaustive fp8 decode +
//     encode(decode) idempotence.
//   - ggml Q8_0/Q4_0 block quantization BYTE-EXACT vs the transcribed
//     quantize_row_*_ref reference vectors (weights interop, v14-m on-ramp).
//   - ★Deterministic SR: neighbor validity, unbiasedness, run-twice
//     bit-identity, and stride/chunk independence (the canonical-index key).

#include "ref_dtypes.inc"

#include <crd/hesap/tensor/dtypes.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>

using namespace crd::hesap::tensor;

namespace
{

[[nodiscard]] bool f32_is_nan_bits(crd::u32 b)
{
    return ((b >> 23U) & 0xFFU) == 0xFFU && (b & 0x7FFFFFU) != 0U;
}
[[nodiscard]] bool f16_is_nan(crd::u16 h)
{
    return (h & 0x7C00U) == 0x7C00U && (h & 0x3FFU) != 0U;
}
[[nodiscard]] bool bf16_is_nan(crd::u16 h)
{
    return (h & 0x7F80U) == 0x7F80U && (h & 0x7FU) != 0U;
}
[[nodiscard]] bool e4m3_is_nan(crd::u8 b)
{
    return (b & 0x7FU) == 0x7FU;
}
[[nodiscard]] bool e5m2_is_nan(crd::u8 b)
{
    return (b & 0x7CU) == 0x7CU && (b & 0x3U) != 0U;
}

// Exact-bits comparison except NaNs, which compare as (sign, is-nan) class —
// reference NaN payloads are implementation detail (stated in the corpus).
template <typename Bits, typename NanPred>
[[nodiscard]] bool bits_match(Bits mine, Bits ref, crd::u32 sign_shift, NanPred is_nan)
{
    if (mine == ref)
    {
        return true;
    }
    return is_nan(mine) && is_nan(ref) && (mine >> sign_shift) == (ref >> sign_shift);
}

[[nodiscard]] bool f32_match(crd::u32 mine, crd::u32 ref)
{
    if (mine == ref)
    {
        return true;
    }
    return f32_is_nan_bits(mine) && f32_is_nan_bits(ref) && (mine >> 31U) == (ref >> 31U);
}

} // namespace

TEST_CASE("v14-a dtypes: f32 narrowing is bit-exact vs the ml_dtypes corpus", "[hesap][tensor][v14][dtypes]")
{
    for (crd::u32 i = 0; i < kRefSrcCount; ++i)
    {
        const crd::f32 x = std::bit_cast<crd::f32>(kRefSrcF32[i]);
        INFO("i=" << i << " f32 bits=0x" << std::hex << kRefSrcF32[i]);
        CHECK(bits_match<crd::u16>(f32_to_f16_bits(x), kRefTo_f16[i], 15U, f16_is_nan));
        CHECK(bits_match<crd::u16>(f32_to_bf16_bits(x), kRefTo_bf16[i], 15U, bf16_is_nan));
        CHECK(bits_match<crd::u8>(f32_to_fp8_e4m3_bits(x), kRefTo_e4m3[i], 7U, e4m3_is_nan));
        CHECK(bits_match<crd::u8>(f32_to_fp8_e5m2_bits(x), kRefTo_e5m2[i], 7U, e5m2_is_nan));
    }
}

TEST_CASE("v14-a dtypes: exhaustive fp8 decode + encode(decode) idempotence", "[hesap][tensor][v14][dtypes]")
{
    for (crd::u32 p = 0; p < 256U; ++p)
    {
        const auto b = static_cast<crd::u8>(p);
        {
            const crd::f32 v = fp8_e4m3_bits_to_f32(b);
            INFO("e4m3 pattern 0x" << std::hex << p);
            CHECK(f32_match(std::bit_cast<crd::u32>(v), kRefFrom_e4m3[p]));
            if (!f32_is_nan_bits(std::bit_cast<crd::u32>(v)))
            {
                CHECK(f32_to_fp8_e4m3_bits(v) == b); // every finite value encodes back to itself
            }
        }
        {
            const crd::f32 v = fp8_e5m2_bits_to_f32(b);
            INFO("e5m2 pattern 0x" << std::hex << p);
            CHECK(f32_match(std::bit_cast<crd::u32>(v), kRefFrom_e5m2[p]));
            if (!f32_is_nan_bits(std::bit_cast<crd::u32>(v)))
            {
                CHECK(f32_to_fp8_e5m2_bits(v) == b);
            }
        }
    }
}

TEST_CASE("v14-a dtypes: f16/bf16 widening matches the reference corpus", "[hesap][tensor][v14][dtypes]")
{
    for (crd::u32 i = 0; i < kRefHalfCount; ++i)
    {
        {
            const crd::u16 h = kRefPat_f16[i];
            const crd::f32 v = f16_bits_to_f32(h);
            INFO("f16 pattern 0x" << std::hex << h);
            CHECK(f32_match(std::bit_cast<crd::u32>(v), kRefVal_f16[i]));
            if (!f32_is_nan_bits(std::bit_cast<crd::u32>(v)))
            {
                CHECK(f32_to_f16_bits(v) == h);
            }
        }
        {
            const crd::u16 h = kRefPat_bf16[i];
            const crd::f32 v = bf16_bits_to_f32(h);
            INFO("bf16 pattern 0x" << std::hex << h);
            CHECK(f32_match(std::bit_cast<crd::u32>(v), kRefVal_bf16[i]));
            if (!f32_is_nan_bits(std::bit_cast<crd::u32>(v)))
            {
                CHECK(f32_to_bf16_bits(v) == h);
            }
        }
    }
}

TEST_CASE("v14-a dtypes: ggml Q8_0/Q4_0 quantization is byte-exact vs the reference", "[hesap][tensor][v14][dtypes]")
{
    crd::f32 src[128];
    for (crd::u32 i = 0; i < 128U; ++i)
    {
        src[i] = std::bit_cast<crd::f32>(kGgmlSrcF32[i]);
    }

    BlockQ8_0 q8[4];
    quantize_q8_0({src, 128U}, {q8, 4U});
    for (crd::u32 b = 0; b < 4U; ++b)
    {
        INFO("q8 block " << b);
        CHECK(q8[b].d == kGgmlQ8Scale[b]);
        for (crd::u32 j = 0; j < 32U; ++j)
        {
            CHECK(static_cast<crd::u8>(q8[b].qs[j]) == kGgmlQ8Qs[b * 32U + j]);
        }
    }
    // Dequantize agrees with the ggml formula and bounds the block error by d/2.
    crd::f32 back[128];
    dequantize_q8_0({q8, 4U}, {back, 128U});
    for (crd::u32 b = 0; b < 4U; ++b)
    {
        const crd::f32 d = f16_bits_to_f32(q8[b].d);
        for (crd::u32 j = 0; j < 32U; ++j)
        {
            CHECK(back[b * 32U + j] == static_cast<crd::f32>(q8[b].qs[j]) * d);
            const crd::f32 err = back[b * 32U + j] - src[b * 32U + j];
            CHECK((err < 0 ? -err : err) <= d * 0.51F + 1e-6F);
        }
    }

    BlockQ4_0 q4[4];
    quantize_q4_0({src, 128U}, {q4, 4U});
    for (crd::u32 b = 0; b < 4U; ++b)
    {
        INFO("q4 block " << b);
        CHECK(q4[b].d == kGgmlQ4Scale[b]);
        for (crd::u32 j = 0; j < 16U; ++j)
        {
            CHECK(q4[b].qs[j] == kGgmlQ4Qs[b * 16U + j]);
        }
    }
    crd::f32 back4[128];
    dequantize_q4_0({q4, 4U}, {back4, 128U});
    for (crd::u32 b = 0; b < 4U; ++b)
    {
        const crd::f32 d = f16_bits_to_f32(q4[b].d);
        for (crd::u32 j = 0; j < 16U; ++j)
        {
            CHECK(back4[b * 32U + j] == static_cast<crd::f32>(static_cast<crd::i32>(q4[b].qs[j] & 0x0FU) - 8) * d);
        }
    }
}

TEST_CASE("v14-a dtypes: deterministic SR - validity, unbiasedness, bit-identity", "[hesap][tensor][v14][dtypes]")
{
    constexpr crd::u64 seed = 0xC0FFEE0123456789ULL;

    // Validity: every SR result is one of the two truncation neighbors
    // (rand=0 IS truncation; the up-neighbor is +1 in the contiguous encoding).
    const crd::f32 samples[] = {1.0003F, -1.0003F, 0.10007F, 3.14159F, -777.7F, 6.1e-5F, 1.0e-7F, 42.42F};
    for (const crd::f32 x : samples)
    {
        const crd::u16 rd = f32_to_f16_bits_sr(x, seed, 0U);
        for (crd::u64 idx = 1; idx < 64U; ++idx)
        {
            const crd::u16 r = f32_to_f16_bits_sr(x, seed, idx);
            const crd::u16 mag_r = static_cast<crd::u16>(r & 0x7FFFU);
            const crd::u16 mag_d = static_cast<crd::u16>(rd & 0x7FFFU);
            const crd::u16 lo = mag_r < mag_d ? mag_r : mag_d;
            const crd::u16 hi = mag_r < mag_d ? mag_d : mag_r;
            CHECK(static_cast<crd::u32>(hi - lo) <= 1U); // same or adjacent representable magnitude
        }
    }

    // Unbiasedness: E[f16_sr(x)] == x. 1.0 + 0.3·2^-10 rounds up with p≈0.3.
    {
        const crd::f32 x = 1.0F + 0.3F * 0.0009765625F;
        crd::f64 acc = 0.0;
        constexpr crd::u32 count = 40000U;
        for (crd::u64 i = 0; i < count; ++i)
        {
            acc += static_cast<crd::f64>(f16_bits_to_f32(f32_to_f16_bits_sr(x, seed, i)));
        }
        const crd::f64 mean = acc / count;
        // 4σ window: σ_mean = ulp·sqrt(p(1−p)/N) ≈ 2.24e-6
        CHECK(mean > static_cast<crd::f64>(x) - 9.0e-6);
        CHECK(mean < static_cast<crd::f64>(x) + 9.0e-6);
    }

    // Run-twice bit-identity + per-format smoke of the SR entry points.
    for (crd::u64 i = 0; i < 32U; ++i)
    {
        const crd::f32 x = 0.37F * static_cast<crd::f32>(i + 1U);
        CHECK(f32_to_f16_bits_sr(x, seed, i) == f32_to_f16_bits_sr(x, seed, i));
        CHECK(f32_to_bf16_bits_sr(x, seed, i) == f32_to_bf16_bits_sr(x, seed, i));
        CHECK(f32_to_fp8_e4m3_bits_sr(x, seed, i) == f32_to_fp8_e4m3_bits_sr(x, seed, i));
        CHECK(f32_to_fp8_e5m2_bits_sr(x, seed, i) == f32_to_fp8_e5m2_bits_sr(x, seed, i));
    }

    // SR never produces a non-finite from a finite input (saturation semantics).
    CHECK((f32_to_f16_bits_sr(65519.9F, seed, 7U) & 0x7FFFU) <= 0x7BFFU);
    CHECK((f32_to_fp8_e5m2_bits_sr(57343.9F, seed, 7U) & 0x7FU) <= 0x7BU);
    CHECK((f32_to_fp8_e4m3_bits_sr(447.9F, seed, 7U) & 0x7FU) <= 0x7EU);
}

TEST_CASE("v14-a dtypes: batch (SIMD) converts are bit-identical to scalar", "[hesap][tensor][v14][dtypes]")
{
    // Corpus + a deterministic Philox-driven random-bit-pattern sweep covering
    // every f32 class (the SIMD==scalar contract on ALL inputs, NaN payloads included).
    constexpr crd::u32 rand_count = 100000U;
    constexpr crd::u32 count = kRefSrcCount + rand_count;
    static crd::f32 src[count];
    for (crd::u32 i = 0; i < kRefSrcCount; ++i)
    {
        src[i] = std::bit_cast<crd::f32>(kRefSrcF32[i]);
    }
    for (crd::u32 i = 0; i < rand_count; ++i)
    {
        // full 32-bit patterns: hits denormals/inf/nan/every exponent
        src[kRefSrcCount + i] = std::bit_cast<crd::f32>(crd::hesap::tensor::detail::sr_draw(0xB17E5ULL, i, 99U));
    }

    static crd::u16 h_batch[count];
    static crd::u8 b_batch[count];
    convert_f32_to_f16({src, count}, {h_batch, count});
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (h_batch[i] != f32_to_f16_bits(src[i]))
        {
            INFO("f16 mismatch at " << i << " bits=0x" << std::hex << std::bit_cast<crd::u32>(src[i]));
            REQUIRE(h_batch[i] == f32_to_f16_bits(src[i]));
        }
    }
    static crd::f32 w_batch[count];
    convert_f16_to_f32({h_batch, count}, {w_batch, count});
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (std::bit_cast<crd::u32>(w_batch[i]) != std::bit_cast<crd::u32>(f16_bits_to_f32(h_batch[i])))
        {
            INFO("f16->f32 mismatch at " << i);
            REQUIRE(std::bit_cast<crd::u32>(w_batch[i]) == std::bit_cast<crd::u32>(f16_bits_to_f32(h_batch[i])));
        }
    }
    convert_f32_to_bf16({src, count}, {h_batch, count});
    for (crd::u32 i = 0; i < count; ++i)
    {
        REQUIRE(h_batch[i] == f32_to_bf16_bits(src[i]));
    }
    convert_f32_to_fp8_e4m3({src, count}, {b_batch, count});
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (b_batch[i] != f32_to_fp8_e4m3_bits(src[i]))
        {
            INFO("e4m3 mismatch at " << i << " bits=0x" << std::hex << std::bit_cast<crd::u32>(src[i]));
            REQUIRE(b_batch[i] == f32_to_fp8_e4m3_bits(src[i]));
        }
    }
    convert_f32_to_fp8_e5m2({src, count}, {b_batch, count});
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (b_batch[i] != f32_to_fp8_e5m2_bits(src[i]))
        {
            INFO("e5m2 mismatch at " << i << " bits=0x" << std::hex << std::bit_cast<crd::u32>(src[i]));
            REQUIRE(b_batch[i] == f32_to_fp8_e5m2_bits(src[i]));
        }
    }

    // Batch SR (AVX2 Philox draws + SIMD SR narrowers) ≡ the per-element scalar
    // wrappers, bit-for-bit — count is deliberately NOT a multiple of the chunk or
    // vector width, so tails and chunk seams are exercised.
    constexpr crd::u64 sr_seed = 0xFEEDFACE12345678ULL;
    convert_f32_to_f16_sr({src, count}, {h_batch, count}, sr_seed);
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (h_batch[i] != f32_to_f16_bits_sr(src[i], sr_seed, i))
        {
            INFO("f16 SR mismatch at " << i << " bits=0x" << std::hex << std::bit_cast<crd::u32>(src[i]));
            REQUIRE(h_batch[i] == f32_to_f16_bits_sr(src[i], sr_seed, i));
        }
    }
    convert_f32_to_bf16_sr({src, count}, {h_batch, count}, sr_seed);
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (h_batch[i] != f32_to_bf16_bits_sr(src[i], sr_seed, i))
        {
            INFO("bf16 SR mismatch at " << i << " bits=0x" << std::hex << std::bit_cast<crd::u32>(src[i]));
            REQUIRE(h_batch[i] == f32_to_bf16_bits_sr(src[i], sr_seed, i));
        }
    }
    convert_f32_to_fp8_e4m3_sr({src, count}, {b_batch, count}, sr_seed);
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (b_batch[i] != f32_to_fp8_e4m3_bits_sr(src[i], sr_seed, i))
        {
            INFO("e4m3 SR mismatch at " << i << " bits=0x" << std::hex << std::bit_cast<crd::u32>(src[i]));
            REQUIRE(b_batch[i] == f32_to_fp8_e4m3_bits_sr(src[i], sr_seed, i));
        }
    }
    convert_f32_to_fp8_e5m2_sr({src, count}, {b_batch, count}, sr_seed);
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (b_batch[i] != f32_to_fp8_e5m2_bits_sr(src[i], sr_seed, i))
        {
            INFO("e5m2 SR mismatch at " << i << " bits=0x" << std::hex << std::bit_cast<crd::u32>(src[i]));
            REQUIRE(b_batch[i] == f32_to_fp8_e5m2_bits_sr(src[i], sr_seed, i));
        }
    }
}

TEST_CASE("v14-a StorageTensor: strided converts + SR chunk/stride independence", "[hesap][tensor][v14][dtypes]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const crd::u64 shape[] = {4U, 6U};
    Tensor<crd::f32> t(&alloc, shape);
    for (crd::u64 i = 0; i < 24U; ++i)
    {
        t.data()[i] = 0.123F * static_cast<crd::f32>(i) - 1.1F;
    }

    // RNE round-trip through a PERMUTED (strided) source view.
    const crd::u32 order[] = {1U, 0U};
    TensorView<const crd::f32> pv = t.view().permute(order); // 6x4 strided
    StorageTensor<StorageDtype::F16> h(&alloc);
    REQUIRE(h.convert_from(pv) == TensorStatus::Ok);

    Tensor<crd::f32> back(&alloc, h.shape());
    REQUIRE(h.convert_to(back.view()) == TensorStatus::Ok);
    crd::u64 k = 0;
    pv.for_each(
        [&](const crd::u64*, const crd::f32& v)
        {
            CHECK(back.data()[k] == f16_bits_to_f32(f32_to_f16_bits(v)));
            ++k;
        });

    // SR key = canonical destination index ⇒ the strided source and its
    // materialized contiguous copy produce IDENTICAL bits.
    constexpr crd::u64 seed = 42U;
    StorageTensor<StorageDtype::Bf16> s1(&alloc);
    REQUIRE(s1.convert_from_sr(pv, seed) == TensorStatus::Ok);

    Tensor<crd::f32> mat(&alloc, h.shape()); // materialize the permuted view
    crd::u64 m = 0;
    pv.for_each([&](const crd::u64*, const crd::f32& v) { mat.data()[m++] = v; });
    StorageTensor<StorageDtype::Bf16> s2(&alloc);
    TensorView<const crd::f32> mv = mat.view();
    REQUIRE(s2.convert_from_sr(mv, seed) == TensorStatus::Ok);

    for (crd::u64 i = 0; i < 24U; ++i)
    {
        CHECK(s1.bits()[i] == s2.bits()[i]);
    }

    // Shape mismatch on convert_to is a status, never UB.
    const crd::u64 bad[] = {3U, 8U};
    Tensor<crd::f32> wrong(&alloc, bad);
    CHECK(h.convert_to(wrong.view()) == TensorStatus::ShapeMismatch);
}
