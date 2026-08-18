// CEIR-22b (DX12) — the DirectX-12 twin of the CEIR→CKIR native-provider FFT device gate (kernel tier). A declare-only
// `ceir.tensor.fft` op is SYNTHESIZED (synth_fft) into a CKIR Fft1dPlan (radix-2), run on a REAL D3D12 device via
// dispatch_kernel_1wg, validated against BOTH eval_cpu_kernel AND an INDEPENDENT naive DFT — for BOTH directions (forward +
// inverse are DISTINCT kernel paths; the 20c-2 guard). ⛔ the CKIR inverse conjugates the SAME forward twiddles internally +
// omits the 1/n scale → the inverse ref is the UNSCALED +i DFT. DECLARED f32 tolerance (2e-3 * maxmag, the FFT-kernel
// precedent). local_size = n/2 (the radix-2 contract, pinned).

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/tensor.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>

#include <crd/math/cmath.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ckir_kernel_dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace kir = crd::kir;

namespace
{
constexpr int kN    = 8;
constexpr int kHalf = kN / 2;

ce::TypeId sh1(ce::Context& ctx, crd::u32 a)
{
    const ce::TypeId d[1] = {ctx.type_dim_static(a)};
    return ctx.type_shape(crd::containers::ConstSpan<ce::TypeId>(d, 1U));
}
ce::Operation* build_fft_op(ce::Context& ctx, ce::Module& m, const char* dir)
{
    const ce::OpId decl = ctx.intern_op("resource", "declare");
    ce::Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const     b  = ce::func::func_body_block(f);
    const ce::TypeId     ef = ctx.type_f32();
    const ce::TypeId     sh = sh1(ctx, kN);
    ce::Operation* const re = ctx.create_operation(decl, {}, 1U, ctx.type_tensor(ef, sh));
    ce::Operation* const im = ctx.create_operation(decl, {}, 1U, ctx.type_tensor(ef, sh));
    b->append(re);
    b->append(im);
    ce::Operation* const op = ce::tensor::build_fft(ctx, re->result(0U), im->result(0U),
                                                    ctx.attr_string(crd::containers::StringView(dir)), ctx.attr_int(0),
                                                    ctx.type_tensor(ef, sh));
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 22b: synth_fft runs forward+inverse c2c FFT on a DX12 device (== eval_cpu_kernel AND an independent DFT, declared tol)",
          "[ceir][ckir-synth][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::tensor::register_dialect(ctx);
    crd::memory::TlsfAllocator alloc(64U << 20U);

    {
        ce::Module* const   mm = ctx.create_module();
        kir::KGraph         g(&alloc);
        const ceg::FftSynth s = ceg::synth_fft(ctx, *build_fft_op(ctx, *mm, "forward"), g);
        REQUIRE(s.reject == ceg::SynthReject::None);
        REQUIRE(s.n == kN);
        REQUIRE(s.plan.entry.local_size[0] == static_cast<crd::u32>(kHalf));
    }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    crd::f64           ir[kN];
    crd::f64           ii[kN];
    crd::f64           twr[kHalf];
    crd::f64           twi[kHalf];
    for (int i = 0; i < kN; ++i)
    {
        ir[i] = static_cast<crd::f64>(static_cast<float>((i * 7 + 3) % 11 - 5));
        ii[i] = static_cast<crd::f64>(static_cast<float>((i * 5 + 1) % 7 - 3));
    }
    for (int k = 0; k < kHalf; ++k)
    {
        const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(kN);
        twr[k]           = static_cast<crd::f64>(static_cast<float>(crd::math::cos(a)));
        twi[k]           = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(a)));
    }
    crd::f64 fwd_r[kN];
    crd::f64 fwd_i[kN];
    crd::f64 inv_r[kN];
    crd::f64 inv_i[kN];
    for (int k = 0; k < kN; ++k)
    {
        crd::f64 fr = 0.0;
        crd::f64 fi = 0.0;
        crd::f64 vr = 0.0;
        crd::f64 vi = 0.0;
        for (int j = 0; j < kN; ++j)
        {
            const crd::f64 th = two_pi * static_cast<crd::f64>(k) * static_cast<crd::f64>(j) / static_cast<crd::f64>(kN);
            const crd::f64 c  = crd::math::cos(th);
            const crd::f64 sn = crd::math::sin(th);
            fr += ir[j] * c + ii[j] * sn;
            fi += ii[j] * c - ir[j] * sn;
            vr += ir[j] * c - ii[j] * sn;
            vi += ir[j] * sn + ii[j] * c;
        }
        fwd_r[k] = fr;
        fwd_i[k] = fi;
        inv_r[k] = vr;
        inv_i[k] = vi;
    }

    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-22b fft device gate"); return; }

    const auto run_dir = [&](const char* dir, const crd::f64* ref_r, const crd::f64* ref_i) {
        ce::Module* const   mm = ctx.create_module();
        kir::KGraph         g(&alloc);
        const ceg::FftSynth s = ceg::synth_fft(ctx, *build_fft_op(ctx, *mm, dir), g);
        REQUIRE(s.reject == ceg::SynthReject::None);
        const crd::u32 ls = s.plan.entry.local_size[0];

        crd::f64 orr[kN];
        crd::f64 oi[kN];
        for (int i = 0; i < kN; ++i) { orr[i] = -99.0; oi[i] = -99.0; }
        kir::KernelBuffer bufs[6] = {{ir, kN, 0, 0},     {ii, kN, 0, 1},   {twr, kHalf, 0, 2},
                                     {twi, kHalf, 0, 3},  {orr, kN, 0, 4},  {oi, kN, 0, 5}};
        kir::eval_cpu_kernel(g, s.plan.entry, bufs, 6, ls, &alloc);

        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(g, s.plan.entry, &alloc, kern));
        auto pipe = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 6, 0U);
        REQUIRE(pipe != nullptr);

        float h_ir[kN];
        float h_ii[kN];
        float h_twr[kHalf];
        float h_twi[kHalf];
        float h_or[kN];
        float h_oi[kN];
        for (int i = 0; i < kN; ++i) { h_ir[i] = static_cast<float>(ir[i]); h_ii[i] = static_cast<float>(ii[i]); h_or[i] = -99.0F; h_oi[i] = -99.0F; }
        for (int k = 0; k < kHalf; ++k) { h_twr[k] = static_cast<float>(twr[k]); h_twi[k] = static_cast<float>(twi[k]); }
        float*    host[6] = {h_ir, h_ii, h_twr, h_twi, h_or, h_oi};
        const int lens[6] = {kN, kN, kHalf, kHalf, kN, kN};
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 6, 1U);

        const auto fa     = [](float x) { return x < 0.0F ? -x : x; };
        float      maxmag = 1e-6F;
        for (int k = 0; k < kN; ++k)
        {
            maxmag = maxmag > fa(static_cast<float>(ref_r[k])) ? maxmag : fa(static_cast<float>(ref_r[k]));
            maxmag = maxmag > fa(static_cast<float>(ref_i[k])) ? maxmag : fa(static_cast<float>(ref_i[k]));
        }
        const float tol = 2e-3F * maxmag;
        for (int k = 0; k < kN; ++k)
        {
            CHECK(fa(h_or[k] - static_cast<float>(orr[k])) <= tol);
            CHECK(fa(h_oi[k] - static_cast<float>(oi[k])) <= tol);
            CHECK(fa(h_or[k] - static_cast<float>(ref_r[k])) <= tol);
            CHECK(fa(h_oi[k] - static_cast<float>(ref_i[k])) <= tol);
        }
    };

    run_dir("forward", fwd_r, fwd_i);
    run_dir("inverse", inv_r, inv_i);
}
