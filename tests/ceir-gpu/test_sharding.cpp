// CEIR-30a-2 — the sharding PROPAGATION analysis (sec-103): propagate_sharding computes a Value->Sharding map by ONE forward
// pass over dist.shard seeds + tensor ops. Device-free (crd-ceir-gpu). Proves the lattice + rules by IDENTITY on every field:
// (a) the sec-140 chain declare->shard(axis0)->reduce(axis0,sum)->all_reduce(sum) = Sharded / Partial{axis0,sum} / Replicated;
// (b) a reduce on the OTHER axis re-indexes the sharded axis (shard axis1, reduce axis0 -> Sharded axis0); (c) all_reduce with a
// fn != the Partial's fn -> Conflict; (d) all_reduce on a Sharded (un-reduced) input -> Conflict (the all_gather negative, ledgered);
// (e) elementwise of two equal Partials -> Partial, and Sharded x Replicated -> Sharded (the meet); (f) an unknown tensor op
// (matmul) -> Conflict, NEVER a silent Replicated (the sec-70 no-lie rule). ⛔ ASCII test names.
// CEIR-30b-3a — lower_sharded_reduction: LOWER a sec-140 sharded reduction into an explicit per-rank plan + a placement PRODUCER
// (rank_lineage), proven device-free — the split (2 tagged reduces + a combine, placed [Host,Gpu,Host], transfers by IDENTITY,
// all-Host bit-exact vs a two-stage f32 oracle that DIFFERS from a flat fold), the 1-device byte-identical identity, and the refuse.

#include <crd/ceir/gpu/sharding.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/dist.hpp>                    // register_dist_ops + build_mesh/shard/all_reduce
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>        // register_resource_ops (resource.declare — the typed-value seed)
#include <crd/ceir/gen/transform_ops.hpp>       // CEIR-30c: register_transform_ops + build_place_mesh (the authored placement directive)
#include <crd/ceir/gpu/partition_ml.hpp>        // CEIR-30b-3a: MlPartition/MlAssignment/MlProvider + 30c Placement/placement_from_transform
#include <crd/ceir/gpu/tensor_pipeline.hpp>     // CEIR-30b-3a: plan_tensor_pipeline_partitioned + StageKind (the lowered plan shape)
#include <crd/ceir/gpu/tensor_pipeline_exec.hpp> // CEIR-30b-3a: execute_tensor_pipeline_host / plan_transfers / stage_class_from_partition
#include <crd/ceir/parse.hpp>                   // CEIR-30c-2: parse + ParseResult (load the committed placement .ceir asset)
#include <crd/ceir/print.hpp>                   // print — the 30a-3 + 30b-3a 1-device identity (byte-identical) gate + 30c-2 anti-drift
#include <crd/ceir/semantics.hpp>               // ProviderClass (§69 Host/Gpu) — the two-class placement axis
#include <crd/ceir/tensor.hpp>                  // register_dialect + build_elementwise/reduce/matmul
#include <crd/ceir/transform.hpp>               // CEIR-30c: find_transform_misuse (the duplicate-directive guard)
#include <crd/ceir/type.hpp>

#include <crd/containers/hash_map.hpp> // CEIR-30b-3a: the rank_lineage out-param (const Operation* -> rank)

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include "../gpu-shared/ceir_asset_slurp.hpp" // CEIR-30c-2: slurp_asset — the shared committed-asset reader (hoisted from band24)

#include <catch2/catch_test_macros.hpp>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "." // CEIR-30c-2: overridden by the crd-ceir-gpu-tests target (repo root) — the committed-asset path base
#endif

