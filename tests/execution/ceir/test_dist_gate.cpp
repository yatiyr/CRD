// CEIR-30a-1 — the ceir.dist dialect (sec-68/sec-103; proof sec-140): the STRUCTURAL THREE placement ops (dist.mesh /
// dist.shard / dist.all_reduce) over the CEIR-3d Tensor TYPE + the semantic find_dist_misuse. Device-free (crd-ceir).
// Proves: (1) a well-formed mesh->shard->all_reduce module verifies clean AND survives a TEXT round-trip (print->parse->
// re-walk None + print==reprint — the dist ops serialize); (2) each semantic misuse is REJECTED with the EXACT
// DistMisuseKind AND the offending OP identity (not a count): MeshShapeInvalid, DuplicateMesh, UnknownMesh,
// OperandNotTensor, ResultTypeMismatch (the sec-70 placement-PRESERVES-the-tensor invariant), ShardAxisInvalid,
// MeshAxisInvalid, FnInvalid (mean is REJECTED — tensor.reduce's vocab minus mean, the 30a-3 post-scale named-forward);
// (3) the COMMITTED assets/ceir/dist_shard_reduce.ceir is anti-drift-through-the-PRINTER vs the builder oracle +
// roundtrip-stable. ⛔ DECLARE-only: typed NoSemantics + NO kernel_ref (sec-70); the lowering (propagation 30a-2 +
// materialization 30a-3) is name-forward. ⛔ TEST NAMES STAY ASCII (a non-ASCII char mangles the ctest->Catch2 filter).

#include <crd/ceir/dist.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp> // register_resource_ops (resource.declare — the typed-value seed)
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream> // slurp the committed dist .ceir asset

// ⛔ NO `#ifndef CRD_REPO_DIR / #define "."` fallback -- the CMakeLists (crd-ceir-tests PRIVATE CRD_REPO_DIR) ALWAYS defines
// it; a "." fallback is the cwd-luck scar pre-armed (Win-greens on ./assets, WSL-reds). A missing define must fail LOUD.

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
void register_dist_all(Context& ctx)
{
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)dist::register_dist_ops(ctx);
}

struct DistKit
{
    OpId decl;
    explicit DistKit(Context& ctx) : decl(ctx.intern_op("resource", "declare")) { register_dist_all(ctx); }
};

Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m.body()->append(top);
    }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
// a resource.declare producing a value of type `t` (the typed-value seed — the test_tensor mold).
Value* mkval(Context& ctx, const DistKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId tensor48(Context& ctx) { return ctx.type_tensor(ctx.type_f32(), sh2(ctx, 4U, 8U)); }

// The ORACLE: module { func main { dist.mesh @m2 "2"; %0 = declare tensor<f32,[4,8]>; %1 = shard(%0) {@m2,0,0};
// %2 = all_reduce(%1) {@m2,sum} } }. The anti-drift SOURCE for the committed asset — keep it, do NOT delete.
Module* build_dist_module(Context& ctx, const DistKit& k)
{
    Module* const m  = ctx.create_module();
    Block* const  b  = mkmain(ctx, *m);
    const TypeId  tt = tensor48(ctx);
    b->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
    Value* const     in = mkval(ctx, k, b, tt);
    Operation* const sh =
        dist::build_shard(ctx, in, ctx.attr_symbol(StringView("m2")), ctx.attr_int(0), ctx.attr_int(0), tt);
    b->append(sh);
    Operation* const ar =
        dist::build_all_reduce(ctx, sh->result(0U), ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("sum")), tt);
    b->append(ar);
    return m;
}
} // namespace

