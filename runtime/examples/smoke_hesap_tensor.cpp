// smoke_hesap_tensor — v14-a headless smoke + THE LINK-ISOLATION GATE
// (ADR-0096 §1): this executable links ONLY crd-hesap-tensor's public closure
// (core/containers/memory/math/jobs + platform — io.hpp's fs seam, declared at
// CI-1 2026-07-23) — deliberately NOT crd-hesap-stats (Philox is include-only)
// nor hesap-dense/sparse. If a future edit link-drags any of them into the
// substrate, THIS TARGET FAILS TO LINK — that is the test.
#include <crd/hesap/tensor/dtypes.hpp>
#include <crd/hesap/tensor/tensor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>

using namespace crd::hesap::tensor;

int main()
{
    crd::memory::TlsfAllocator alloc(1U << 22U);

    // Views: build 2x3x4, permute+slice, spot-check the stride math.
    const crd::u64 shape[] = {2U, 3U, 4U};
    Tensor<crd::f32> t(&alloc, shape);
    for (crd::u64 i = 0; i < 24U; ++i)
    {
        t.data()[i] = static_cast<crd::f32>(i);
    }
    const crd::u32 order[] = {2U, 0U, 1U};
    TensorView<crd::f32> v = t.view().permute(order).slice(0U, 1U, 4U, 2U);
    if (v(0U, 1U, 2U) != 21.0F)
    {
        std::puts("FAIL: view stride math");
        return 1;
    }

    // Converts (crd-math primitives) + deterministic SR (tensor keying).
    crd::u16 h[24];
    convert_f32_to_f16({t.data(), 24U}, {h, 24U});
    crd::u16 h_sr[24];
    convert_f32_to_f16_sr({t.data(), 24U}, {h_sr, 24U}, /*seed=*/7U);
    crd::u16 h_sr2[24];
    convert_f32_to_f16_sr({t.data(), 24U}, {h_sr2, 24U}, /*seed=*/7U);
    for (crd::u32 i = 0; i < 24U; ++i)
    {
        if (h_sr[i] != h_sr2[i])
        {
            std::puts("FAIL: SR run-twice bit-identity");
            return 1;
        }
    }

    // ggml block quant round-trip sanity.
    crd::f32 x[32];
    for (crd::u32 i = 0; i < 32U; ++i)
    {
        x[i] = 0.25F * static_cast<crd::f32>(i) - 3.0F;
    }
    BlockQ8_0 q8[1];
    quantize_q8_0({x, 32U}, {q8, 1U});
    crd::f32 back[32];
    dequantize_q8_0({q8, 1U}, {back, 32U});
    const crd::f32 d = f16_bits_to_f32(q8[0].d);
    for (crd::u32 i = 0; i < 32U; ++i)
    {
        const crd::f32 err = back[i] - x[i];
        if ((err < 0 ? -err : err) > d)
        {
            std::puts("FAIL: q8_0 round-trip error bound");
            return 1;
        }
    }

    std::puts("smoke_hesap_tensor: OK (views + converts + deterministic SR + q8_0; link-isolated)");
    return 0;
}
