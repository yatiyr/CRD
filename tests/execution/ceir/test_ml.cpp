// CEIR-24a (§55) — the ceir.ml dialect: ml.mlp (a multi-layer perceptron, x + a VARIADIC tail of per-layer weights) +
// ml.attention (single-head scaled dot-product attention). Both are PURE composite ops over dense rank-2 Tensors — the §69
// provider PARTITION UNIT (a provider EXPANDS the op into 22/23 vocab OR CLAIMS it whole; the mapping is 24b/24c). find_ml_misuse
// enforces the SHAPE-AWARE + ELEMENT-ROLE contract the generated verify_mlp/verify_attention cannot: Tensor-kinded operands +
// result, Float + EQUAL element, all rank-2, the mlp WIDTH CHAIN (input.dim1==W_1.dim0, W_i.dim1==W_{i+1}.dim0, output==[M, D_n]),
// the closed-vocab `activation`, and the attention Q·Kᵀ·V shape relations. A well-formed 2-layer MLP + single-head SDPA verify
// clean; every contract misuse rejects with the exact kind. ⛔ NO new TypeKind; ⛔ MODE-FREE (§56 — no inference/training mode);
// bias/dropout/gelu/mask/causal/multi-head/batched = name-forward. ASCII names.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/ml.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
using MK = crd::ceir::ml::MlMisuseKind;