using namespace crd;            // NOLINT(google-build-using-namespace)
using namespace crd::ceir;      // NOLINT(google-build-using-namespace)
using namespace crd::ceir::gpu; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
struct DistKit
{
    OpId decl;
    explicit DistKit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)tensor::register_dialect(ctx);
        (void)dist::register_dist_ops(ctx);
        (void)transform::register_transform_ops(ctx); // CEIR-30c: the transform.place_mesh directive
    }
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
Value* mkval(Context& ctx, const DistKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId sh0(Context& ctx) { const TypeId d[1] = {}; return ctx.type_shape(ConstSpan<TypeId>(d, 0U)); } // rank-0 (scalar)
TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return ctx.type_shape(ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId tf(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_f32(), shape); }

AttrId sym(Context& ctx, const char* s) { return ctx.attr_symbol(StringView(s)); }
AttrId str(Context& ctx, const char* s) { return ctx.attr_string(StringView(s)); }

// resource.export(%v) — a boundary op with NO result: an ESCAPE consumer (not all_reduce/reduce/elementwise) for the 30a-3 gate.
Operation* mk_export(Context& ctx, Block* b, Value* v)
{
    Value* ops[1] = {v};
    Operation* const e = ctx.create_operation(ctx.intern_op("resource", "export"), ConstSpan<Value*>(ops, 1U), 0U);
    b->append(e);
    return e;
}
u32 count_named(Context& ctx, Region* r, StringView name) // NOLINT(misc-no-recursion)
{
    u32 n = 0;
    if (r == nullptr) { return n; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == name) { ++n; }
            for (u32 i = 0; i < op->num_regions(); ++i) { n += count_named(ctx, op->region(i), name); }
        }
    }
    return n;
}
Operation* first_named(Context& ctx, Region* r, StringView name) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return nullptr; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == name) { return op; }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                Operation* const f = first_named(ctx, op->region(i), name);
                if (f != nullptr) { return f; }
            }
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("ceir 30a-2: sharding propagation over the dist + tensor ops", "[ceir][dist][sharding]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const DistKit                 k(ctx);
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    const TypeId                  t48 = tf(ctx, sh2(ctx, 4U, 8U));
    const TypeId                  t8  = tf(ctx, sh1(ctx, 8U));

    SECTION("the sec-140 chain: declare -> shard(axis0) -> reduce(axis0,sum) -> all_reduce(sum)")
    {
        Operation* const mesh = dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2"));
        b->append(mesh);
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rd);
        Operation* const ar = dist::build_all_reduce(ctx, rd->result(0U), sym(ctx, "m2"), str(ctx, "sum"), t8);
        b->append(ar);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        const Sharding    s  = sm.of(sh->result(0U));
        CHECK(s.kind == ShardingKind::Sharded);
        CHECK(s.mesh == mesh);
        CHECK(s.axis == 0);
        CHECK(s.mesh_axis == 0);
        const Sharding p = sm.of(rd->result(0U));
        CHECK(p.kind == ShardingKind::Partial); // reduce OVER the split axis
        CHECK(p.mesh == mesh);
        CHECK(p.axis == 0);
        CHECK(p.mesh_axis == 0);
        CHECK(p.fn == StringView("sum"));
        CHECK(sm.of(ar->result(0U)).kind == ShardingKind::Replicated); // all_reduce COMPLETES the partial
    }
    SECTION("reduce on the OTHER axis re-indexes: shard(axis1), reduce(axis0) -> Sharded(axis0)")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(1), ctx.attr_int(0), t48); // axis 1
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8); // reduce axis 0
        b->append(rd);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        const Sharding    s  = sm.of(rd->result(0U));
        CHECK(s.kind == ShardingKind::Sharded); // NOT partial — the reduced axis is not the sharded one
        CHECK(s.axis == 0);                      // the sharded axis 1 shifts down to 0 (axis 0 was dropped)
        CHECK(s.mesh_axis == 0);
    }
    SECTION("all_reduce fn != the Partial's fn -> Conflict")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rd);
        Operation* const ar = dist::build_all_reduce(ctx, rd->result(0U), sym(ctx, "m2"), str(ctx, "max"), t8); // max != sum
        b->append(ar);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        CHECK(sm.of(ar->result(0U)).kind == ShardingKind::Conflict);
    }
    SECTION("all_reduce on a Sharded (un-reduced) input -> Conflict (the all_gather negative)")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const ar = dist::build_all_reduce(ctx, sh->result(0U), sym(ctx, "m2"), str(ctx, "sum"), t48); // Sharded in
        b->append(ar);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        CHECK(sm.of(ar->result(0U)).kind == ShardingKind::Conflict);
    }
    SECTION("elementwise: two equal Partials -> Partial; Sharded x Replicated -> Sharded")
    {
        Operation* const mesh = dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2"));
        b->append(mesh);
        // two Partials (each declare -> shard(axis0) -> reduce(axis0,sum))
        Value* const     xa  = mkval(ctx, k, b, t48);
        Operation* const sha = dist::build_shard(ctx, xa, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sha);
        Operation* const rda = tensor::build_reduce(ctx, sha->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rda);
        Value* const     xb  = mkval(ctx, k, b, t48);
        Operation* const shb = dist::build_shard(ctx, xb, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(shb);
        Operation* const rdb = tensor::build_reduce(ctx, shb->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rdb);
        Operation* const ew = tensor::build_elementwise(ctx, rda->result(0U), rdb->result(0U), str(ctx, "add"), t8);
        b->append(ew);
        // Sharded x Replicated: a sharded value + a plain (Replicated) declare
        Value* const     xs  = mkval(ctx, k, b, t48);
        Operation* const shs = dist::build_shard(ctx, xs, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(shs);
        Value* const     rep = mkval(ctx, k, b, t48); // a plain declare — Replicated
        Operation* const ew2 = tensor::build_elementwise(ctx, shs->result(0U), rep, str(ctx, "add"), t48);
        b->append(ew2);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        const Sharding    p  = sm.of(ew->result(0U));
        CHECK(p.kind == ShardingKind::Partial); // meet(Partial, Partial) with equal mesh/axis/fn
        CHECK(p.mesh == mesh);
        CHECK(p.fn == StringView("sum"));
        const Sharding s = sm.of(ew2->result(0U));
        CHECK(s.kind == ShardingKind::Sharded); // meet(Sharded, Replicated) -> Sharded (Replicated is bottom)
        CHECK(s.axis == 0);
        CHECK(s.mesh_axis == 0);
    }
    SECTION("elementwise of two DIFFERENT-axis Shardeds -> Conflict (the meet is not axis-blind)")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     xa  = mkval(ctx, k, b, t48);
        Operation* const sha = dist::build_shard(ctx, xa, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48); // axis 0
        b->append(sha);
        Value* const     xb  = mkval(ctx, k, b, t48);
        Operation* const shb = dist::build_shard(ctx, xb, sym(ctx, "m2"), ctx.attr_int(1), ctx.attr_int(0), t48); // axis 1
        b->append(shb);
        Operation* const ew = tensor::build_elementwise(ctx, sha->result(0U), shb->result(0U), str(ctx, "add"), t48);
        b->append(ew);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        CHECK(sm.of(ew->result(0U)).kind == ShardingKind::Conflict); // a meet that ignored the axis would pass as Sharded
    }
    SECTION("an unknown tensor op (matmul) -> Conflict, never a silent Replicated")
    {
        Value* const     a  = mkval(ctx, k, b, t48);              // [4,8]
        Value* const     bb = mkval(ctx, k, b, tf(ctx, sh2(ctx, 8U, 4U))); // [8,4]
        Operation* const mm = tensor::build_matmul(ctx, a, bb, tf(ctx, sh2(ctx, 4U, 4U)));
        b->append(mm);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        CHECK(sm.of(mm->result(0U)).kind == ShardingKind::Conflict); // no rule for matmul yet (sec-70 no-lie)
    }
    SECTION("shard on a 1-device mesh -> Replicated (the 1-device identity is an analysis fact)")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m1"), str(ctx, "1"))); // a SINGLE-device mesh
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m1"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        CHECK(sm.of(sh->result(0U)).kind == ShardingKind::Replicated); // a size-1 mesh axis is degenerate — no sharding
    }
    SECTION("1-device mesh: an authored shard -> reduce -> all_reduce stays Replicated end-to-end (NOT Conflict)")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m1"), str(ctx, "1")));
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m1"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), tf(ctx, sh1(ctx, 8U)));
        b->append(rd);
        Operation* const ar = dist::build_all_reduce(ctx, rd->result(0U), sym(ctx, "m1"), str(ctx, "sum"), tf(ctx, sh1(ctx, 8U)));
        b->append(ar);

        const ShardingMap sm = propagate_sharding(ctx, *m, &root);
        CHECK(sm.of(sh->result(0U)).kind == ShardingKind::Replicated);
        CHECK(sm.of(rd->result(0U)).kind == ShardingKind::Replicated);
        CHECK(sm.of(ar->result(0U)).kind == ShardingKind::Replicated); // all_reduce over 1 device = identity, NOT Conflict
    }
}

