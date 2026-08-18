// CEIR-22b (DX12) — the DirectX-12 twin of the CEIR→CKIR native-provider REDUCE device gate. A declare-only
// `ceir.tensor.reduce` op is SYNTHESIZED (synth_reduce) into a graph-tier CKIR reduce, run on a REAL D3D12 device
// (KirBackendDx12), proven bit-exact vs BOTH eval_cpu AND an INDEPENDENT serial reference for ALL FOUR fns {sum,prod,max,min}
// (a fn→KOp mis-wiring would bit-match its own oracle but NOT the independent ref; the 20c-2 guard). data ((r+c)%3)+1 ∈ {1,2,3}.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/tensor.hpp>

#include <crd/kir/backend.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/dx12/backend_dx12.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
constexpr int kR = 6;
constexpr int kC = 8;

TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return ctx.type_shape(ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
Operation* build_reduce_op(Context& ctx, Module& m, const char* fn)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const     b   = func::func_body_block(f);
    const TypeId     ef  = ctx.type_f32();
    Operation* const src = ctx.create_operation(decl, {}, 1U, ctx.type_tensor(ef, sh2(ctx, kR, kC)));
    b->append(src);
    Operation* const op = tensor::build_reduce(ctx, src->result(0U), ctx.attr_int(1), ctx.attr_string(StringView(fn)),
                                               ctx.type_tensor(ef, sh1(ctx, kR)));
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 22b: synth_reduce runs sum/prod/max/min bit-exact on a DX12 device (== eval_cpu AND independent serial refs)",
          "[ceir][ckir-synth][gpu]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)tensor::register_dialect(ctx);

    memory::TlsfAllocator kalloc(64U << 20U);

    static float in_data[kR * kC];
    for (int r = 0; r < kR; ++r)
    {
        for (int c = 0; c < kC; ++c) { in_data[r * kC + c] = static_cast<float>(((r + c) % 3) + 1); }
    }
    static float ref_sum[kR];
    static float ref_prod[kR];
    static float ref_max[kR];
    static float ref_min[kR];
    for (int r = 0; r < kR; ++r)
    {
        float sm = 0.0F;
        float pr = 1.0F;
        float mx = in_data[r * kC];
        float mn = in_data[r * kC];
        for (int c = 0; c < kC; ++c)
        {
            const float v = in_data[r * kC + c];
            sm += v;
            pr *= v;
            mx = mx > v ? mx : v;
            mn = mn < v ? mn : v;
        }
        ref_sum[r]  = sm;
        ref_prod[r] = pr;
        ref_max[r]  = mx;
        ref_min[r]  = mn;
    }

    {
        Module* const    m  = ctx.create_module();
        Operation* const op = build_reduce_op(ctx, *m, "sum");
        kir::KGraph       g(&kalloc);
        REQUIRE(gpu::synth_reduce(ctx, *op, g).reject == gpu::SynthReject::None);
    }

    kir::KirBackendDx12 dx(&kalloc);
    if (!dx.valid()) { WARN("no DX12 device — skipping the CEIR-22b reduce device gate"); return; }
    kir::KirBackendCpu cpu(&kalloc);

    struct Case
    {
        const char*  fn;
        const float* ref;
    };
    const Case cases[4] = {{"sum", ref_sum}, {"prod", ref_prod}, {"max", ref_max}, {"min", ref_min}};
    for (const Case& cs : cases)
    {
        Module* const         m  = ctx.create_module();
        Operation* const      op = build_reduce_op(ctx, *m, cs.fn);
        kir::KGraph           g(&kalloc);
        const gpu::GraphSynth s = gpu::synth_reduce(ctx, *op, g);
        REQUIRE(s.reject == gpu::SynthReject::None);
        const float* inputs[1] = {static_cast<const float*>(in_data)};
        float        gpu_out[kR];
        float        cpu_out[kR];
        REQUIRE(dx.run(g, s.output, inputs, 1, gpu_out));
        REQUIRE(cpu.run(g, s.output, inputs, 1, cpu_out));
        for (int i = 0; i < kR; ++i)
        {
            CHECK(gpu_out[i] == cpu_out[i]);
            CHECK(gpu_out[i] == cs.ref[i]);
        }
    }
}
