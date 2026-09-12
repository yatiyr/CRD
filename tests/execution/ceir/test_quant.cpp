// CEIR-23a (§54) — the ceir.quant dialect: quant.quantize (float -> int-storage) + quant.dequantize (int-storage -> float),
// scale/zero_point as rank-0 (per-tensor) or rank-1 (per-axis) OPERANDS, the storage int riding the result. find_quant_misuse
// enforces the SHAPE-AWARE + ELEMENT-ROLE contract (value=float / storage=int, swapped between the two ops) the generated
// verify cannot. Well-formed per-tensor + per-axis verify clean; every contract misuse rejects with the exact kind.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/quant.hpp>
#include <crd/ceir/tensor.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
using QK = crd::ceir::quant::QuantMisuseKind;

namespace
{
struct Kit
{
    OpId decl;
    explicit Kit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)tensor::register_dialect(ctx);
        (void)quant::register_dialect(ctx);
    }
};
TypeId sh0(Context& ctx) { return ctx.type_shape(ConstSpan<TypeId>{}); }
TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return ctx.type_shape(ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 b)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(b)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId tt(Context& ctx, TypeId elem, TypeId shape) { return ctx.type_tensor(elem, shape); }

Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
Value* decl(Context& ctx, const Kit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
} // namespace

TEST_CASE("ceir 23a: well-formed quantize + dequantize verify clean (per-tensor + per-axis)", "[ceir][quant]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  f32 = ctx.type_f32();
    const TypeId                  i8  = ctx.type_int(8U, true);
    Module* const                 m   = ctx.create_module();
    Block* const                  b   = mkmain(ctx, *m);

    // per-tensor quantize: input f32[4,4], scale f32[], zero_point i8[], output i8[4,4].
    Value* const in_f  = decl(ctx, k, b, tt(ctx, f32, sh2(ctx, 4U, 4U)));
    Value* const sc0   = decl(ctx, k, b, tt(ctx, f32, sh0(ctx)));
    Value* const zp0   = decl(ctx, k, b, tt(ctx, i8, sh0(ctx)));
    Operation* const q = quant::build_quantize(ctx, in_f, sc0, zp0, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                               tt(ctx, i8, sh2(ctx, 4U, 4U)));
    b->append(q);
    // per-tensor dequantize: input i8[4,4], scale f32[], zero_point i8[], output f32[4,4].
    Value* const in_q   = decl(ctx, k, b, tt(ctx, i8, sh2(ctx, 4U, 4U)));
    Operation* const dq = quant::build_dequantize(ctx, in_q, sc0, zp0, ctx.attr_int(0), ctx.attr_string(StringView("asymmetric")),
                                                  tt(ctx, f32, sh2(ctx, 4U, 4U)));
    b->append(dq);
    // per-axis quantize: scale f32[4], zero_point i8[4], axis=1 (== input.dim[1]==4).
    Value* const sc1   = decl(ctx, k, b, tt(ctx, f32, sh1(ctx, 4U)));
    Value* const zp1   = decl(ctx, k, b, tt(ctx, i8, sh1(ctx, 4U)));
    Operation* const qa = quant::build_quantize(ctx, in_f, sc1, zp1, ctx.attr_int(1), ctx.attr_string(StringView("symmetric")),
                                                tt(ctx, i8, sh2(ctx, 4U, 4U)));
    b->append(qa);

    CHECK(quant::find_quant_misuse(ctx, *m).kind == QK::None);
}

TEST_CASE("ceir 23a: find_quant_misuse rejects every contract misuse with the exact kind", "[ceir][quant]")
{
    const auto one = [](auto build) {
        memory::GrowableTlsfAllocator root;
        Context                       ctx(&root);
        const Kit                     k(ctx);
        Module* const                 m = ctx.create_module();
        Block* const                  b = mkmain(ctx, *m);
        build(ctx, k, b);
        return quant::find_quant_misuse(ctx, *m).kind;
    };
    const TypeId dummy = {};
    (void)dummy;

    // ShapeMismatch: output shape [4,2] != input [4,4].
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f = ctx.type_f32();
        const TypeId i = ctx.type_int(8U, true);
        Value* in = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 4U)));
        Value* sc = decl(ctx, k, b, tt(ctx, f, sh0(ctx)));
        Value* zp = decl(ctx, k, b, tt(ctx, i, sh0(ctx)));
        b->append(quant::build_quantize(ctx, in, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                        tt(ctx, i, sh2(ctx, 4U, 2U))));
    }) == QK::ShapeMismatch);

    // ScaleRankInvalid: scale is rank-2.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f = ctx.type_f32();
        const TypeId i = ctx.type_int(8U, true);
        Value* in = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 4U)));
        Value* sc = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 4U)));
        Value* zp = decl(ctx, k, b, tt(ctx, i, sh2(ctx, 4U, 4U)));
        b->append(quant::build_quantize(ctx, in, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                        tt(ctx, i, sh2(ctx, 4U, 4U))));
    }) == QK::ScaleRankInvalid);

    // AxisScaleMismatch: rank-1 scale of size 3 but input.dim[1]==4.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f = ctx.type_f32();
        const TypeId i = ctx.type_int(8U, true);
        Value* in = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 4U)));
        Value* sc = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 3U)));
        Value* zp = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 3U)));
        b->append(quant::build_quantize(ctx, in, sc, zp, ctx.attr_int(1), ctx.attr_string(StringView("symmetric")),
                                        tt(ctx, i, sh2(ctx, 4U, 4U))));
    }) == QK::AxisScaleMismatch);

    // ScaleElementMismatch: scale is int, not the value/float element.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f = ctx.type_f32();
        const TypeId i = ctx.type_int(8U, true);
        Value* in = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 4U)));
        Value* sc = decl(ctx, k, b, tt(ctx, i, sh0(ctx))); // WRONG: int scale
        Value* zp = decl(ctx, k, b, tt(ctx, i, sh0(ctx)));
        b->append(quant::build_quantize(ctx, in, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                        tt(ctx, i, sh2(ctx, 4U, 4U))));
    }) == QK::ScaleElementMismatch);

    // ZeroPointElementMismatch: zero_point is float, not the storage/int element.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f = ctx.type_f32();
        const TypeId i = ctx.type_int(8U, true);
        Value* in = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 4U)));
        Value* sc = decl(ctx, k, b, tt(ctx, f, sh0(ctx)));
        Value* zp = decl(ctx, k, b, tt(ctx, f, sh0(ctx))); // WRONG: float zero_point
        b->append(quant::build_quantize(ctx, in, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                        tt(ctx, i, sh2(ctx, 4U, 4U))));
    }) == QK::ZeroPointElementMismatch);

    // SchemeInvalid: scheme "linear" not in {symmetric,asymmetric}.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f = ctx.type_f32();
        const TypeId i = ctx.type_int(8U, true);
        Value* in = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 4U)));
        Value* sc = decl(ctx, k, b, tt(ctx, f, sh0(ctx)));
        Value* zp = decl(ctx, k, b, tt(ctx, i, sh0(ctx)));
        b->append(quant::build_quantize(ctx, in, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("linear")),
                                        tt(ctx, i, sh2(ctx, 4U, 4U))));
    }) == QK::SchemeInvalid);
}