TEST_CASE("ceir 30a-3: materialize_sharding auto-inserts all_reduce where a Partial escapes", "[ceir][dist][sharding]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const DistKit                 k(ctx);
    Module* const                 m   = ctx.create_module();
    Block* const                  b   = mkmain(ctx, *m);
    const TypeId                  t48 = tf(ctx, sh2(ctx, 4U, 8U));
    const TypeId                  t8  = tf(ctx, sh1(ctx, 8U));

    SECTION("auto-insert: shard -> reduce -> export gains a dist.all_reduce reading the reduce, feeding the export")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rd);
        Operation* const ex = mk_export(ctx, b, rd->result(0U));

        const MaterializeResult res = materialize_sharding(ctx, *m, &root);
        CHECK(res.inserted == 1U);
        CHECK_FALSE(res.had_conflict);
        Operation* const ar = first_named(ctx, m->body(), StringView("dist.all_reduce"));
        REQUIRE(ar != nullptr);
        CHECK(ar->operand(0U) == rd->result(0U)); // reads the reduce's partial
        CHECK(ex->operand(0U) == ar->result(0U)); // the export now reads the COMPLETED value
        CHECK(ctx.attr_value(ar->attr("fn")).s == StringView("sum"));
        CHECK(ctx.attr_value(ar->attr("mesh")).s == StringView("m2"));
    }
    SECTION("1-device identity: mesh 1 inserts nothing and the module is byte-identical")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m1"), str(ctx, "1"))); // single-device mesh
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m1"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rd);
        (void)mk_export(ctx, b, rd->result(0U));

        const containers::String before = print(ctx, *m, &root);
        const MaterializeResult  res    = materialize_sharding(ctx, *m, &root);
        const containers::String after  = print(ctx, *m, &root);
        CHECK(res.inserted == 0U);
        CHECK(StringView(after.c_str(), after.size()) == StringView(before.c_str(), before.size())); // NO collective inserted
    }
    SECTION("an authored all_reduce is respected (no double-insert)")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rd);
        Operation* const ar = dist::build_all_reduce(ctx, rd->result(0U), sym(ctx, "m2"), str(ctx, "sum"), t8);
        b->append(ar);
        (void)mk_export(ctx, b, ar->result(0U));

        const MaterializeResult res = materialize_sharding(ctx, *m, &root);
        CHECK(res.inserted == 0U);                                                // the partial is already completed
        CHECK(count_named(ctx, m->body(), StringView("dist.all_reduce")) == 1U);  // still only the authored one
    }
    SECTION("refuse past a Conflict: an fn-mismatch all_reduce -> had_conflict, no insert")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rd);
        Operation* const ar = dist::build_all_reduce(ctx, rd->result(0U), sym(ctx, "m2"), str(ctx, "max"), t8); // max != sum
        b->append(ar);
        (void)mk_export(ctx, b, ar->result(0U));

        const MaterializeResult res = materialize_sharding(ctx, *m, &root);
        CHECK(res.had_conflict);
        CHECK(res.inserted == 0U);
    }
    SECTION("chain: reduce -> reduce -> export inserts ONE all_reduce after the SECOND reduce")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x   = mkval(ctx, k, b, t48);
        Operation* const sh  = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd1 = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rd1);
        Operation* const rd2 = tensor::build_reduce(ctx, rd1->result(0U), ctx.attr_int(0), str(ctx, "sum"), tf(ctx, sh0(ctx)));
        b->append(rd2);
        (void)mk_export(ctx, b, rd2->result(0U));

        const MaterializeResult res = materialize_sharding(ctx, *m, &root);
        CHECK(res.inserted == 1U); // rd1's partial COMPOSES into rd2 (not an escape); only rd2's partial escapes
        Operation* const ar = first_named(ctx, m->body(), StringView("dist.all_reduce"));
        REQUIRE(ar != nullptr);
        CHECK(ar->operand(0U) == rd2->result(0U));
    }
    SECTION("mixed use: RAUW-all completes the COMPOSING use too (per-use repoint is the ledgered optimization)")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x   = mkval(ctx, k, b, t48);
        Operation* const sh  = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t48);
        b->append(sh);
        Operation* const rd  = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t8);
        b->append(rd);
        Operation* const rd2 = tensor::build_reduce(ctx, rd->result(0U), ctx.attr_int(0), str(ctx, "sum"), tf(ctx, sh0(ctx)));
        b->append(rd2);                          // a COMPOSING use of rd's partial
        (void)mk_export(ctx, b, rd->result(0U)); // an ESCAPING use of the SAME partial
        (void)mk_export(ctx, b, rd2->result(0U));

        const MaterializeResult res = materialize_sharding(ctx, *m, &root);
        CHECK(res.inserted == 1U);
        Operation* const ar = first_named(ctx, m->body(), StringView("dist.all_reduce"));
        REQUIRE(ar != nullptr);
        CHECK(rd2->operand(0U) == ar->result(0U)); // the composing reduce2 now reads the COMPLETED value (RAUW-all policy)
    }
}