namespace
{
struct Kit
{
    OpId decl;
    OpId mlp;
    explicit Kit(Context& ctx) : decl(ctx.intern_op("resource", "declare")), mlp(ctx.intern_op("ml", "mlp"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)ml::register_dialect(ctx);
    }
};
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
// Build ml.mlp with N weights (the variadic tail — build_mlp only wires 1 weight, so an N-layer MLP is a raw create_operation +
// the `activation` string attr). Operands = [input, W_1..W_n]. `weights` <= 6 (test scale).
Operation* mlp(Context& ctx, const Kit& k, Block* b, Value* input, ConstSpan<Value*> weights, TypeId result, StringView act)
{
    Value* ops[8] = {};
    ops[0] = input;
    for (u32 i = 0; i < weights.size(); ++i) { ops[1U + i] = weights[i]; }
    Operation* const op = ctx.create_operation(k.mlp, ConstSpan<Value*>(ops, 1U + weights.size()), 1U, result, 0U);
    ctx.set_attr(op, "activation", ctx.attr_string(act));
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 24a: a well-formed 2-layer MLP + single-head attention verify clean", "[ceir][ml]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  f32 = ctx.type_f32();
    Module* const                 m   = ctx.create_module();
    Block* const                  b   = mkmain(ctx, *m);

    // ml.mlp: x[4,8] · W_1[8,16] · relu · W_2[16,4] -> y[4,4] (the width chain 8->16->4, M=4 preserved).
    Value* const x  = decl(ctx, k, b, tt(ctx, f32, sh2(ctx, 4U, 8U)));
    Value* const w1 = decl(ctx, k, b, tt(ctx, f32, sh2(ctx, 8U, 16U)));
    Value* const w2 = decl(ctx, k, b, tt(ctx, f32, sh2(ctx, 16U, 4U)));
    Value* const ws[2] = {w1, w2};
    (void)mlp(ctx, k, b, x, ConstSpan<Value*>(ws, 2U), tt(ctx, f32, sh2(ctx, 4U, 4U)), StringView("relu"));

    // ml.attention: Q[6,8] · K[10,8] · V[10,4] -> out[6,4] (head D=8, seq Sk=10, Sq=6, Dv=4).
    Value* const q  = decl(ctx, k, b, tt(ctx, f32, sh2(ctx, 6U, 8U)));
    Value* const ky = decl(ctx, k, b, tt(ctx, f32, sh2(ctx, 10U, 8U)));
    Value* const v  = decl(ctx, k, b, tt(ctx, f32, sh2(ctx, 10U, 4U)));
    b->append(ml::build_attention(ctx, q, ky, v, tt(ctx, f32, sh2(ctx, 6U, 4U))));

    CHECK(ml::find_ml_misuse(ctx, *m).kind == MK::None);
}

TEST_CASE("ceir 24a: find_ml_misuse rejects every contract misuse with the exact kind", "[ceir][ml]")
{
    const auto one = [](auto build) {
        memory::GrowableTlsfAllocator root;
        Context                       ctx(&root);
        const Kit                     k(ctx);
        Module* const                 m = ctx.create_module();
        Block* const                  b = mkmain(ctx, *m);
        build(ctx, k, b);
        return ml::find_ml_misuse(ctx, *m).kind;
    };

    // --- ml.mlp misuses ---------------------------------------------------------------------------------------------------

    // OperandNotTensor: the input is a bare scalar f32, not a Tensor.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       x  = decl(ctx, k, b, f); // WRONG: a scalar, not a tensor
        Value*       w1 = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 8U, 4U)));
        Value* const ws[1] = {w1};
        (void)mlp(ctx, k, b, x, ConstSpan<Value*>(ws, 1U), tt(ctx, f, sh2(ctx, 4U, 4U)), StringView("relu"));
    }) == MK::OperandNotTensor);

    // ResultNotTensor: the MLP result type is a bare scalar f32.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       x  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 8U)));
        Value*       w1 = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 8U, 4U)));
        Value* const ws[1] = {w1};
        (void)mlp(ctx, k, b, x, ConstSpan<Value*>(ws, 1U), f, StringView("relu")); // WRONG: scalar result
    }) == MK::ResultNotTensor);

    // MlpElementMismatch: W_2 is an INTEGER tensor (not the float element the input/W_1 carry).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       x  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 8U)));
        Value*       w1 = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 8U, 16U)));
        Value*       w2 = decl(ctx, k, b, tt(ctx, i, sh2(ctx, 16U, 4U))); // WRONG: int element
        Value* const ws[2] = {w1, w2};
        (void)mlp(ctx, k, b, x, ConstSpan<Value*>(ws, 2U), tt(ctx, f, sh2(ctx, 4U, 4U)), StringView("relu"));
    }) == MK::MlpElementMismatch);

    // MlpRankInvalid: the input is rank-1 (not a rank-2 [M, D0] matrix).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       x  = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 8U))); // WRONG: rank-1
        Value*       w1 = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 8U, 4U)));
        Value* const ws[1] = {w1};
        (void)mlp(ctx, k, b, x, ConstSpan<Value*>(ws, 1U), tt(ctx, f, sh2(ctx, 4U, 4U)), StringView("relu"));
    }) == MK::MlpRankInvalid);

    // MlpArityInvalid: zero weights (the op has only the input operand — an MLP needs >=1 layer).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f    = ctx.type_f32();
        Value*       x    = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 8U)));
        Value* const no[1] = {}; // an empty weights span (0-length): the input operand is the only one
        (void)mlp(ctx, k, b, x, ConstSpan<Value*>(no, 0U), tt(ctx, f, sh2(ctx, 4U, 4U)), StringView("relu")); // WRONG: no weights
    }) == MK::MlpArityInvalid);

    // MlpWidthMismatch: W_1[8,16] then W_2[15,4] — W_1.dim1(16) != W_2.dim0(15) (the chain broke).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       x  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 8U)));
        Value*       w1 = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 8U, 16U)));
        Value*       w2 = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 15U, 4U))); // WRONG: 15 != W_1.dim1 (16)
        Value* const ws[2] = {w1, w2};
        (void)mlp(ctx, k, b, x, ConstSpan<Value*>(ws, 2U), tt(ctx, f, sh2(ctx, 4U, 4U)), StringView("relu"));
    }) == MK::MlpWidthMismatch);

    // MlpActivationInvalid: activation "gelu" is outside the closed vocab {relu}.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       x  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 4U, 8U)));
        Value*       w1 = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 8U, 4U)));
        Value* const ws[1] = {w1};
        (void)mlp(ctx, k, b, x, ConstSpan<Value*>(ws, 1U), tt(ctx, f, sh2(ctx, 4U, 4U)), StringView("gelu")); // WRONG: not {relu}
    }) == MK::MlpActivationInvalid);

    // --- ml.attention misuses ---------------------------------------------------------------------------------------------

    // AttnElementMismatch: K is an INTEGER tensor (not the float element Q/V carry).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       q  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 6U, 8U)));
        Value*       ky = decl(ctx, k, b, tt(ctx, i, sh2(ctx, 10U, 8U))); // WRONG: int element
        Value*       v  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 10U, 4U)));
        b->append(ml::build_attention(ctx, q, ky, v, tt(ctx, f, sh2(ctx, 6U, 4U))));
    }) == MK::AttnElementMismatch);

    // AttnRankInvalid: Q is rank-1 (not a rank-2 [Sq, D] matrix).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       q  = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 8U))); // WRONG: rank-1
        Value*       ky = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 10U, 8U)));
        Value*       v  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 10U, 4U)));
        b->append(ml::build_attention(ctx, q, ky, v, tt(ctx, f, sh2(ctx, 6U, 4U))));
    }) == MK::AttnRankInvalid);

    // AttnHeadDimMismatch: Q[6,8] but K[10,7] — the head dim D disagrees (8 != 7).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       q  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 6U, 8U)));
        Value*       ky = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 10U, 7U))); // WRONG: D=7 != Q's 8
        Value*       v  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 10U, 4U)));
        b->append(ml::build_attention(ctx, q, ky, v, tt(ctx, f, sh2(ctx, 6U, 4U))));
    }) == MK::AttnHeadDimMismatch);

    // AttnSeqMismatch: K[10,8] but V[9,4] — the key/value seq Sk disagrees (10 != 9).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       q  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 6U, 8U)));
        Value*       ky = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 10U, 8U)));
        Value*       v  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 9U, 4U))); // WRONG: Sk=9 != K's 10
        b->append(ml::build_attention(ctx, q, ky, v, tt(ctx, f, sh2(ctx, 6U, 4U))));
    }) == MK::AttnSeqMismatch);

    // AttnOutShapeMismatch: out[5,4] but Sq=6 (out.dim0 must equal Q's row count).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        Value*       q  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 6U, 8U)));
        Value*       ky = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 10U, 8U)));
        Value*       v  = decl(ctx, k, b, tt(ctx, f, sh2(ctx, 10U, 4U)));
        b->append(ml::build_attention(ctx, q, ky, v, tt(ctx, f, sh2(ctx, 5U, 4U)))); // WRONG: Sq=5 != Q's 6
    }) == MK::AttnOutShapeMismatch);
}
