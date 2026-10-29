// v14-a dtype convert baseline probe (bench harness for docs/bench; engine TLSF, no malloc).
// Measures ns/element for the scalar bit-exact cores vs the numpy/ml_dtypes peers
// (timed by scripts/bench_dtypes_peers.py at matched element counts).
// The batches run the crd-math conversion and hesap-tensor kernel levels the process selected (CRD_CPU_LEVEL forces
// one, scripts/run_dtype_bench.sh runs each). After each timed row its output must give the bits of the portable
// level's kernel over the same input, the SR batches those of the per-element scalar converts (the pinned keying),
// or the run exits 1. dequantize_q8_0 is a scalar loop at every level.
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/tensor/dtypes.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/math/float_convert.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/construct.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace crd::hesap::tensor;

namespace
{
constexpr crd::u32 kN = 1U << 20U;
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc((kN * 8U + (1U << 20U)) * 8U);
    const char* const oom = "bench_dtypes: the bench arena is out of memory";
    auto* x = crd::memory::allocate_array_or_fatal<crd::f32>(alloc, kN, 64U, oom);
    auto* h = crd::memory::allocate_array_or_fatal<crd::u16>(alloc, kN, 64U, oom);
    auto* b = crd::memory::allocate_array_or_fatal<crd::u8>(alloc, kN, 64U, oom);
    auto* q8 = crd::memory::allocate_array_or_fatal<BlockQ8Zero>(alloc, kN / 32U, 64U, oom);
    auto* want32 = crd::memory::allocate_array_or_fatal<crd::f32>(alloc, kN, 64U, oom);
    auto* want16 = crd::memory::allocate_array_or_fatal<crd::u16>(alloc, kN, 64U, oom);
    auto* want8 = crd::memory::allocate_array_or_fatal<crd::u8>(alloc, kN, 64U, oom);
    auto* want_q8 = crd::memory::allocate_array_or_fatal<BlockQ8Zero>(alloc, kN / 32U, 64U, oom);
    for (crd::u32 i = 0; i < kN; ++i)
    {
        x[i] = 0.001F * static_cast<crd::f32>(i % 9973U) - 3.3F;
    }
    const crd::math::FloatConvertKernels& convert = *crd::math::float_convert_levels()[0].table;
    const TensorKernels& tensor = *tensor_kernel_levels()[0].table;
    std::printf("conversion kernel level %s, hesap-tensor kernel level %s (selected %s)\n",
                crd::cpu_level_name(crd::math::float_convert_kernels().level),
                crd::cpu_level_name(tensor_kernels().level), crd::cpu_level_name(crd::cpu_selection().selected));

    bool ok = true;
    // The row's output against the reference written by want(): the same bytes, or a miss fails the run.
    auto bench = [&](const char* name, auto fn, const void* got, const void* expected, crd::usize bytes, auto want)
    {
        fn(); // warm
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int reps = 20;
        for (int r = 0; r < reps; ++r)
        {
            fn();
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (reps * double(kN));
        std::printf("%-22s %8.3f ns/elem\n", name, ns);
        want();
        if (bytes != 0U && std::memcmp(got, expected, bytes) != 0)
        {
            std::printf("!MISMATCH! %s: not the reference bits\n", name);
            ok = false;
        }
    };
    const auto none = [] {};

    bench(
        "f32->f16 (batch)", [&] { convert_f32_to_f16({x, kN}, {h, kN}); }, h, want16, kN * sizeof(crd::u16),
        [&] { convert.f32_to_f16(x, want16, kN); });
    bench(
        "f16->f32 (batch)", [&] { convert_f16_to_f32({h, kN}, {x, kN}); }, x, want32, kN * sizeof(crd::f32),
        [&] { convert.f16_to_f32(h, want32, kN); });
    bench(
        "f32->bf16 (batch)", [&] { convert_f32_to_bf16({x, kN}, {h, kN}); }, h, want16, kN * sizeof(crd::u16),
        [&] { convert.f32_to_bf16(x, want16, kN); });
    bench(
        "f32->e4m3 (batch)", [&] { convert_f32_to_fp8_e4m3({x, kN}, {b, kN}); }, b, want8, kN,
        [&] { convert.f32_to_fp8_e4m3(x, want8, kN); });
    bench(
        "f32->e5m2 (batch)", [&] { convert_f32_to_fp8_e5m2({x, kN}, {b, kN}); }, b, want8, kN,
        [&] { convert.f32_to_fp8_e5m2(x, want8, kN); });
    bench(
        "f32->f16 SR (scalar)",
        [&]
        {
            for (crd::u32 i = 0; i < kN; ++i)
            {
                h[i] = f32_to_f16_bits_sr(x[i], 7U, i);
            }
        },
        nullptr, nullptr, 0U, none);
    bench(
        "f32->f16 SR (batch)", [&] { convert_f32_to_f16_sr({x, kN}, {h, kN}, 7U); }, h, want16, kN * sizeof(crd::u16),
        [&]
        {
            for (crd::u32 i = 0; i < kN; ++i)
            {
                want16[i] = f32_to_f16_bits_sr(x[i], 7U, i);
            }
        });
    bench(
        "f32->bf16 SR (batch)", [&] { convert_f32_to_bf16_sr({x, kN}, {h, kN}, 7U); }, h, want16, kN * sizeof(crd::u16),
        [&]
        {
            for (crd::u32 i = 0; i < kN; ++i)
            {
                want16[i] = f32_to_bf16_bits_sr(x[i], 7U, i);
            }
        });
    bench(
        "f32->e4m3 SR (batch)", [&] { convert_f32_to_fp8_e4m3_sr({x, kN}, {b, kN}, 7U); }, b, want8, kN,
        [&]
        {
            for (crd::u32 i = 0; i < kN; ++i)
            {
                want8[i] = f32_to_fp8_e4m3_bits_sr(x[i], 7U, i);
            }
        });
    bench(
        "quantize q8_0", [&] { quantize_q8_0({x, kN}, {q8, kN / 32U}); }, q8, want_q8, kN / 32U * sizeof(BlockQ8Zero),
        [&] { tensor.quantize_q8_0(x, want_q8, kN / 32U); });
    bench("dequantize q8_0", [&] { dequantize_q8_0({q8, kN / 32U}, {x, kN}); }, nullptr, nullptr, 0U, none);
    return ok ? 0 : 1;
}
