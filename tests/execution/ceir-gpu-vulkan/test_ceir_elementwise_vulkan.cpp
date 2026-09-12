// CEIR-25b-2a (Vulkan) -- the CEIR->CKIR native-provider ELEMENTWISE device gate. A declare-only ceir.tensor.elementwise op is
// SYNTHESIZED (synth_elementwise) into a graph-tier CKIR binary node, run on a REAL Vulkan device (KirBackendVulkan) via the
// EXISTING fused-elementwise emitter (emit_elementwise_glsl, reached by run()'s fall-through -- is_fusable covers the vocab),
// and proven vs BOTH eval_cpu AND an INDEPENDENT scalar reference for the FULL binary vocab {add,sub,mul,div,max,min,pow}.
// A fn->KOp mis-wiring (e.g. sub->KOp::Add) would bit-match its own eval_cpu oracle but NOT the independent ref (the 20c-2 guard).
// PER-OP contract: add/sub/mul/div/max/min are BIT-EXACT (correctly-rounded single f32 op, no FMA under `precise`; div exact for
// b in {1,2} = exact reciprocal); pow is transcendental = ULP-class, a declared relative tol. data a in {2,4,6,8}, b in {1,2}.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/tensor.hpp>

#include <crd/kir/backend.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/vulkan/backend_vulkan.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath> // std::pow / std::fabs -- the INDEPENDENT scalar reference (NOT the CKIR path)

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
constexpr int kRows = 4;
constexpr int kCols = 8;
constexpr int kN    = kRows * kCols; // 32

TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
// elementwise(a[kRows,kCols] {fn} b[kRows,kCols]) -> [kRows,kCols], in a FRESH module (one per fn).
Operation* build_ew_op(Context& ctx, Module& m, const char* fn)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const     b  = func::func_body_block(f);
    const TypeId     ef = ctx.type_f32();
    const TypeId     s  = sh2(ctx, kRows, kCols);
    Operation* const a  = ctx.create_operation(decl, {}, 1U, ctx.type_tensor(ef, s));
    Operation* const bb = ctx.create_operation(decl, {}, 1U, ctx.type_tensor(ef, s));
    b->append(a);
    b->append(bb);
    Operation* const op = tensor::build_elementwise(ctx, a->result(0U), bb->result(0U), ctx.attr_string(StringView(fn)),
                                                    ctx.type_tensor(ef, s));
    b->append(op);
    return op;
}
// the INDEPENDENT scalar reference (the op DEFINITION, per fn) -- NOT the CKIR path.
float ref_op(const char* fn, float a, float b)
{
    const StringView f(fn);
    if (f == StringView("add")) { return a + b; }
    if (f == StringView("sub")) { return a - b; }
    if (f == StringView("mul")) { return a * b; }
    if (f == StringView("div")) { return a / b; }
    if (f == StringView("max")) { return a > b ? a : b; }
    if (f == StringView("min")) { return a < b ? a : b; }
    return static_cast<float>(std::pow(static_cast<double>(a), static_cast<double>(b))); // pow
}
} // namespace

TEST_CASE("ceir 25b-2a: synth_elementwise runs the full binary vocab on a Vulkan device (add/sub/mul/div/max/min exact, pow within tol)",
          "[ceir][ckir-synth][gpu]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)tensor::register_dialect(ctx);

    memory::TlsfAllocator kalloc(64U << 20U);

    // data a in {2,4,6,8}, b in {1,2}: distinct + f32-exact (div exact = exact reciprocal of 1/2).
    static float a_data[kN];
    static float b_data[kN];
    for (int i = 0; i < kN; ++i)
    {
        a_data[i] = static_cast<float>(2 * ((i % 4) + 1));
        b_data[i] = static_cast<float>((i % 2) + 1);
    }

    // -- SYNTHESIS (device-free, ALWAYS runs -- the all-skip false-green guard) --
    {
        Module* const    m  = ctx.create_module();
        Operation* const op = build_ew_op(ctx, *m, "add");
        kir::KGraph      g(&kalloc);
        REQUIRE(gpu::synth_elementwise(ctx, *op, g).reject == gpu::SynthReject::None);
    }

    // -- DEVICE (soft-skip with no adapter) --
    kir::KirBackendVulkan vk(&kalloc);
    if (!vk.valid()) { WARN("no Vulkan device -- skipping the CEIR-25b-2a elementwise device gate"); return; }
    kir::KirBackendCpu cpu(&kalloc);

    struct Case
    {
        const char* fn;
        bool        exact;
    };
    const Case cases[7] = {{"add", true}, {"sub", true}, {"mul", true},  {"div", true},
                           {"max", true}, {"min", true}, {"pow", false}};
    for (const Case& cs : cases)
    {
        Module* const         m  = ctx.create_module();
        Operation* const      op = build_ew_op(ctx, *m, cs.fn);
        kir::KGraph           g(&kalloc);
        const gpu::GraphSynth s = gpu::synth_elementwise(ctx, *op, g);
        REQUIRE(s.reject == gpu::SynthReject::None);
        const float* inputs[2] = {static_cast<const float*>(a_data), static_cast<const float*>(b_data)};
        float        gpu_out[kN];
        float        cpu_out[kN];
        REQUIRE(vk.run(g, s.output, inputs, 2, gpu_out));
        REQUIRE(cpu.run(g, s.output, inputs, 2, cpu_out));
        for (int i = 0; i < kN; ++i)
        {
            const float ref = ref_op(cs.fn, a_data[i], b_data[i]);
            if (cs.exact)
            {
                CHECK(gpu_out[i] == cpu_out[i]); // device == the CKIR oracle (EXECUTION)
                CHECK(gpu_out[i] == ref);        // device == the independent op (SYNTHESIS -- the fn->KOp guard)
            }
            else
            {
                const float tol_c = 1.0e-5F * (1.0F + std::fabs(cpu_out[i]));
                const float tol_r = 1.0e-5F * (1.0F + std::fabs(ref));
                CHECK(std::fabs(gpu_out[i] - cpu_out[i]) <= tol_c);
                CHECK(std::fabs(gpu_out[i] - ref) <= tol_r);
            }
        }
    }
    CHECK(vk.validation_errors() == 0);
}
