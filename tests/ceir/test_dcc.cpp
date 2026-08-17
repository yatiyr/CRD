// CEIR-9c (UNIVERSALITY VALIDATION, U3 DCC modifier graph, U-§84/U-§37). ⛔ A PROOF, not a feature: a Blender-class
// modifier stack runs on the CEIR foundation with ZERO new machinery — the 8h `IncrementalDag::affected_by` (transitive
// dependents) IS the depsgraph, 8d stable ids ARE modifier identity, 8i transactions ARE the parameter-edit path. ⭐ The
// DRIVER is the differentiator from 9a: where 9a PUSHED forward from a seed (Salsa-style), 9c PULLS — `affected_by(M)`
// computes the full invalidation set UPFRONT (Blender's TAG pass), then ONE topo walk EVALUATES only tagged nodes,
// early-outing any whose inputs all turned out unchanged (Blender's "no update needed"). Two consumer strategies, ONE
// engine, no code motion. The topology is a DAG with a JOIN over two SEPARATE-source branches (base->subdivide and
// cutter->transform meet at boolean), so a parameter edit on one branch must NOT re-evaluate the other (precise upstream
// isolation). Modifiers are inline-registered mock ops (zero central edits); a "mesh" is a mock poly count. Exact
// recompute counts. Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp> // umbrella: context/ir/dialect/transaction/diagnostic
#include <crd/containers/incremental_dag.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include "incremental_helpers.hpp" // fnv_mix / content_hash / value_hash / set_eq (CEIR-9x shared)

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::test; // NOLINT(google-build-using-namespace) — the shared incremental-proof helpers
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::IncrementalDag;
using crd::containers::StringView;
using crd::memory::IAllocator;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

namespace
{
// A mock DCC modifier stack over the 8h engine. Cells indexed by (stable_id - 1). `deps[i]` = a modifier's UPSTREAM
// inputs (stable ids), derived from its operands.
struct DccGraph
{
    Context&          ctx;
    IAllocator*       alloc;
    Module*           m = nullptr;
    IncrementalDag    dag;
    OpId              base, subdiv, xform, boolean_op, deform, output;
    Array<Operation*> cell;
    Array<i64>        value;
    Array<u32>        recomputes;
    Array<Array<u64>> deps;