namespace
{
using crd::containers::Array;
using crd::containers::HashMap;

// collect every op named `name` (pre-order) — the shard declares / reduces the lowered module carries, in creation order.
void collect_named(Context& ctx, Region* r, StringView name, Array<Operation*>& out) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == name) { out.push_back(op); }
            for (u32 i = 0; i < op->num_regions(); ++i) { collect_named(ctx, op->region(i), name, out); }
        }
    }
}
// a LEFT-FOLD f32 sum of `n` values at `base` strided by `stride` — matches eval_cpu's sequential ReduceSum order (the 30b-1 reduce
// gate proved eval_cpu's reduce IS a sequential f32 sum), so the oracle is BIT-EXACT with the lowered plan's Host run.
[[nodiscard]] f32 fold_sum(const f32* base, u32 n, u32 stride)
{
    f32 acc = base[0];
    for (u32 i = 1; i < n; ++i) { acc = acc + base[static_cast<usize>(i) * stride]; }
    return acc;
}
struct Seed
{
    const Value* value  = nullptr;
    const f32*   floats = nullptr;
    u32          count  = 0;
};
// Run a plan ALL on the Host: one f32 region per plan buffer (aliases share the landlord's slot), seed ExternalIn by SSA value,
// execute, copy the Output-role buffer to `out`. The device-free 30b-3a placement proof (the 30b-1 run_host mold, minimized).
ExecuteError run_all_host(Context& ctx, memory::IAllocator* alloc, const TensorPipelinePlan& plan, const Seed* seeds,
                          usize n_seeds, f32* out, usize out_len)
{
    const usize nb = plan.buffers.size();
    REQUIRE(nb <= 40U);
    u64 offs[40] = {};
    u64 total    = 0;
    for (usize i = 0; i < nb; ++i)
    {
        offs[i] = total;
        if (plan.buffers[i].alias_of < 0) { total += plan.buffers[i].bytes / sizeof(f32); }
    }
    Array<f32> store(alloc);
    store.resize(static_cast<usize>(total), 0.0F);
    f32* ptr[40] = {};
    for (usize i = 0; i < nb; ++i)
    {
        ptr[i] = plan.buffers[i].alias_of >= 0 ? ptr[static_cast<usize>(plan.buffers[i].alias_of)] : store.data() + offs[i];
    }
    for (usize i = 0; i < nb; ++i)
    {
        if (plan.buffers[i].role != BufferRole::ExternalIn) { continue; }
        for (usize s = 0; s < n_seeds; ++s)
        {
            if (seeds[s].value == plan.buffers[i].value)
            {
                for (u32 e = 0; e < seeds[s].count; ++e) { ptr[i][e] = seeds[s].floats[e]; }
            }
        }
    }
    const ExecuteError ee = execute_tensor_pipeline_host(ctx, plan, ConstSpan<f32*>(ptr, nb), alloc);
    if (out != nullptr)
    {
        i32 oi = -1;
        for (usize i = 0; i < nb; ++i)
        {
            if (plan.buffers[i].role == BufferRole::Output) { oi = static_cast<i32>(i); }
        }
        REQUIRE(oi >= 0);
        for (usize e = 0; e < out_len; ++e) { out[e] = ptr[static_cast<usize>(oi)][e]; }
    }
    return ee;
}
// a SEPARATE transform module carrying ONE transform.place_mesh directive (mesh symbol + classes/fallback strings) — the 30c
// authored-placement asset, built (30c-1 gates the loader on a built module; 30c-2 commits the .ceir via bootstrap-via-print).
Module* mk_place_module(Context& ctx, const char* mesh, const char* classes, const char* fallback)
{
    Module* const xf = ctx.create_module();
    Block* const  xb = ctx.create_block(0U);
    xf->body()->append(xb);
    xb->append(transform::build_place_mesh(ctx, ctx.attr_symbol(StringView(mesh)), ctx.attr_string(StringView(classes)),
                                           ctx.attr_string(StringView(fallback))));
    return xf;
}
} // namespace