TEST_CASE("ceir 30a-1: a well-formed dist module verifies and survives a text round-trip", "[ceir][dist]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const DistKit                 k(ctx);
    Module* const                 m = build_dist_module(ctx, k);

    CHECK(dist::find_dist_misuse(ctx, *m).kind == dist::DistMisuseKind::None);

    // TEXT round-trip: the dist ops PRINT + re-PARSE to the identical canonical text and re-walk clean.
    const containers::String t0 = print(ctx, *m, &root);
    Context                  ctx2(&root);
    register_dist_all(ctx2);
    const ParseResult pr = parse(ctx2, StringView(t0.c_str(), t0.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    CHECK(dist::find_dist_misuse(ctx2, *pr.module).kind == dist::DistMisuseKind::None);
    const containers::String t1 = print(ctx2, *pr.module, &root);
    CHECK(StringView(t1.c_str(), t1.size()) == StringView(t0.c_str(), t0.size()));

    // the collective fn vocab ADMITS all of {sum,prod,max,min} — a 4-way identity so collective_fn_in can't pass the suite
    // by matching "sum" alone (the mean REJECTION is the misuse TEST_CASE; this is its acceptance counterpart).
    const StringView fns[4] = {StringView("sum"), StringView("prod"), StringView("max"), StringView("min")};
    for (const StringView fn : fns)
    {
        Module* const mm = ctx.create_module();
        Block* const  bb = mkmain(ctx, *mm);
        const TypeId  tt = tensor48(ctx);
        bb->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
        Value* const in = mkval(ctx, k, bb, tt);
        bb->append(dist::build_all_reduce(ctx, in, ctx.attr_symbol(StringView("m2")), ctx.attr_string(fn), tt));
        CHECK(dist::find_dist_misuse(ctx, *mm).kind == dist::DistMisuseKind::None);
    }
}

TEST_CASE("ceir 30a-1: each dist misuse is rejected with the exact kind and offending op", "[ceir][dist]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const DistKit                 k(ctx);
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    const TypeId                  tt = tensor48(ctx);

    SECTION("mesh-shape-invalid: a non-positive mesh dim")
    {
        Operation* const mesh = dist::build_mesh(ctx, ctx.attr_symbol(StringView("m0")), ctx.attr_string(StringView("0")));
        b->append(mesh);
        const dist::DistMisuse e = dist::find_dist_misuse(ctx, *m);
        CHECK(e.kind == dist::DistMisuseKind::MeshShapeInvalid);
        CHECK(e.op == mesh);
    }
    SECTION("duplicate-mesh: two meshes share a name")
    {
        b->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
        Operation* const dup = dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("4")));
        b->append(dup);
        const dist::DistMisuse e = dist::find_dist_misuse(ctx, *m);
        CHECK(e.kind == dist::DistMisuseKind::DuplicateMesh);
        CHECK(e.op == dup); // the SECOND occurrence
    }
    SECTION("unknown-mesh: a shard references an undeclared mesh")
    {
        b->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
        Value* const     in = mkval(ctx, k, b, tt);
        Operation* const sh =
            dist::build_shard(ctx, in, ctx.attr_symbol(StringView("nope")), ctx.attr_int(0), ctx.attr_int(0), tt);
        b->append(sh);
        const dist::DistMisuse e = dist::find_dist_misuse(ctx, *m);
        CHECK(e.kind == dist::DistMisuseKind::UnknownMesh);
        CHECK(e.op == sh);
    }
    SECTION("operand-not-tensor: a shard on a non-tensor value")
    {
        b->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
        Value* const     in = mkval(ctx, k, b, ctx.type_i32());
        Operation* const sh = dist::build_shard(ctx, in, ctx.attr_symbol(StringView("m2")), ctx.attr_int(0),
                                                ctx.attr_int(0), ctx.type_i32());
        b->append(sh);
        const dist::DistMisuse e = dist::find_dist_misuse(ctx, *m);
        CHECK(e.kind == dist::DistMisuseKind::OperandNotTensor);
        CHECK(e.op == sh);
        CHECK(e.value == in);
    }
    SECTION("result-type-mismatch: a shard whose result type differs from its input")
    {
        b->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
        Value* const     in  = mkval(ctx, k, b, tt);
        const TypeId     bad = ctx.type_tensor(ctx.type_f32(), sh2(ctx, 4U, 4U)); // != input [4,8]
        Operation* const sh =
            dist::build_shard(ctx, in, ctx.attr_symbol(StringView("m2")), ctx.attr_int(0), ctx.attr_int(0), bad);
        b->append(sh);
        const dist::DistMisuse e = dist::find_dist_misuse(ctx, *m);
        CHECK(e.kind == dist::DistMisuseKind::ResultTypeMismatch);
        CHECK(e.op == sh);
    }
    SECTION("shard-axis-invalid: a tensor axis out of range")
    {
        b->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
        Value* const     in = mkval(ctx, k, b, tt);
        Operation* const sh = dist::build_shard(ctx, in, ctx.attr_symbol(StringView("m2")), ctx.attr_int(5), // rank 2
                                                ctx.attr_int(0), tt);
        b->append(sh);
        const dist::DistMisuse e = dist::find_dist_misuse(ctx, *m);
        CHECK(e.kind == dist::DistMisuseKind::ShardAxisInvalid);
        CHECK(e.op == sh);
    }
    SECTION("mesh-axis-invalid: a mesh axis beyond the mesh rank")
    {
        b->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2")))); // rank 1
        Value* const     in = mkval(ctx, k, b, tt);
        Operation* const sh = dist::build_shard(ctx, in, ctx.attr_symbol(StringView("m2")), ctx.attr_int(0),
                                                ctx.attr_int(3), tt); // mesh_axis 3 >= 1
        b->append(sh);
        const dist::DistMisuse e = dist::find_dist_misuse(ctx, *m);
        CHECK(e.kind == dist::DistMisuseKind::MeshAxisInvalid);
        CHECK(e.op == sh);
    }
    SECTION("fn-invalid: mean is not a collective fn")
    {
        b->append(dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
        Value* const     in = mkval(ctx, k, b, tt);
        Operation* const ar =
            dist::build_all_reduce(ctx, in, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("mean")), tt);
        b->append(ar);
        const dist::DistMisuse e = dist::find_dist_misuse(ctx, *m);
        CHECK(e.kind == dist::DistMisuseKind::FnInvalid); // mean is in tensor.reduce's vocab but NOT the collective's
        CHECK(e.op == ar);
    }
}

