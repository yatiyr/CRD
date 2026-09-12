// CEIR-21z — the BAND-21 COMPOSING GATE (the test_band12_gate / test_band3_gate mold): the sec-21/22/35/51 high-level tensor IR
// COMPOSES. ONE module carries all THREE dialects — ceir.shape (make/rank) + ceir.tensor (the structural six) + ceir.layout
// (constrain) — chained: a rank-1 row tensor -> tensor.broadcast UP -> layout.constrain -> tensor.elementwise -> transpose ->
// matmul -> reshape -> reduce-to-RANK-0 (sec-21 !shape<>, the case CEIR-21b deferred here). The band property (the 12z
// discipline): all THREE misuse walks (find_shape_misuse + find_tensor_misuse + find_layout_misuse) + find_structure_error are
// None on the ONE module — the VERIFIERS COMPOSE (no walk falsely flags another dialect's ops) — AND the module survives BOTH
// serializers byte-clean: a TEXT round-trip (print -> parse -> re-walk all-None + print==reprint) and a BINARY round-trip
// (serialize -> deserialize -> re-walk all-None). ⛔ this is the FIRST time shape/tensor/layout ops cross the serializers.
// Device-free (crd-ceir). Test-only (a gate that forced an engine change = a 21a-c defect).

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/layout.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/shape.hpp>
#include <crd/ceir/tensor.hpp>
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
    (void)shape::register_dialect(ctx);
    (void)tensor::register_dialect(ctx);
    (void)layout::register_dialect(ctx);
}
Value* mkval(Context& ctx, OpId decl, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId shp(Context& ctx, ConstSpan<TypeId> dims) { return ctx.type_shape(dims); }
TypeId ten(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_f32(), shape); }

// Build the composing module in `m` (all three dialects, chained). Returns true if the whole chain built.
void build_band21(Context& ctx, Module& m)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const b = func::func_body_block(f);

    // shapes
    const TypeId d4 = ctx.type_dim_static(4U);
    const TypeId d3 = ctx.type_dim_static(3U);
    const TypeId s13[2] = {ctx.type_dim_static(1U), d3};
    const TypeId s43[2] = {d4, d3};
    const TypeId s34[2] = {d3, d4};
    const TypeId s44[2] = {d4, d4};
    const TypeId sh13 = shp(ctx, ConstSpan<TypeId>(s13, 2U)); // <1,3>
    const TypeId sh43 = shp(ctx, ConstSpan<TypeId>(s43, 2U)); // <4,3>
    const TypeId sh34 = shp(ctx, ConstSpan<TypeId>(s34, 2U)); // <3,4>
    const TypeId sh44 = shp(ctx, ConstSpan<TypeId>(s44, 2U)); // <4,4>
    const TypeId d16[1] = {ctx.type_dim_static(16U)};
    const TypeId sh16 = shp(ctx, ConstSpan<TypeId>(d16, 1U)); // <16>
    const TypeId sh0  = shp(ctx, ConstSpan<TypeId>{}); // <> rank-0 (the reduce target — the 21b-deferred !shape<>)

    // ── ceir.shape sub-chain (present + verified; make + rank) ──
    Value* const vd4 = mkval(ctx, decl, b, d4);
    Value* const vd3 = mkval(ctx, decl, b, d3);
    Value* const mkops[2] = {vd4, vd3};
    Operation* const sm = ctx.create_operation(ctx.intern_op("shape", "make"), ConstSpan<Value*>(mkops, 2U), 1U, sh43);
    b->append(sm);
    b->append(shape::build_rank(ctx, sm->result(0U), ctx.type_index()));

    // ── ceir.tensor + ceir.layout chain ──
    Value* const trow = mkval(ctx, decl, b, ten(ctx, sh13)); // <1,3> row
    // broadcast <1,3> UP to <4,3>
    Operation* const bc = tensor::build_broadcast(ctx, trow, ten(ctx, sh43));
    b->append(bc);
    // layout.constrain (blocked, block matches rank-2) — the passthrough that composes with the tensor ops
    Operation* const lc = layout::build_constrain(ctx, bc->result(0U), ctx.attr_string(StringView("blocked")), ten(ctx, sh43));
    ctx.set_attr(lc, StringView("block"), ctx.attr_string(StringView("2,3")));
    b->append(lc);
    // elementwise(L, L){mul} -> <4,3>
    Operation* const ew = tensor::build_elementwise(ctx, lc->result(0U), lc->result(0U), ctx.attr_string(StringView("mul")), ten(ctx, sh43));
    b->append(ew);
    // transpose{1,0} -> <3,4>
    Operation* const tr = tensor::build_transpose(ctx, ew->result(0U), ctx.attr_string(StringView("1,0")), ten(ctx, sh34));
    b->append(tr);
    // matmul(<4,3>, <3,4>) -> <4,4>
    Operation* const mm = tensor::build_matmul(ctx, ew->result(0U), tr->result(0U), ten(ctx, sh44));
    b->append(mm);
    // reshape <4,4>(16) -> <16>
    Operation* const rs = tensor::build_reshape(ctx, mm->result(0U), ten(ctx, sh16));
    b->append(rs);
    // reduce{axis=0,sum} <16> -> <> (RANK-0, the 21b-deferred !shape<> case)
    Operation* const rd = tensor::build_reduce(ctx, rs->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")), ten(ctx, sh0));
    b->append(rd);
}

// All three misuse walks + structure clean on `m`.
void require_all_clean(Context& ctx, const Module& m)
{
    REQUIRE(ctx.find_structure_error(m).kind == StructureErrorKind::None);
    REQUIRE(shape::find_shape_misuse(ctx, m).kind == shape::ShapeMisuseKind::None);
    REQUIRE(tensor::find_tensor_misuse(ctx, m).kind == tensor::TensorMisuseKind::None);
    REQUIRE(layout::find_layout_misuse(ctx, m).kind == layout::LayoutMisuseKind::None);
}
} // namespace

TEST_CASE("ceir 21z band gate: the shape/tensor/layout dialects COMPOSE + round-trip byte-clean", "[ceir][band21]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    Module* const m = ctx.create_module();
    build_band21(ctx, *m);

    // (1) the composing property: all three verifiers agree None on the ONE module (the reduce-to-rank-0 !shape<> included).
    require_all_clean(ctx, *m);

    // (2) TEXT round-trip: print -> parse -> re-walk all-None + print==reprint (5d byte-exact).
    const String text1 = print(ctx, *m, &root);
    Context      ctx_t(&root);
    register_all(ctx_t);
    const ParseResult pr = parse(ctx_t, StringView(text1.c_str(), text1.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    require_all_clean(ctx_t, *pr.module);
    const String text2 = print(ctx_t, *pr.module, &root);
    CHECK(StringView(text1.c_str(), text1.size()) == StringView(text2.c_str(), text2.size()));

    // (3) BINARY round-trip: serialize -> deserialize -> re-walk all-None (the FIRST binary crossing for these ops).
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    REQUIRE(blob.size() > 0U);
    Context ctx_b(&root);
    register_all(ctx_b);
    const ParseResult dr = deserialize(ctx_b, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);
    require_all_clean(ctx_b, *dr.module);
}