// CEIR-30b-3a (sec-103/sec-140) — LOWER a sec-140 sharded reduction into an explicit per-rank plan + a placement PRODUCER
// (`rank_lineage`), then PROVE it device-free: (split) a mesh-2 declare->shard(axis0)->reduce(axis0,sum) lowers to 2 tagged reduces
// + a combine; plan_tensor_pipeline_partitioned yields [Reduce,Reduce,Elementwise] with providers [0,1,-1]; stage_class_from_partition
// {Host,Gpu} + plan_transfers yields exactly {shard1 H->D @1, partial1 D->H @2}; and the all-Host run is BIT-EXACT vs a two-stage
// f32 oracle that DIFFERS from a flat 8-row fold (non-associativity proven LIVE). (identity) a mesh-1 shard lowers to EXACTLY the
// unsharded reduce — byte-identical print vs a separately-authored plain reduce, ranks==1, untagged. (refuse) a Sharded value into a
// matmul is a Conflict -> had_conflict + the module is byte-identical (the conflict scan runs before ANY mutation). ⛔ ASCII names.
TEST_CASE("ceir 30b-3a: lower_sharded_reduction splits a sec-140 reduction into a placed per-rank plan", "[ceir][dist][sharding]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const DistKit                 k(ctx);
    Module* const                 m   = ctx.create_module();
    Block* const                  b   = mkmain(ctx, *m);
    const TypeId                  t84 = tf(ctx, sh2(ctx, 8U, 4U)); // [8,4]
    const TypeId                  t4  = tf(ctx, sh1(ctx, 4U));     // [4]

    SECTION("the split: mesh-2 reduction -> 2 tagged reduces + a combine; placed plan + transfers + all-Host bit-exact")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x  = mkval(ctx, k, b, t84);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t84);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t4);
        b->append(rd);
        (void)mk_export(ctx, b, rd->result(0U));

        const MaterializeResult mres = materialize_sharding(ctx, *m, &root); // inserts the all_reduce completing the escaping Partial
        REQUIRE(mres.inserted == 1U);
        REQUIRE_FALSE(mres.had_conflict);

        HashMap<const Operation*, i32> lineage(&root);
        const LowerResult              lr = lower_sharded_reduction(ctx, *m, &root, lineage);
        CHECK(lr.ranks == 2U);
        CHECK_FALSE(lr.had_conflict);
        // exactly 2 reduces remain, each tagged with a distinct rank {0,1}; the original T declare is gone (split into 2 shards).
        Array<Operation*> reduces(&root);
        collect_named(ctx, m->body(), StringView("tensor.reduce"), reduces);
        REQUIRE(reduces.size() == 2U);
        const i32* const g0 = lineage.find(reduces[0]);
        const i32* const g1 = lineage.find(reduces[1]);
        REQUIRE(g0 != nullptr);
        REQUIRE(g1 != nullptr);
        CHECK(*g0 == 0); // creation order is deterministic (rank 0's reduce built first) — the tags are EXACTLY 0 then 1
        CHECK(*g1 == 1);
        // NO dist ops survive the lowering.
        CHECK(count_named(ctx, m->body(), StringView("dist.shard")) == 0U);
        CHECK(count_named(ctx, m->body(), StringView("dist.all_reduce")) == 0U);
        CHECK(count_named(ctx, m->body(), StringView("dist.mesh")) == 0U);
        // the 2 shard seeds, in creation order: decls[0] = rank 0 (rows [0,4)), decls[1] = rank 1 (rows [4,8)). Original T erased.
        Array<Operation*> decls(&root);
        collect_named(ctx, m->body(), StringView("resource.declare"), decls);
        REQUIRE(decls.size() == 2U);

        // the PLACEMENT: rank r -> assignments[r].provider = r; the partitioned planner reads rank_lineage -> PlanStage.provider.
        MlPartition partition(&root);
        partition.assignments.push_back(MlAssignment{nullptr, 0, -1});
        partition.assignments.push_back(MlAssignment{nullptr, 1, -1});
        const TensorPipelinePlan plan = plan_tensor_pipeline_partitioned(ctx, *m, &root, partition, lineage);
        REQUIRE(plan.reject == PlanReject::None);
        REQUIRE(plan.stages.size() == 3U);
        CHECK(plan.stages[0].kind == StageKind::Reduce);
        CHECK(plan.stages[1].kind == StageKind::Reduce);
        CHECK(plan.stages[2].kind == StageKind::Elementwise);
        CHECK(plan.stages[0].provider == 0);
        CHECK(plan.stages[1].provider == 1);
        CHECK(plan.stages[2].provider == -1); // the combine is unplaced (the caller's fallback class runs it)

        // stage_class_from_partition {Host, Gpu} (fallback Host) -> [Host, Gpu, Host].
        MlProvider provs[2] = {};
        provs[0].name           = StringView("host");
        provs[0].provider_class = ProviderClass::Host;
        provs[1].name           = StringView("gpu");
        provs[1].provider_class = ProviderClass::Gpu;
        const Array<ProviderClass> sc =
            stage_class_from_partition(plan, ConstSpan<MlProvider>(provs, 2U), ProviderClass::Host, &root);
        REQUIRE(sc.size() == 3U);
        CHECK(sc[0] == ProviderClass::Host);
        CHECK(sc[1] == ProviderClass::Gpu);
        CHECK(sc[2] == ProviderClass::Host);

        // plan_transfers derives EXACTLY the two inter-class crossings by identity: shard1 uploaded before the Gpu reduce (stage 1);
        // partial1 read back before the Host combine (stage 2). NO final Output readback (the combine writes it on Host).
        const TransferPlan tp = plan_transfers(plan, ConstSpan<ProviderClass>(sc.data(), sc.size()), &root);
        REQUIRE(tp.transfers.size() == 2U);
        // IDENTITY, not category: the ONE upload names EXACTLY rank-1's shard buffer; the ONE readback names EXACTLY rank-1's
        // reduce output (stage 1's trailing write). Anything else would be a placement bug a count could not catch.
        i32 shard1_bi = -1;
        for (usize i = 0; i < plan.buffers.size(); ++i)
        {
            if (plan.buffers[i].value == decls[1]->result(0U)) { shard1_bi = static_cast<i32>(i); }
        }
        REQUIRE(shard1_bi >= 0);
        REQUIRE(plan.stages[1].nbind >= 1U);
        const i32 rd1_out_bi = plan.stages[1].bind[plan.stages[1].nbind - 1U]; // rd1's trailing output = rank-1's partial
        i32       up_buf     = -1;
        i32       rb_buf     = -1;
        for (usize i = 0; i < tp.transfers.size(); ++i)
        {
            if (tp.transfers[i].direction == TransferDir::HostToDevice && tp.transfers[i].before_stage == 1U)
            {
                up_buf = static_cast<i32>(tp.transfers[i].buffer);
            }
            if (tp.transfers[i].direction == TransferDir::DeviceToHost && tp.transfers[i].before_stage == 2U)
            {
                rb_buf = static_cast<i32>(tp.transfers[i].buffer);
            }
        }
        CHECK(up_buf == shard1_bi);  // the upload is rank-1's shard, before the Gpu reduce (stage 1)
        CHECK(rb_buf == rd1_out_bi); // the readback is rank-1's partial, before the Host combine (stage 2)

        // all-Host run == the TWO-STAGE f32 oracle, which DIFFERS from a flat 8-row fold (non-associativity, proven live).
        f32 t[32] = {};
        for (u32 idx = 0; idx < 32U; ++idx) { t[idx] = 0.1F * static_cast<f32>(static_cast<i32>(idx) - 12); }
        f32 shard0[16] = {};
        f32 shard1[16] = {};
        for (u32 li = 0; li < 4U; ++li)
        {
            for (u32 c = 0; c < 4U; ++c)
            {
                shard0[li * 4U + c] = t[(0U * 4U + li) * 4U + c]; // rows [0,4)
                shard1[li * 4U + c] = t[(1U * 4U + li) * 4U + c]; // rows [4,8)
            }
        }
        const Seed seeds[2] = {{decls[0]->result(0U), shard0, 16U}, {decls[1]->result(0U), shard1, 16U}};

        f32                result[4] = {};
        const ExecuteError ee        = run_all_host(ctx, &root, plan, seeds, 2U, result, 4U);
        REQUIRE(ee == ExecuteError::None);

        bool any_diff = false;
        for (u32 c = 0; c < 4U; ++c)
        {
            const f32 p0        = fold_sum(&t[c], 4U, 4U);      // rank 0: rows [0,4), column c
            const f32 p1        = fold_sum(&t[16U + c], 4U, 4U); // rank 1: rows [4,8)
            const f32 two_stage = p0 + p1;
            const f32 flat      = fold_sum(&t[c], 8U, 4U);       // one sequential fold over all 8 rows
            CHECK(result[c] == two_stage);                       // BIT-EXACT vs the placed two-stage reduction
            if (two_stage != flat) { any_diff = true; }
        }
        CHECK(any_diff); // the split changes the summation grouping — f32 non-associativity is observable, not assumed
    }
    SECTION("the INNERMOST-axis chain: shard [4,8] axis1 -> two [4,4] -> reduce axis1 -> [4] (the geometry a future reduce-on-CUDA slice needs)")
    {
        // ⛔ axis1 (INNERMOST) makes each per-rank reduce a CONTIGUOUS per-row fold — the ONLY shape emit_reduce_cuda could run
        // (trailing-axis only). It EXISTS (ckir_cuda.hpp:712) but its 2-scalar push is incompatible with CudaComputeContext's
        // single-blob dispatch — a cross-launch-site CONTRACT change ([[feedback_cuda_emitter_signature_is_a_contract...]]), its OWN
        // slice. So 30b-3b runs the reduces on HOST (the all-reduce COMBINE on CUDA); this SECTION proves the lowering handles the
        // innermost geometry device-free, ready for that future slice.
        const TypeId t48 = tf(ctx, sh2(ctx, 4U, 8U)); // [4,8], sharded along axis1 into two [4,4]
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     x  = mkval(ctx, k, b, t48);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(1), ctx.attr_int(0), t48); // shard axis 1
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(1), str(ctx, "sum"), t4); // reduce axis 1
        b->append(rd);
        (void)mk_export(ctx, b, rd->result(0U));

        const MaterializeResult mres = materialize_sharding(ctx, *m, &root);
        REQUIRE(mres.inserted == 1U);
        HashMap<const Operation*, i32> lineage(&root);
        const LowerResult              lr = lower_sharded_reduction(ctx, *m, &root, lineage);
        CHECK(lr.ranks == 2U);
        CHECK_FALSE(lr.had_conflict);

        MlPartition partition(&root);
        partition.assignments.push_back(MlAssignment{nullptr, 0, -1});
        partition.assignments.push_back(MlAssignment{nullptr, 1, -1});
        const TensorPipelinePlan plan = plan_tensor_pipeline_partitioned(ctx, *m, &root, partition, lineage);
        REQUIRE(plan.reject == PlanReject::None);
        REQUIRE(plan.stages.size() == 3U);
        CHECK(plan.stages[0].kind == StageKind::Reduce);
        CHECK(plan.stages[1].kind == StageKind::Reduce);
        CHECK(plan.stages[2].kind == StageKind::Elementwise);

        Array<Operation*> decls(&root);
        collect_named(ctx, m->body(), StringView("resource.declare"), decls);
        REQUIRE(decls.size() == 2U);
        // T[4,8] row-major; shard0 = cols [0,4), shard1 = cols [4,8), each [4,4] row-major.
        f32 t[32] = {};
        for (u32 idx = 0; idx < 32U; ++idx) { t[idx] = 0.1F * static_cast<f32>(static_cast<i32>(idx) - 12); }
        f32 shard0[16] = {};
        f32 shard1[16] = {};
        for (u32 r = 0; r < 4U; ++r)
        {
            for (u32 c = 0; c < 4U; ++c)
            {
                shard0[r * 4U + c] = t[r * 8U + c];      // cols [0,4)
                shard1[r * 4U + c] = t[r * 8U + 4U + c]; // cols [4,8)
            }
        }
        const Seed seeds[2] = {{decls[0]->result(0U), shard0, 16U}, {decls[1]->result(0U), shard1, 16U}};

        f32                result[4] = {};
        const ExecuteError ee        = run_all_host(ctx, &root, plan, seeds, 2U, result, 4U);
        REQUIRE(ee == ExecuteError::None);

        bool any_diff = false;
        for (u32 r = 0; r < 4U; ++r)
        {
            const f32 p0        = fold_sum(&t[r * 8U], 4U, 1U);       // rank 0: row r, cols [0,4) (CONTIGUOUS — the CUDA fold shape)
            const f32 p1        = fold_sum(&t[r * 8U + 4U], 4U, 1U);  // rank 1: row r, cols [4,8)
            const f32 two_stage = p0 + p1;
            const f32 flat      = fold_sum(&t[r * 8U], 8U, 1U);       // one sequential fold over all 8 cols
            CHECK(result[r] == two_stage);
            if (two_stage != flat) { any_diff = true; }
        }
        CHECK(any_diff);
    }
    SECTION("1-device identity: a mesh-1 shard lowers to EXACTLY the unsharded reduce (byte-identical)")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m1"), str(ctx, "1")));
        Value* const     x  = mkval(ctx, k, b, t84);
        Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m1"), ctx.attr_int(0), ctx.attr_int(0), t84);
        b->append(sh);
        Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t4);
        b->append(rd);
        (void)mk_export(ctx, b, rd->result(0U));

        const MaterializeResult mres = materialize_sharding(ctx, *m, &root); // mesh-1 -> Replicated -> nothing to insert
        REQUIRE(mres.inserted == 0U);

        HashMap<const Operation*, i32> lineage(&root);
        const LowerResult              lr = lower_sharded_reduction(ctx, *m, &root, lineage);
        CHECK(lr.ranks == 1U);
        CHECK_FALSE(lr.had_conflict);
        Array<Operation*> reduces(&root);
        collect_named(ctx, m->body(), StringView("tensor.reduce"), reduces);
        REQUIRE(reduces.size() == 1U);
        CHECK(lineage.find(reduces[0]) == nullptr); // untagged — a 1-device mesh has no rank to place
        CHECK(count_named(ctx, m->body(), StringView("dist.shard")) == 0U);
        CHECK(count_named(ctx, m->body(), StringView("dist.mesh")) == 0U);

        // the lowered plan is exactly [Reduce] (unplaced).
        const MlPartition        empty(&root);
        const TensorPipelinePlan plan = plan_tensor_pipeline_partitioned(ctx, *m, &root, empty, lineage);
        REQUIRE(plan.reject == PlanReject::None);
        REQUIRE(plan.stages.size() == 1U);
        CHECK(plan.stages[0].kind == StageKind::Reduce);
        CHECK(plan.stages[0].provider == -1);

        // byte-identical print vs a SEPARATELY-authored plain declare -> reduce -> export (no dist ops).
        const containers::String lowered = print(ctx, *m, &root);
        Module* const            ref     = ctx.create_module();
        Block* const             rb      = mkmain(ctx, *ref);
        Value* const             rt      = mkval(ctx, k, rb, t84);
        Operation* const         rrd     = tensor::build_reduce(ctx, rt, ctx.attr_int(0), str(ctx, "sum"), t4);
        rb->append(rrd);
        (void)mk_export(ctx, rb, rrd->result(0U));
        const containers::String plain = print(ctx, *ref, &root);
        CHECK(StringView(lowered.c_str(), lowered.size()) == StringView(plain.c_str(), plain.size()));
    }
    SECTION("refuse: a Sharded value into a matmul -> Conflict -> had_conflict, module byte-identical")
    {
        b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
        Value* const     a   = mkval(ctx, k, b, t84); // [8,4]
        Operation* const sha = dist::build_shard(ctx, a, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t84);
        b->append(sha);
        Value* const     bb  = mkval(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));                             // [4,4]
        Operation* const mm  = tensor::build_matmul(ctx, sha->result(0U), bb, tf(ctx, sh2(ctx, 8U, 4U))); // [8,4] = [8,4]@[4,4]
        b->append(mm);
        (void)mk_export(ctx, b, mm->result(0U));

        const containers::String before = print(ctx, *m, &root);
        HashMap<const Operation*, i32> lineage(&root);
        const LowerResult              lr    = lower_sharded_reduction(ctx, *m, &root, lineage);
        const containers::String       after = print(ctx, *m, &root);
        CHECK(lr.had_conflict);
        CHECK(lr.ranks == 0U);
        CHECK(StringView(after.c_str(), after.size()) == StringView(before.c_str(), before.size())); // refuse -> no mutation
    }
}