TEST_CASE("ceir 30a-1: the committed dist_shard_reduce.ceir is anti-drift and roundtrip-stable", "[ceir][dist]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const DistKit                 k(ctx);

    // the in-memory ORACLE + its canonical print (the anti-drift SOURCE — regenerate the asset if build_dist_module changes).
    Module* const            built   = build_dist_module(ctx, k);
    const containers::String t_built = print(ctx, *built, &root);

    // parse-load the COMMITTED asset.
    std::ifstream f(CRD_REPO_DIR "/assets/ceir/dist_shard_reduce.ceir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    containers::Array<char> src(ctx.allocator());
    src.resize(static_cast<usize>(sz), '\0');
    f.read(src.data(), sz);

    const ParseResult pr = parse(ctx, StringView(src.data(), src.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ANTI-DRIFT: the committed file's canonical print == the oracle's (never file-bytes — regenerate on a builder change).
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ROUNDTRIP stability: print(parse(file)) re-parses to the identical canonical text (the committed file IS canonical).
    Context ctx2(&root);
    register_dist_all(ctx2);
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

    // the loaded asset is well-formed BOTH ways: semantically (find_dist_misuse) AND structurally (find_structure_error --
    // the func body is a Graph region [no terminator required], the sharded reduction a pure DAG; the 31a-1a-ii mold parity).
    CHECK(dist::find_dist_misuse(ctx, *pr.module).kind == dist::DistMisuseKind::None);
    CHECK(ctx.find_structure_error(*pr.module).kind == StructureErrorKind::None);
}
