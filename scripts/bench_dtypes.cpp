// v14-a dtype convert baseline probe (bench harness for docs/bench; engine TLSF, no malloc).
// Measures ns/element for the scalar bit-exact cores vs the numpy/ml_dtypes peers
// (timed by scripts/bench_dtypes_peers.py at matched element counts).
#include <crd/hesap/tensor/dtypes.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using namespace crd::hesap::tensor;

int main()
{
    constexpr crd::u32 kN = 1U << 20U;
    crd::memory::TlsfAllocator alloc((kN * 8U + (1U << 20U)) * 4U);
    auto* x = static_cast<crd::f32*>(alloc.allocate(kN * sizeof(crd::f32), 64U));
    auto* h = static_cast<crd::u16*>(alloc.allocate(kN * sizeof(crd::u16), 64U));
    auto* b = static_cast<crd::u8*>(alloc.allocate(kN, 64U));
    auto* q8 = static_cast<BlockQ8_0*>(alloc.allocate((kN / 32U) * sizeof(BlockQ8_0), 64U));
    for (crd::u32 i = 0; i < kN; ++i)
    {
        x[i] = 0.001F * static_cast<crd::f32>(i % 9973U) - 3.3F;
    }

    auto bench = [&](const char* name, auto fn)
    {
        fn(); // warm
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int kReps = 20;
        for (int r = 0; r < kReps; ++r)
        {
            fn();
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (kReps * double(kN));
        std::printf("%-22s %8.3f ns/elem\n", name, ns);
    };

    bench("f32->f16 (batch)", [&] { convert_f32_to_f16({x, kN}, {h, kN}); });
    bench("f16->f32 (batch)", [&] { convert_f16_to_f32({h, kN}, {x, kN}); });
    bench("f32->bf16 (batch)", [&] { convert_f32_to_bf16({x, kN}, {h, kN}); });
    bench("f32->e4m3 (batch)", [&] { convert_f32_to_fp8_e4m3({x, kN}, {b, kN}); });
    bench("f32->e5m2 (batch)", [&] { convert_f32_to_fp8_e5m2({x, kN}, {b, kN}); });
    bench("f32->f16 SR (scalar)", [&] { for (crd::u32 i = 0; i < kN; ++i) h[i] = f32_to_f16_bits_sr(x[i], 7U, i); });
    bench("f32->f16 SR (batch)", [&] { convert_f32_to_f16_sr({x, kN}, {h, kN}, 7U); });
    bench("f32->bf16 SR (batch)", [&] { convert_f32_to_bf16_sr({x, kN}, {h, kN}, 7U); });
    bench("f32->e4m3 SR (batch)", [&] { convert_f32_to_fp8_e4m3_sr({x, kN}, {b, kN}, 7U); });
    bench("quantize q8_0", [&] { quantize_q8_0({x, kN}, {q8, kN / 32U}); });
    bench("dequantize q8_0", [&] { dequantize_q8_0({q8, kN / 32U}, {x, kN}); });
    return 0;
}