// CEIR-30c-1 (sec-103/sec-146) — AUTHORED PLACEMENT: a `transform.place_mesh` directive resolves (placement_from_transform) to the
// per-rank device-class + fallback that feed `stage_class_from_placement` — so the CEIR-30b-3 hand-written [Host,Host,Gpu] placement
// becomes an AUTHORED asset. Device-free (30c-1 gates the loader + stage_class on a BUILT transform module; 30c-2 commits the .ceir).
// Proves: (a) resolve → rank_classes [Host,Host] + fallback Gpu; (b) the 3 typed rejects (unknown mesh / unknown class / rank-count
// mismatch) each fire; (c) a duplicate directive → find_transform_misuse DuplicateDirective; (d) ⭐ stage_class_from_placement ==
// stage_class_from_partition on the SAME plan (the "third producer, one consumer" identity). ⛔ ASCII names.
TEST_CASE("ceir 30c-1: transform.place_mesh authored placement feeds stage_class_from_placement", "[ceir][dist][sharding][transform]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const DistKit                 k(ctx);
    Module* const                 m   = ctx.create_module(); // the PAYLOAD (carries the mesh the placement references)
    Block* const                  b   = mkmain(ctx, *m);
    const TypeId                  t84 = tf(ctx, sh2(ctx, 8U, 4U));
    const TypeId                  t4  = tf(ctx, sh1(ctx, 4U));

    // the sec-140 payload: mesh "2" + declare[8,4] -> shard(axis0) -> reduce(axis0,sum) -> export.
    b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
    Value* const     x  = mkval(ctx, k, b, t84);
    Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t84);
    b->append(sh);
    Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t4);
    b->append(rd);
    (void)mk_export(ctx, b, rd->result(0U));
    REQUIRE(materialize_sharding(ctx, *m, &root).inserted == 1U);

    // ── resolve the placement BEFORE lowering (the mesh must still exist for the rank-count validation) ──
    Module* const xf = mk_place_module(ctx, "m2", "host,host", "gpu");
    CHECK(transform::find_transform_misuse(ctx, *xf).kind == transform::TransformMisuseKind::None);
    const Placement pl = placement_from_transform(ctx, *xf, *m, &root);
    REQUIRE(pl.kind == PlacementKind::Resolved);
    REQUIRE(pl.rank_classes.size() == 2U);
    CHECK(pl.rank_classes[0] == ProviderClass::Host);
    CHECK(pl.rank_classes[1] == ProviderClass::Host);
    CHECK(pl.fallback == ProviderClass::Gpu);

    // ── the typed rejects (resolved against the SAME payload — mesh "2", extent 2). ⭐ each reject is asserted by IDENTITY:
    //    the KIND *and* op != nullptr — the loader's contract is "op points at the offending directive so the caller can
    //    SURFACE the reject at the right site", so a category-only check would false-green a reject that dropped its op. ──
    const Placement r_mesh = placement_from_transform(ctx, *mk_place_module(ctx, "m9", "host,host", "gpu"), *m, &root);
    CHECK(r_mesh.kind == PlacementKind::UnknownMesh);
    CHECK(r_mesh.op != nullptr);
    const Placement r_cls = placement_from_transform(ctx, *mk_place_module(ctx, "m2", "host,bogus", "gpu"), *m, &root);
    CHECK(r_cls.kind == PlacementKind::UnknownProviderClass); // a bad `classes` segment
    CHECK(r_cls.op != nullptr);
    const Placement r_fb = placement_from_transform(ctx, *mk_place_module(ctx, "m2", "host,host", "bogus"), *m, &root);
    CHECK(r_fb.kind == PlacementKind::UnknownProviderClass); // a bad `fallback` — the SAME kind covers both attrs
    CHECK(r_fb.op != nullptr);
    const Placement r_cnt = placement_from_transform(ctx, *mk_place_module(ctx, "m2", "host,host,host", "gpu"), *m, &root);
    CHECK(r_cnt.kind == PlacementKind::RankCountMismatch);
    CHECK(r_cnt.op != nullptr);

    // ── ⛔ a 2-D mesh (shape "2,2") is verify-CLEAN but axis-0 validation would silently place 2 classes onto 4 ranks —
    //    the loader MUST refuse a >1-axis mesh with a typed reject (a `mesh_axis` attr is the sec-71 named-forward). ──
    {
        Module* const mm = ctx.create_module();
        Block* const  mb = ctx.create_block(0U);
        mm->body()->append(mb);
        mb->append(dist::build_mesh(ctx, sym(ctx, "m22"), str(ctx, "2,2")));
        const Placement r_axis = placement_from_transform(ctx, *mk_place_module(ctx, "m22", "host,host", "gpu"), *mm, &root);
        CHECK(r_axis.kind == PlacementKind::MultiAxisMesh);
        CHECK(r_axis.op != nullptr);
    }

    // ── the DUPLICATE guard: two transform.place_mesh -> find_transform_misuse DuplicateDirective ──
    {
        Module* const dup = ctx.create_module();
        Block* const  db  = ctx.create_block(0U);
        dup->body()->append(db);
        db->append(transform::build_place_mesh(ctx, sym(ctx, "m2"), str(ctx, "host,host"), str(ctx, "gpu")));
        db->append(transform::build_place_mesh(ctx, sym(ctx, "m2"), str(ctx, "host,host"), str(ctx, "gpu")));
        CHECK(transform::find_transform_misuse(ctx, *dup).kind == transform::TransformMisuseKind::DuplicateDirective);
    }

    // ── now LOWER + plan; the AUTHORED placement -> the SAME [Host,Host,Gpu] as the hand-written partition producer ──
    HashMap<const Operation*, i32> lineage(&root);
    REQUIRE(lower_sharded_reduction(ctx, *m, &root, lineage).ranks == 2U);
    MlPartition partition(&root);
    partition.assignments.push_back(MlAssignment{nullptr, 0, -1});
    partition.assignments.push_back(MlAssignment{nullptr, 1, -1});
    const TensorPipelinePlan plan = plan_tensor_pipeline_partitioned(ctx, *m, &root, partition, lineage);
    REQUIRE(plan.reject == PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);

    const Array<ProviderClass> sc_placement = stage_class_from_placement(
        plan, ConstSpan<ProviderClass>(pl.rank_classes.data(), pl.rank_classes.size()), pl.fallback, &root);
    REQUIRE(sc_placement.size() == 3U);
    CHECK(sc_placement[0] == ProviderClass::Host);
    CHECK(sc_placement[1] == ProviderClass::Host);
    CHECK(sc_placement[2] == ProviderClass::Gpu);

    // ⭐ IDENTITY: the authored placement produces the SAME per-stage vector as the hand-written partition producer — a THIRD
    //    producer agreeing with stage_class_from_partition on the same plan (the "two producers, one consumer" promise, now three).
    MlProvider provs[2] = {};
    provs[0].provider_class = ProviderClass::Host;
    provs[1].provider_class = ProviderClass::Host;
    const Array<ProviderClass> sc_partition =
        stage_class_from_partition(plan, ConstSpan<MlProvider>(provs, 2U), ProviderClass::Gpu, &root);
    REQUIRE(sc_partition.size() == sc_placement.size());
    for (usize i = 0; i < sc_placement.size(); ++i) { CHECK(sc_placement[i] == sc_partition[i]); }
}

