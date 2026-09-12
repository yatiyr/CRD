// CEIR-25b-2b (Vulkan) -- the CEIR->CKIR native-provider BROADCAST + PERMUTE device gate. A declare-only ceir.tensor.broadcast /
// ceir.tensor.transpose op is SYNTHESIZED (synth_broadcast / synth_transpose) into a graph-tier CKIR Broadcast / Permute node, run
// on a REAL Vulkan device (KirBackendVulkan) via the NEW root emitters (emit_broadcast_glsl / emit_permute_glsl, wired into run()),
// proven EXACT (pure data movement) vs BOTH eval_cpu AND an INDEPENDENT index-map reference. A baked-stride bug maps to the wrong
// input element -> mismatches the independent ref (which the shared eval_cpu path might share). Rank-2 [1,0] AND a rank-3 perm.

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

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
TypeId shp(Context& ctx, const u32* dims, u32 rank)
{
    TypeId d[8];
    for (u32 i = 0; i < rank; ++i) { d[i] = ctx.type_dim_static(dims[i]); }
    return ctx.type_shape(ConstSpan<TypeId>(d, rank));
}
Value* decl_tensor(Context& ctx, Block* b, TypeId t)
{
    const OpId       decl = ctx.intern_op("resource", "declare");
    Operation* const d    = ctx.create_operation(decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
u32 numel(const u32* d, u32 r)
{
    u32 n = 1U;
    for (u32 i = 0; i < r; ++i) { n *= d[i]; }
    return n;
}
} // namespace

TEST_CASE("ceir 25b-2b: synth_broadcast runs on a Vulkan device (both axes) == eval_cpu AND an independent index-map ref", "[ceir][ckir-synth][gpu]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)tensor::register_dialect(ctx);
    memory::TlsfAllocator kalloc(64U << 20U);

    struct Case
    {
        u32 in_dims[2];
        u32 out_dims[2]; // rank-2, same rank (the synth envelope)
    };
    const Case cases[2] = {{{3U, 1U}, {3U, 4U}}, {{1U, 4U}, {3U, 4U}}}; // last-axis broadcast; first-axis broadcast

    // synthesis always runs (all-skip false-green guard)
    {
        Module* const m  = ctx.create_module();
        Block* const  b  = mkmain(ctx, *m);
        const TypeId  ef = ctx.type_f32();
        Value* const  in = decl_tensor(ctx, b, ctx.type_tensor(ef, shp(ctx, cases[0].in_dims, 2U)));
        Operation* const op = tensor::build_broadcast(ctx, in, ctx.type_tensor(ef, shp(ctx, cases[0].out_dims, 2U)));
        b->append(op);
        kir::KGraph g(&kalloc);
        REQUIRE(gpu::synth_broadcast(ctx, *op, g).reject == gpu::SynthReject::None);
    }

    kir::KirBackendVulkan vk(&kalloc);
    if (!vk.valid()) { WARN("no Vulkan device -- skipping the CEIR-25b-2b broadcast device gate"); return; }
    kir::KirBackendCpu cpu(&kalloc);

    for (const Case& cs : cases)
    {
        const u32 in_n  = numel(cs.in_dims, 2U);
        const u32 out_n = numel(cs.out_dims, 2U);
        static float in_data[16];
        for (u32 i = 0; i < in_n; ++i) { in_data[i] = static_cast<float>(10 * (i + 1)); }

        Module* const    m  = ctx.create_module();
        Block* const     b  = mkmain(ctx, *m);
        const TypeId     ef = ctx.type_f32();
        Value* const     in = decl_tensor(ctx, b, ctx.type_tensor(ef, shp(ctx, cs.in_dims, 2U)));
        Operation* const op = tensor::build_broadcast(ctx, in, ctx.type_tensor(ef, shp(ctx, cs.out_dims, 2U)));
        b->append(op);
        kir::KGraph           g(&kalloc);
        const gpu::GraphSynth s = gpu::synth_broadcast(ctx, *op, g);
        REQUIRE(s.reject == gpu::SynthReject::None);

        const float* inputs[1] = {static_cast<const float*>(in_data)};
        float        gpu_out[12];
        float        cpu_out[12];
        REQUIRE(vk.run(g, s.output, inputs, 1, gpu_out));
        REQUIRE(cpu.run(g, s.output, inputs, 1, cpu_out));
        for (u32 r = 0; r < cs.out_dims[0]; ++r)
        {
            for (u32 c = 0; c < cs.out_dims[1]; ++c)
            {
                // independent ref: source coord is 0 on a size-1 axis, else the output coord.
                const u32   ir  = (cs.in_dims[0] == 1U) ? 0U : r;
                const u32   ic  = (cs.in_dims[1] == 1U) ? 0U : c;
                const float ref = in_data[ir * cs.in_dims[1] + ic];
                const u32   o   = r * cs.out_dims[1] + c;
                CHECK(gpu_out[o] == cpu_out[o]);
                CHECK(gpu_out[o] == ref);
            }
        }
        (void)out_n;
    }
    CHECK(vk.validation_errors() == 0);
}

TEST_CASE("ceir 25b-2b: synth_transpose runs on a Vulkan device (rank-2 [1,0] AND a rank-3 perm) == eval_cpu AND an independent ref", "[ceir][ckir-synth][gpu]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)tensor::register_dialect(ctx);
    memory::TlsfAllocator kalloc(64U << 20U);

    kir::KirBackendVulkan vk(&kalloc);
    const bool            have_dev = vk.valid();
    kir::KirBackendCpu    cpu(&kalloc);

    // -- rank-2 transpose [2,3] perm=[1,0] -> [3,2]; ref out[c*2+r] = in[r*3+c] --
    {
        const u32        idim[2] = {2U, 3U};
        const u32        odim[2] = {3U, 2U};
        const TypeId     ef      = ctx.type_f32();
        Module* const    m       = ctx.create_module();
        Block* const     b       = mkmain(ctx, *m);
        Value* const     in      = decl_tensor(ctx, b, ctx.type_tensor(ef, shp(ctx, idim, 2U)));
        Operation* const op = tensor::build_transpose(ctx, in, ctx.attr_string(StringView("1,0")), ctx.type_tensor(ef, shp(ctx, odim, 2U)));
        b->append(op);
        kir::KGraph           g(&kalloc);
        const gpu::GraphSynth s = gpu::synth_transpose(ctx, *op, g);
        REQUIRE(s.reject == gpu::SynthReject::None); // synthesis always checked (all-skip guard)
        if (have_dev)
        {
            static float in_data[6];
            for (int i = 0; i < 6; ++i) { in_data[i] = static_cast<float>(i + 1); }
            const float* inputs[1] = {static_cast<const float*>(in_data)};
            float        gpu_out[6];
            float        cpu_out[6];
            REQUIRE(vk.run(g, s.output, inputs, 1, gpu_out));
            REQUIRE(cpu.run(g, s.output, inputs, 1, cpu_out));
            for (u32 r = 0; r < 2U; ++r)
            {
                for (u32 c = 0; c < 3U; ++c)
                {
                    const u32 o = c * 2U + r; // [3,2] row-major
                    CHECK(gpu_out[o] == cpu_out[o]);
                    CHECK(gpu_out[o] == in_data[r * 3U + c]);
                }
            }
        }
    }

    // -- rank-3 transpose [2,3,4] perm=[2,0,1] -> [4,2,3]: out axis0<-in axis2, axis1<-in axis0, axis2<-in axis1 --
    {
        const u32        idim[3] = {2U, 3U, 4U};
        const u32        odim[3] = {4U, 2U, 3U};
        const TypeId     ef      = ctx.type_f32();
        Module* const    m       = ctx.create_module();
        Block* const     b       = mkmain(ctx, *m);
        Value* const     in      = decl_tensor(ctx, b, ctx.type_tensor(ef, shp(ctx, idim, 3U)));
        Operation* const op = tensor::build_transpose(ctx, in, ctx.attr_string(StringView("2,0,1")), ctx.type_tensor(ef, shp(ctx, odim, 3U)));
        b->append(op);
        kir::KGraph           g(&kalloc);
        const gpu::GraphSynth s = gpu::synth_transpose(ctx, *op, g);
        REQUIRE(s.reject == gpu::SynthReject::None);
        if (have_dev)
        {
            static float in_data[24];
            for (int i = 0; i < 24; ++i) { in_data[i] = static_cast<float>(i); }
            const float* inputs[1] = {static_cast<const float*>(in_data)};
            float        gpu_out[24];
            float        cpu_out[24];
            REQUIRE(vk.run(g, s.output, inputs, 1, gpu_out));
            REQUIRE(cpu.run(g, s.output, inputs, 1, cpu_out));
            // independent ref: out[o0,o1,o2] = in[i0,i1,i2] with i0=o1, i1=o2, i2=o0 (perm[k]=source axis of out axis k).
            for (u32 o0 = 0; o0 < 4U; ++o0)
            {
                for (u32 o1 = 0; o1 < 2U; ++o1)
                {
                    for (u32 o2 = 0; o2 < 3U; ++o2)
                    {
                        const u32 o      = (o0 * 2U + o1) * 3U + o2;     // [4,2,3] row-major
                        const u32 in_idx = (o1 * 3U + o2) * 4U + o0;     // [2,3,4]: i0=o1, i1=o2, i2=o0
                        CHECK(gpu_out[o] == cpu_out[o]);
                        CHECK(gpu_out[o] == in_data[in_idx]);
                    }
                }
            }
        }
    }
    if (have_dev) { CHECK(vk.validation_errors() == 0); }
    else { WARN("no Vulkan device -- ran synthesis-only for the CEIR-25b-2b transpose gate"); }
}
