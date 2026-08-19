// CEIR-24z — the BAND-24 COMPOSING GATE (the test_band23_gate mold): the §55 ceir.ml dialect COMPOSES + survives both serializers,
// and the §69 partitioner handles a mixed module. ONE module carries BOTH ml ops — ml.mlp (a 3-WEIGHT MLP: a VARIADIC operand tail
// LONGER than the 2-weight proof dims, uniform hidden=8) + ml.attention (single-head SDPA). The band property: find_ml_misuse +
// find_structure_error are None on the ONE module, the module survives a TEXT round-trip (print -> parse -> re-walk None +
// print==reprint) AND a BINARY round-trip (serialize -> deserialize -> re-walk None) — ⛔ the FIRST time ml.mlp's VARIADIC tail +
// ml.attention cross the binary serializer — and the §69 partitioner ASSIGNS the mixed module (coopvec CLAIMS the mlp, the
// attention FALLS BACK — no native attention kernel). Device-free (crd-ceir + the ceir-gpu partitioner), test-only. ASCII names.

#include <crd/ceir/gpu/partition_ml.hpp>

#include <crd/ceir/binary.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/ml.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
void register_all(Context& ctx)
{
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)ml::register_dialect(ctx);
}
Value* mkval(Context& ctx, OpId decl, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId sh2(Context& ctx, u32 a, u32 b)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(b)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId tf(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_f32(), shape); }

// Build the composing module in `m`: a 3-weight ml.mlp (variadic tail) + a single-head ml.attention (they COEXIST).
void build_band24(Context& ctx, Module& m)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const b = func::func_body_block(f);

    // ── ml.mlp: x[4,8] · W1[8,8] · W2[8,8] · W3[8,2] {relu} -> y[4,2] (3 weights = a variadic tail > the 2-weight proof; uniform hidden=8) ──
    Value* const x   = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const w1  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const w2  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const w3  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 2U)));
    Value*       ops[4] = {x, w1, w2, w3};
    Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(ops, 4U), 1U, tf(ctx, sh2(ctx, 4U, 2U)), 0U);
    ctx.set_attr(mo, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mo);

    // ── ml.attention: Q[2,4] · K[3,4] · V[3,2] -> out[2,2] ──
    Value* const q  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 2U, 4U)));
    Value* const ky = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 3U, 4U)));
    Value* const v  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(ml::build_attention(ctx, q, ky, v, tf(ctx, sh2(ctx, 2U, 2U))));
}

void require_clean(Context& ctx, const Module& m)
{
    REQUIRE(ctx.find_structure_error(m).kind == StructureErrorKind::None);
    REQUIRE(ml::find_ml_misuse(ctx, m).kind == ml::MlMisuseKind::None);
}
gpu::MlProvider coopvec(bool available)
{
    gpu::MlProvider p;
    p.name      = StringView("coopvec");
    p.available = available;
    p.advertise = &gpu::coopvec_can_claim_mlp;
    return p;
}
} // namespace

TEST_CASE("ceir 24z band gate: ml.mlp (variadic) + ml.attention COMPOSE, round-trip byte-clean (text + binary), and partition",
          "[ceir][band24]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    Module* const m = ctx.create_module();
    build_band24(ctx, *m);

    // (1) the composing property: find_ml + structure clean on the ONE mixed-op module.
    require_clean(ctx, *m);

    // (2) TEXT round-trip: print -> parse -> re-walk None + print==reprint (byte-exact).
    const String text1 = print(ctx, *m, &root);
    Context      ctx_t(&root);
    register_all(ctx_t);
    const ParseResult pr = parse(ctx_t, StringView(text1.c_str(), text1.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    require_clean(ctx_t, *pr.module);
    const String text2 = print(ctx_t, *pr.module, &root);
    CHECK(StringView(text1.c_str(), text1.size()) == StringView(text2.c_str(), text2.size()));

    // (3) BINARY round-trip: serialize -> deserialize -> re-walk None (the FIRST binary crossing for ml.mlp's VARIADIC tail + ml.attention).
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    REQUIRE(blob.size() > 0U);
    Context ctx_b(&root);
    register_all(ctx_b);
    const ParseResult dr = deserialize(ctx_b, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);
    require_clean(ctx_b, *dr.module);

    // (4) the §69 partitioner handles the MIXED module: coopvec CLAIMS the mlp (uniform hidden, valid dims), the attention FALLS BACK.
    const gpu::MlProvider  provs_on[1] = {coopvec(true)};
    const gpu::MlPartition p_on        = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs_on, 1U), &root);
    REQUIRE(p_on.assignments.size() == 2U);
    CHECK(p_on.claimed_by(0) == 1U); // the mlp -> coopvec
    CHECK(p_on.fallback() == 1U);    // the attention -> CKIR fallback
    // caps OFF (a non-NVIDIA device): everything falls back to the portable CKIR expansion.
    const gpu::MlProvider  provs_off[1] = {coopvec(false)};
    const gpu::MlPartition p_off        = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs_off, 1U), &root);
    CHECK(p_off.fallback() == 2U);
}