// CEIR-30c-2 (sec-146 "placement is an asset") — the COMMITTED placement .ceir asset: assets/ceir/place_mesh_host_host_gpu.ceir
// parse-loads, is CANONICAL (anti-drift through the PRINTER vs the mk_place_module oracle — NEVER file-bytes; the CEIR-28
// committed-asset scar), roundtrip-stable, walks clean, and placement_from_transform RESOLVES it against the sec-140 payload to
// {Resolved,[Host,Host],Gpu} — the LOADED placement drives stage_class_from_placement -> [Host,Host,Gpu] (the CUDA arm proves the
// SAME loaded vector runs execute_two_class bit-exact). ⭐ the payload is walked by find_dist_misuse BEFORE the loader (the intended
// dist-verify-first flow — this is what makes the partition_ml "extent<=0 unreachable past find_dist_misuse" fold HONEST). Device-free.
// ⛔ KEEP mk_place_module as the anti-drift ORACLE; if it changes, REGENERATE the .ceir (the 22c-3c committed-asset rule).
TEST_CASE("ceir 30c-2: the committed place_mesh.ceir asset is canonical and drives stage_class_from_placement",
          "[ceir][dist][sharding][transform]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const DistKit                 k(ctx);

    // the sec-140 payload (carries the mesh "m2" the placement references) — the SAME chain the 30c-1 gate + the CUDA 30b-3b arm build.
    Module* const    m   = ctx.create_module();
    Block* const     b   = mkmain(ctx, *m);
    const TypeId     t84 = tf(ctx, sh2(ctx, 8U, 4U));
    const TypeId     t4  = tf(ctx, sh1(ctx, 4U));
    b->append(dist::build_mesh(ctx, sym(ctx, "m2"), str(ctx, "2")));
    Value* const     x  = mkval(ctx, k, b, t84);
    Operation* const sh = dist::build_shard(ctx, x, sym(ctx, "m2"), ctx.attr_int(0), ctx.attr_int(0), t84);
    b->append(sh);
    Operation* const rd = tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), str(ctx, "sum"), t4);
    b->append(rd);
    (void)mk_export(ctx, b, rd->result(0U));
    REQUIRE(materialize_sharding(ctx, *m, &root).inserted == 1U);

    // ⭐ dist-verify-FIRST: the payload walks clean BEFORE the placement loader (the intended flow; makes the loader's
    //    "extent<=0 unreachable past find_dist_misuse" fold HONEST — a malformed mesh would be caught HERE, never mislabeled).
    REQUIRE(dist::find_dist_misuse(ctx, *m).kind == dist::DistMisuseKind::None);

    // the in-memory ORACLE + its canonical print (the anti-drift SOURCE — keep mk_place_module, do NOT delete it).
    Module* const            built   = mk_place_module(ctx, "m2", "host,host", "gpu");
    const containers::String t_built = print(ctx, *built, &root);

    // parse-load the COMMITTED asset.
    const containers::Array<char> src = test_support::slurp_asset(CRD_REPO_DIR "/assets/ceir/place_mesh_host_host_gpu.ceir", ctx);
    const ParseResult             pr  = parse(ctx, StringView(src.data(), src.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ⭐ ANTI-DRIFT: the committed file's canonical print == the oracle's (regenerate the file if mk_place_module changes).
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ROUNDTRIP stability: print(parse(file)) re-parses to the identical canonical text (the committed file IS canonical).
    Context       ctx2(&root);
    const DistKit k2(ctx2);
    (void)k2;
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

    // well-formed (the place_mesh directive appears at most once) + it RESOLVES against the payload to [Host,Host]+Gpu.
    CHECK(transform::find_transform_misuse(ctx, *pr.module).kind == transform::TransformMisuseKind::None);
    const Placement pl = placement_from_transform(ctx, *pr.module, *m, &root);
    REQUIRE(pl.kind == PlacementKind::Resolved);
    REQUIRE(pl.rank_classes.size() == 2U);
    CHECK(pl.rank_classes[0] == ProviderClass::Host);
    CHECK(pl.rank_classes[1] == ProviderClass::Host);
    CHECK(pl.fallback == ProviderClass::Gpu);

    // the LOADED placement drives stage_class_from_placement -> [Host,Host,Gpu] on the lowered plan (the device-free end-to-end half).
    HashMap<const Operation*, i32> lineage(&root);
    REQUIRE(lower_sharded_reduction(ctx, *m, &root, lineage).ranks == 2U);
    MlPartition partition(&root);
    partition.assignments.push_back(MlAssignment{nullptr, 0, -1});
    partition.assignments.push_back(MlAssignment{nullptr, 1, -1});
    const TensorPipelinePlan plan = plan_tensor_pipeline_partitioned(ctx, *m, &root, partition, lineage);
    REQUIRE(plan.reject == PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);
    const Array<ProviderClass> sc = stage_class_from_placement(
        plan, ConstSpan<ProviderClass>(pl.rank_classes.data(), pl.rank_classes.size()), pl.fallback, &root);
    REQUIRE(sc.size() == 3U);
    CHECK(sc[0] == ProviderClass::Host);
    CHECK(sc[1] == ProviderClass::Host);
    CHECK(sc[2] == ProviderClass::Gpu);
}