    DccGraph(Context& c, IAllocator* a)
        : ctx(c), alloc(a), dag(a), base(c.intern_op("dcc", "base_mesh")), subdiv(c.intern_op("dcc", "subdivide")),
          xform(c.intern_op("dcc", "transform")), boolean_op(c.intern_op("dcc", "boolean")),
          deform(c.intern_op("dcc", "deform")), output(c.intern_op("dcc", "output")), cell(a), value(a), recomputes(a),
          deps(a)
    {
        Dialect* const d = c.register_dialect("dcc"); // inline-registered mock dialect — zero central edits
        (void)d->register_op("base_mesh", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
        (void)d->register_op("subdivide", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
        (void)d->register_op("transform", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
        (void)d->register_op("boolean", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
        (void)d->register_op("deform", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
        (void)d->register_op("output", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
    }

    [[nodiscard]] Operation* mod(Block* b, OpId k, ConstSpan<Value*> inputs, StringView param, i64 pval)
    {
        Operation* const o = ctx.create_operation(k, inputs, 1U, ctx.type_i64());
        if (!param.empty()) { ctx.set_attr(o, param, ctx.attr_int(pval)); }
        b->append(o);
        return o;
    }
    [[nodiscard]] i64 eval(const Operation& op) const
    {
        if (op.kind() == base) { return ctx.attr_value(op.attr("poly")).i; }
        const i64 in0 = value[op.operand(0)->defining_op()->stable_id().value - 1U];
        if (op.kind() == subdiv) { return in0 * ctx.attr_value(op.attr("level")).i; }
        if (op.kind() == xform) { return in0 + ctx.attr_value(op.attr("extra")).i; }
        if (op.kind() == boolean_op) { return in0 + value[op.operand(1)->defining_op()->stable_id().value - 1U]; }
        return in0; // deform / output preserve the poly count (a deform's `strength` does NOT change topology)
    }
    [[nodiscard]] u32 count(const Operation* op) const { return recomputes[op->stable_id().value - 1U]; }

    // Build the stack: base(100)->subdivide(level 4); cutter(50)->transform(extra 0); boolean(sub, cut_t);
    // deform(strength 1); output. ⛔ TWO SEPARATE source meshes (base, cutter) — the branches share NO op.
    void build(Array<Operation*>& out)
    {
        m                = ctx.create_module();
        Block* const top = ctx.create_block(0U);
        m->body()->append(top);
        Operation* const a_base   = mod(top, base, {}, StringView("poly"), 100);
        Value* const     sub_in[1] = {a_base->result(0)};
        Operation* const a_sub    = mod(top, subdiv, ConstSpan<Value*>(sub_in, 1U), StringView("level"), 4);
        Operation* const a_cutter = mod(top, base, {}, StringView("poly"), 50);
        Value* const     cut_in[1] = {a_cutter->result(0)};
        Operation* const a_cut    = mod(top, xform, ConstSpan<Value*>(cut_in, 1U), StringView("extra"), 0);
        Value* const     bool_in[2] = {a_sub->result(0), a_cut->result(0)};
        Operation* const a_bool   = mod(top, boolean_op, ConstSpan<Value*>(bool_in, 2U), StringView(), 0);
        Value* const     def_in[1] = {a_bool->result(0)};
        Operation* const a_def    = mod(top, deform, ConstSpan<Value*>(def_in, 1U), StringView("strength"), 1);
        Value* const     out_in[1] = {a_def->result(0)};
        Operation* const a_out    = mod(top, output, ConstSpan<Value*>(out_in, 1U), StringView(), 0);
        ctx.assign_stable_ids(*m);
        Operation* const ops[7] = {a_base, a_sub, a_cutter, a_cut, a_bool, a_def, a_out};
        for (u32 i = 0; i < 7U; ++i)
        {
            cell.push_back(ops[i]);
            value.push_back(0);
            recomputes.push_back(0U);
            deps.push_back(Array<u64>(alloc));
            out.push_back(ops[i]);
        }
        for (u32 i = 0; i < 7U; ++i)
        {
            const u64 id = cell[i]->stable_id().value;
            dag.add_node(id);
            for (u32 k = 0; k < cell[i]->num_operands(); ++k)
            {
                const Operation* const dop = cell[i]->operand(k)->defining_op();
                if (dop == nullptr) { continue; }
                dag.add_edge(id, dop->stable_id().value);
                deps[i].push_back(dop->stable_id().value);
            }
        }
    }
    void full_eval()
    {
        Array<u64> order(alloc);
        REQUIRE(dag.topo_order(order));
        for (usize i = 0; i < order.size(); ++i)
        {
            const u64 id    = order[i];
            const i64 v     = eval(*cell[id - 1U]);
            value[id - 1U]  = v;
            recomputes[id - 1U] += 1U;
            dag.set_revision(id, content_hash(*cell[id - 1U], ctx), value_hash(v));
        }
    }
    // The Blender depsgraph re-eval. TAG pass: the invalidation set = {seed} + affected_by(seed) (the 8h engine's
    // transitive dependents, computed UPFRONT). EVAL pass: ONE topo walk; a TAGGED node re-evaluates IFF (it is the seed
    // and its FORMULA changed — content-addressed) OR (an upstream input's mesh changed); else it EARLY-OUTS. Fills
    // `tag` (the invalidation set) and `evaluated` (the nodes actually recomputed) so the proof can compare them.
    void depsgraph_reeval(u64 seed, Array<u64>& tag, Array<u64>& evaluated)
    {
        tag.clear();
        evaluated.clear();
        Array<u64> aff(alloc);
        REQUIRE(dag.affected_by(seed, aff));
        tag.push_back(seed);
        for (usize i = 0; i < aff.size(); ++i) { tag.push_back(aff[i]); }
        Array<u8> tagged(alloc);
        Array<u8> changed(alloc);
        for (u32 i = 0; i < 7U; ++i)
        {
            tagged.push_back(0U);
            changed.push_back(0U);
        }
        for (usize i = 0; i < tag.size(); ++i) { tagged[tag[i] - 1U] = 1U; }
        Array<u64> order(alloc);
        REQUIRE(dag.topo_order(order));
        for (usize i = 0; i < order.size(); ++i)
        {
            const u64 id = order[i];
            if (tagged[id - 1U] == 0U) { continue; }
            const u64 new_content = content_hash(*cell[id - 1U], ctx);
            bool      must        = false;
            if (id == seed && new_content != dag.content_of(id)) { must = true; } // the edited modifier's formula changed
            for (usize k = 0; k < deps[id - 1U].size(); ++k)
            {
                if (changed[deps[id - 1U][k] - 1U] != 0U) { must = true; } // an upstream input's mesh changed
            }
            if (!must) { continue; } // early-out (Blender "no update needed")
            const u64 old_interface = dag.interface_of(id);
            const i64 v             = eval(*cell[id - 1U]);
            value[id - 1U]          = v;
            recomputes[id - 1U] += 1U;
            const u64 new_interface = value_hash(v);
            dag.set_revision(id, new_content, new_interface);
            evaluated.push_back(id);
            if (new_interface != old_interface) { changed[id - 1U] = 1U; }
        }
    }
    // A parameter edit rides an 8i transaction (the touched-set is the depsgraph seed).
    [[nodiscard]] u64 edit_param(Operation* modop, StringView param, i64 newval)
    {
        DiagnosticEngine diag(ctx, alloc);
        Transaction      tx(ctx, *m, diag, alloc);
        REQUIRE(tx.set_attr(modop, param, ctx.attr_int(newval)));
        REQUIRE(tx.commit());
        REQUIRE(tx.touched().size() == 1U);
        return tx.touched()[0].value;
    }
};
} // namespace

TEST_CASE("ceir 9c: initial evaluation computes every modifier exactly once", "[ceir][dcc]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    DccGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    for (u32 i = 0; i < 7U; ++i) { CHECK(g.recomputes[i] == 1U); }
    CHECK(g.value[1] == 400); // subdivide = base(100) * level(4)
    CHECK(g.value[4] == 450); // boolean = sub(400) + cut_t(50)
    CHECK(g.value[6] == 450); // output = deform(preserves) = 450
}

TEST_CASE("ceir 9c: a subdivide edit re-evaluates exactly its downstream (tag set equals eval set)", "[ceir][dcc]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    DccGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    Operation* const sub = c[1];

    // affected_by(subdivide) is the depsgraph TAG set — asserted EXACT (both directions).
    Array<u64> aff(&root);
    REQUIRE(g.dag.affected_by(sub->stable_id().value, aff));
    const u64 down[3] = {c[4]->stable_id().value, c[5]->stable_id().value, c[6]->stable_id().value}; // bool, deform, output
    CHECK(set_eq(aff, down, 3U));

    const u64  seed = g.edit_param(sub, StringView("level"), 8); // 4 -> 8, a real value change
    Array<u64> tag(&root);
    Array<u64> evaluated(&root);
    g.depsgraph_reeval(seed, tag, evaluated);

    // every downstream modifier's mesh genuinely changed -> the eval set EQUALS the tag set.
    CHECK(evaluated.size() == tag.size());
    CHECK(g.recomputes[1] == 2U); // subdivide
    CHECK(g.recomputes[4] == 2U); // boolean
    CHECK(g.recomputes[5] == 2U); // deform
    CHECK(g.recomputes[6] == 2U); // output
    CHECK(g.recomputes[0] == 1U); // base       cache-hit
    CHECK(g.recomputes[2] == 1U); // cutter     cache-hit
    CHECK(g.recomputes[3] == 1U); // transform  cache-hit
    CHECK(g.value[6] == 850);     // output = (800 + 50)
}

TEST_CASE("ceir 9c: a cutter-branch edit does not re-evaluate the subdivide branch (precise upstream isolation)", "[ceir][dcc]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    DccGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    Operation* const cut = c[3]; // transform (the cutter branch)
    Operation* const sub = c[1]; // subdivide (the OTHER branch)

    // subdivide is NOT in the cutter's transitive dependents (the two branches are independent until the boolean join).
    Array<u64> aff(&root);
    REQUIRE(g.dag.affected_by(cut->stable_id().value, aff));
    bool has_sub = false;
    for (usize i = 0; i < aff.size(); ++i)
    {
        if (aff[i] == sub->stable_id().value) { has_sub = true; }
    }
    CHECK_FALSE(has_sub);

    const u64  seed = g.edit_param(cut, StringView("extra"), 10); // cutter branch: cut_t 50 -> 60
    Array<u64> tag(&root);
    Array<u64> evaluated(&root);
    g.depsgraph_reeval(seed, tag, evaluated);

    CHECK(g.recomputes[3] == 2U); // transform (edited)
    CHECK(g.recomputes[4] == 2U); // boolean   (join re-evals)
    CHECK(g.recomputes[5] == 2U); // deform
    CHECK(g.recomputes[6] == 2U); // output
    CHECK(g.recomputes[1] == 1U); // ⛔ subdivide CACHE-HIT — the isolated branch
    CHECK(g.recomputes[0] == 1U); // base   cache-hit
    CHECK(g.recomputes[2] == 1U); // cutter cache-hit
    CHECK(g.value[6] == 460);     // output = (400 + 60)
}

TEST_CASE("ceir 9c: a topology-preserving deform edit early-outs its dependents (tag set exceeds eval set)", "[ceir][dcc]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    DccGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    Operation* const def = c[5];
    Operation* const out = c[6];

    // editing deform's `strength` changes its FORMULA (content) but NOT its output poly (deform preserves topology) ->
    // output is TAGGED but EARLY-OUTS (its input mesh is unchanged) -> the §107 cutoff.
    const u64  seed = g.edit_param(def, StringView("strength"), 2);
    Array<u64> tag(&root);
    Array<u64> evaluated(&root);
    g.depsgraph_reeval(seed, tag, evaluated);

    const u64 tag_ids[2] = {def->stable_id().value, out->stable_id().value};
    CHECK(set_eq(tag, tag_ids, 2U));   // the tag set is {deform, output}...
    CHECK(evaluated.size() == 1U);      // ...but only deform re-evaluated
    CHECK(evaluated[0] == def->stable_id().value);
    CHECK(g.recomputes[5] == 2U);       // deform recomputed (its formula changed)
    CHECK(g.recomputes[6] == 1U);       // ⛔ output EARLY-OUT — the named cut node (unchanged input mesh)
}

TEST_CASE("ceir 9c: a no-op parameter edit through a transaction re-evaluates nothing", "[ceir][dcc]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    DccGraph                     g(ctx, &root);
    Array<Operation*>            c(&root);
    g.build(c);
    g.full_eval();
    Operation* const sub = c[1];

    // set subdivide's level to its CURRENT value (4) -> the tx reports it touched, but the content-addressed memo
    // (the seed's formula unchanged) re-evaluates NOTHING downstream.
    const u64  seed = g.edit_param(sub, StringView("level"), 4);
    Array<u64> tag(&root);
    Array<u64> evaluated(&root);
    g.depsgraph_reeval(seed, tag, evaluated);
    CHECK(evaluated.size() == 0U);
    for (u32 i = 0; i < 7U; ++i) { CHECK(g.recomputes[i] == 1U); }
}
