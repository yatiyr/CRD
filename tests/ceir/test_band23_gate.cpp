// CEIR-23z — the BAND-23 COMPOSING GATE (the test_band21_gate mold): the §53/§54 quant + sparse dialects COMPOSE + survive both
// serializers. ONE module carries BOTH dialects — quant.quantize + quant.dequantize (per-tensor, i8 storage) + sparse.spmv (CSR,
// its inputs are declares, so it only needs to COEXIST — no dataflow to the quant ops). The band property: BOTH misuse walks
// (find_quant_misuse + find_sparse_misuse) + find_structure_error are None on the ONE module (the verifiers COMPOSE — neither
// falsely flags the other's ops), AND the module survives BOTH serializers byte-clean: a TEXT round-trip (print -> parse ->
// re-walk all-None + print==reprint) and a BINARY round-trip (serialize -> deserialize -> re-walk all-None). ⛔ this is the FIRST
// time the quant.quantize/dequantize + sparse.spmv OPS cross the serializers (the 21z contract). Device-free (crd-ceir), test-only.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/quant.hpp>
#include <crd/ceir/sparse.hpp>
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
    (void)quant::register_dialect(ctx);
    (void)sparse::register_dialect(ctx);
}
Value* mkval(Context& ctx, OpId decl, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId shp(Context& ctx, ConstSpan<TypeId> dims) { return ctx.type_shape(dims); }
TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return shp(ctx, ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 b)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(b)};
    return shp(ctx, ConstSpan<TypeId>(d, 2U));
}
TypeId tt(Context& ctx, TypeId elem, TypeId shape) { return ctx.type_tensor(elem, shape); }

// Build the composing module in `m`: quantize + dequantize + spmv, all fed by resource.declare (they COEXIST).
void build_band23(Context& ctx, Module& m)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const b = func::func_body_block(f);

    const TypeId f32 = ctx.type_f32();
    const TypeId i8  = ctx.type_int(8U, true);
    const TypeId i32 = ctx.type_int(32U, true);
    const TypeId r0  = shp(ctx, ConstSpan<TypeId>{});

    // ── quant.quantize + quant.dequantize (per-tensor, i8 storage) ──
    Value* const     qin = mkval(ctx, decl, b, tt(ctx, f32, sh2(ctx, 4U, 4U)));
    Value* const     sc  = mkval(ctx, decl, b, tt(ctx, f32, r0));
    Value* const     zp  = mkval(ctx, decl, b, tt(ctx, i8, r0));
    Operation* const q   = quant::build_quantize(ctx, qin, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                                 tt(ctx, i8, sh2(ctx, 4U, 4U)));
    b->append(q);
    Value* const     din = mkval(ctx, decl, b, tt(ctx, i8, sh2(ctx, 4U, 4U)));
    Operation* const dq  = quant::build_dequantize(ctx, din, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("asymmetric")),
                                                   tt(ctx, f32, sh2(ctx, 4U, 4U)));
    b->append(dq);

    // ── sparse.spmv (CSR: row_ptr i32[5], col_idx i32[9], values f32[9], x f32[6] -> y f32[4]) ──
    Value* const     rp = mkval(ctx, decl, b, tt(ctx, i32, sh1(ctx, 5U)));
    Value* const     ci = mkval(ctx, decl, b, tt(ctx, i32, sh1(ctx, 9U)));
    Value* const     vl = mkval(ctx, decl, b, tt(ctx, f32, sh1(ctx, 9U)));
    Value* const     xv = mkval(ctx, decl, b, tt(ctx, f32, sh1(ctx, 6U)));
    Operation* const sp = sparse::build_spmv(ctx, rp, ci, vl, xv, tt(ctx, f32, sh1(ctx, 4U)));
    b->append(sp);
}

// Both misuse walks + structure clean on `m`.
void require_all_clean(Context& ctx, const Module& m)
{
    REQUIRE(ctx.find_structure_error(m).kind == StructureErrorKind::None);
    REQUIRE(quant::find_quant_misuse(ctx, m).kind == quant::QuantMisuseKind::None);
    REQUIRE(sparse::find_sparse_misuse(ctx, m).kind == sparse::SparseMisuseKind::None);
}
} // namespace

TEST_CASE("ceir 23z band gate: the quant + sparse dialects COMPOSE + round-trip byte-clean (text + binary)", "[ceir][band23]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    Module* const m = ctx.create_module();
    build_band23(ctx, *m);

    // (1) the composing property: both verifiers agree None on the ONE module (neither flags the other dialect's ops).
    require_all_clean(ctx, *m);

    // (2) TEXT round-trip: print -> parse -> re-walk all-None + print==reprint (byte-exact).
    const String text1 = print(ctx, *m, &root);
    Context      ctx_t(&root);
    register_all(ctx_t);
    const ParseResult pr = parse(ctx_t, StringView(text1.c_str(), text1.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    require_all_clean(ctx_t, *pr.module);
    const String text2 = print(ctx_t, *pr.module, &root);
    CHECK(StringView(text1.c_str(), text1.size()) == StringView(text2.c_str(), text2.size()));

    // (3) BINARY round-trip: serialize -> deserialize -> re-walk all-None (the FIRST binary crossing for quant + sparse ops).
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    REQUIRE(blob.size() > 0U);
    Context ctx_b(&root);
    register_all(ctx_b);
    const ParseResult dr = deserialize(ctx_b, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);
    require_all_clean(ctx_b, *dr.module);
}
