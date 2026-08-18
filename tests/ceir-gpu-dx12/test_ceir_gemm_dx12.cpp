// CEIR-22b (DX12) — the DirectX-12 twin of the CEIR→CKIR native-provider GEMM device gate (test_ceir_gemm_vulkan.cpp). A
// declare-only `ceir.linalg.gemm` op is SYNTHESIZED (ckir_synth::synth_gemm) into a graph-tier CKIR Contract, run on a REAL
// D3D12 device (KirBackendDx12), and proven bit-exact against BOTH the CKIR CPU oracle (KirBackendCpu = eval_cpu) AND an
// INDEPENDENT triple-loop reference. ⛔ the independent ref guards the 20c-2 trap (eval_cpu runs the SAME synthesized graph).
// Small-integer data ⇒ exact f32; the odd 32×48×24 shape forces the Naive (bit-exact) schedule. Device-free synth REQUIRE runs
// ALWAYS (the all-skip false-green guard); the device run soft-skips with no adapter.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/linalg.hpp>

#include <crd/kir/backend.hpp>            // KirBackendCpu (the eval_cpu oracle wrapper)
#include <crd/kir/ckir.hpp>
#include <crd/kir/dx12/backend_dx12.hpp> // KirBackendDx12 (self-creates a D3D12 device)

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;

namespace
{
constexpr int kM = 32;
constexpr int kK = 48;
constexpr int kN = 24;

TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
Operation* build_gemm_op(Context& ctx, Module& m)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const b  = func::func_body_block(f);
    const TypeId ef = ctx.type_f32();
    const auto   mk = [&](TypeId t) { Operation* const d = ctx.create_operation(decl, {}, 1U, t); b->append(d); return d->result(0U); };
    Value* const a  = mk(ctx.type_tensor(ef, sh2(ctx, kM, kK)));
    Value* const bb = mk(ctx.type_tensor(ef, sh2(ctx, kK, kN)));
    Value* const c  = mk(ctx.type_tensor(ef, sh2(ctx, kM, kN)));
    Operation* const op = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                             ctx.attr_bool(false), ctx.type_tensor(ef, sh2(ctx, kM, kN)));
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 22b: synth_gemm runs A*B bit-exact on a DX12 device (== eval_cpu AND an independent triple loop)",
          "[ceir][ckir-synth][gpu]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)linalg::register_dialect(ctx);
    Module* const    m  = ctx.create_module();
    Operation* const op = build_gemm_op(ctx, *m);

    memory::TlsfAllocator kalloc(64U << 20U);
    kir::KGraph           g(&kalloc);
    const gpu::GraphSynth s = gpu::synth_gemm(ctx, *op, g);
    REQUIRE(s.reject == gpu::SynthReject::None);
    REQUIRE(s.output >= 0);

    static float a_data[kM * kK];
    static float b_data[kK * kN];
    for (int mm = 0; mm < kM; ++mm)
    {
        for (int kk = 0; kk < kK; ++kk) { a_data[mm * kK + kk] = static_cast<float>(((mm + kk) % 7) - 3); }
    }
    for (int kk = 0; kk < kK; ++kk)
    {
        for (int nn = 0; nn < kN; ++nn) { b_data[kk * kN + nn] = static_cast<float>(((kk + 2 * nn) % 5) - 2); }
    }
    static float ref[kM * kN];
    for (int mm = 0; mm < kM; ++mm)
    {
        for (int nn = 0; nn < kN; ++nn)
        {
            float acc = 0.0F;
            for (int kk = 0; kk < kK; ++kk) { acc += a_data[mm * kK + kk] * b_data[kk * kN + nn]; }
            ref[mm * kN + nn] = acc;
        }
    }

    kir::KirBackendDx12 dx(&kalloc);
    if (!dx.valid()) { WARN("no DX12 device — skipping the CEIR-22b gemm device gate"); return; }
    kir::KirBackendCpu cpu(&kalloc);

    const float* inputs[2] = {static_cast<const float*>(a_data), static_cast<const float*>(b_data)};
    static float gpu_out[kM * kN];
    static float cpu_out[kM * kN];
    REQUIRE(dx.run(g, s.output, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, s.output, inputs, 2, cpu_out));

    for (int i = 0; i < kM * kN; ++i)
    {
        CHECK(gpu_out[i] == cpu_out[i]); // device == the CKIR oracle (EXECUTION)
        CHECK(gpu_out[i] == ref[i]);     // device == the independent triple loop (SYNTHESIS — the 20c-2 guard)
    }
}
