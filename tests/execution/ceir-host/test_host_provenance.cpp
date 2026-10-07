// DIAG.8a — a host-provider runtime error names its authored op. The crd-jobs provider runs parallel bodies, map_reduce
// folds and pooled launch bodies on SEPARATE sub-interpreters; an error raised inside one must still be blamed on the
// body op that raised it, exactly as the sequential reference interpreter and the compiled plan blame it, and that op
// must resolve to its authored line after CSE, serialization and a load into a fresh Context. The expected positions
// come from scanning the printed text, never from the parser. Controls: the first offender is chosen in INDEX order
// (not program order, not worker timing) for every job split; an error raised by the owning op itself still names that
// op. ⛔ The jobs pool is owned by the listener in test_host_provider.cpp (same binary). ASCII test names.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/host/host_provider.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/pass_manager.hpp>
#include <crd/ceir/passes/cse.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
constexpr const char* kFile = "programs/diag/host_provenance.ceir";

void register_dialects(Context& ctx)
{
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)task::register_task_ops(ctx);
    (void)async::register_async_ops(ctx);
    (void)func::register_dialect(ctx);
}

Operation* konst(Context& ctx, Block* b, i64 v)
{
    Operation* const op = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_i32());
    ctx.set_attr(op, "value", ctx.attr_int(v));
    b->append(op);
    return op;
}

Value* bin(Context& ctx, const char* name, Block* b, Value* x, Value* y)
{
    Value*           in[2] = {x, y};
    Operation* const op =
        ctx.create_operation(ctx.intern_op("arith", name), ConstSpan<Value*>(in, 2U), 1U, ctx.type_i32());
    b->append(op);
    return op->result(0U);
}

void yield_values(Context& ctx, Block* b, ConstSpan<Value*> vs)
{
    b->append(ctx.create_operation(ctx.intern_op("core", "yield"), vs, 0U));
}

// core.for(0, 1, %step) { yield } in `b`: a one-trip loop that faults (BadForStep) exactly when %step <= 0.
void guarded_for(Context& ctx, Block* b, Value* step)
{
    Value*           range[3] = {konst(ctx, b, 0)->result(0U), konst(ctx, b, 1)->result(0U), step};
    Operation* const loop =
        ctx.create_operation(ctx.intern_op("core", "for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    Block* const lb = ctx.create_block(1U, ctx.type_i32());
    loop->region(0)->append(lb);
    yield_values(ctx, lb, {});
    b->append(loop);
}

Block* add_func(Context& ctx, Module& m, const char* name)
{
    Operation* const fn = func::create_func(ctx, m, name, Visibility::Public, 0U, ctx.type_i32());
    m.body()->first_block()->append(fn);
    return func::func_body_block(fn);
}

// Four entries, one fault each. core.for occurrences in text order: 0 = loop A, 1 = loop B (parallel body), 2 = the
// launch body's loop, 3 = the combine's loop.
//   @pfor_fault  : parallel_for(0, 8, 1) { iv: for(0,1, 4-iv) {}  (A: faults for iv >= 4)
//                                             for(0,1, iv-1) {}  (B: faults for iv <= 1)  yield iv }
//                  Index 0 is the first offender and faults at B, although A precedes B in program order.
//   @launch_fault: %t = async.launch { for(0,1,0) {}; yield 7 }; %v = async.await %t; return %v
//   @fold_fault  : map_reduce(0, 4, 1, 0) map { iv: yield iv } combine { acc, e: for(0,1,0) {}; yield acc }
//   @owner_fault : parallel_for(0, 4, 0) { iv: yield iv }   (the parallel op's own step: the owner is the offender)
Module* build(Context& ctx)
{
    Module* const m = ctx.create_module();
    m->body()->append(ctx.create_block(0U));

    {
        Block* const fb = add_func(ctx, *m, "pfor_fault");
        Value* range[3] = {konst(ctx, fb, 0)->result(0U), konst(ctx, fb, 8)->result(0U), konst(ctx, fb, 1)->result(0U)};
        Operation* const pf =
            ctx.create_operation(ctx.intern_op("task", "parallel_for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
        fb->append(pf);
        Block* const body = ctx.create_block(1U, ctx.type_i32());
        pf->region(0)->append(body);
        Value* const iv  = body->arg(0U);
        Value* const neg = bin(ctx, "muli", body, iv, konst(ctx, body, -1)->result(0U));
        guarded_for(ctx, body, bin(ctx, "addi", body, neg, konst(ctx, body, 4)->result(0U)));
        guarded_for(ctx, body, bin(ctx, "addi", body, iv, konst(ctx, body, -1)->result(0U)));
        Value* yv[1] = {iv};
        yield_values(ctx, body, ConstSpan<Value*>(yv, 1U));
        fb->append(func::create_return(ctx, {}));
    }
    {
        Block* const     fb = add_func(ctx, *m, "launch_fault");
        Operation* const l  = ctx.create_operation(ctx.intern_op("async", "launch"), {}, 1U, ctx.type_i32(), 1U);
        Block* const     lb = ctx.create_block(0U);
        l->region(0)->append(lb);
        guarded_for(ctx, lb, konst(ctx, lb, 0)->result(0U));
        Value* lv[1] = {konst(ctx, lb, 7)->result(0U)};
        yield_values(ctx, lb, ConstSpan<Value*>(lv, 1U));
        fb->append(l);
        Value*           tok[1] = {l->result(0U)};
        Operation* const aw =
            ctx.create_operation(ctx.intern_op("async", "await"), ConstSpan<Value*>(tok, 1U), 1U, ctx.type_i32());
        fb->append(aw);
        Value* rv[1] = {aw->result(0U)};
        fb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    }
    {
        Block* const fb = add_func(ctx, *m, "fold_fault");
        Value* ops4[4] = {konst(ctx, fb, 0)->result(0U), konst(ctx, fb, 4)->result(0U), konst(ctx, fb, 1)->result(0U),
                          konst(ctx, fb, 0)->result(0U)};
        Operation* const mr = ctx.create_operation(ctx.intern_op("task", "map_reduce"), ConstSpan<Value*>(ops4, 4U), 1U,
                                                   ctx.type_i32(), 2U);
        fb->append(mr);
        Block* const mapb = ctx.create_block(1U, ctx.type_i32());
        mr->region(0)->append(mapb);
        Value* mv[1] = {mapb->arg(0U)};
        yield_values(ctx, mapb, ConstSpan<Value*>(mv, 1U));
        Block* const cb = ctx.create_block(2U, ctx.type_i32());
        mr->region(1)->append(cb);
        guarded_for(ctx, cb, konst(ctx, cb, 0)->result(0U));
        Value* cv[1] = {cb->arg(0U)};
        yield_values(ctx, cb, ConstSpan<Value*>(cv, 1U));
        Value* rv[1] = {mr->result(0U)};
        fb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    }
    {
        Block* const fb = add_func(ctx, *m, "owner_fault");
        Value* range[3] = {konst(ctx, fb, 0)->result(0U), konst(ctx, fb, 4)->result(0U), konst(ctx, fb, 0)->result(0U)};
        Operation* const pf =
            ctx.create_operation(ctx.intern_op("task", "parallel_for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
        fb->append(pf);
        Block* const body = ctx.create_block(1U, ctx.type_i32());
        pf->region(0)->append(body);
        Value* yv[1] = {body->arg(0U)};
        yield_values(ctx, body, ConstSpan<Value*>(yv, 1U));
        fb->append(func::create_return(ctx, {}));
    }
    return m;
}

// The 1-based line holding the n-th occurrence of `needle`, and the column of that line's first non-blank character.
struct TextPos
{
    u32 line = 0U;
    u32 col  = 0U;
};
TextPos find_op(StringView text, const char* needle, u32 nth)
{
    const usize nlen       = StringView(needle).size();
    u32         line       = 1U;
    usize       line_start = 0U;
    u32         seen       = 0U;
    for (usize i = 0; i + nlen <= text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            line_start = i + 1U;
            continue;
        }
        if (StringView(text.data() + i, nlen) == StringView(needle) && seen++ == nth)
        {
            usize first = line_start;
            while (text[first] == ' ' || text[first] == '\t')
            {
                ++first;
            }
            return TextPos{line, static_cast<u32>(first - line_start + 1U)};
        }
    }
    return TextPos{};
}

void check_at(const Context& ctx, const Provenance& p, TextPos at)
{
    CHECK(p.gap == ProvenanceGap::None);
    const Origin* const o = p.primary();
    REQUIRE(o != nullptr);
    CHECK(o->loc.line == at.line);
    CHECK(o->loc.col == at.col);
    CHECK(ctx.file_path(o->loc.file_id) == StringView(kFile));
}

// The authored program after the production optimization and a binary round trip into `loaded`.
struct Loaded
{
    Module* module = nullptr;
    TextPos for_a, for_b, for_launch, for_combine, pfor_owner;
};
Loaded author_optimize_reload(Context& loaded, memory::IAllocator* alloc)
{
    Context builder(alloc);
    register_dialects(builder);
    const String     text = print(builder, *build(builder), alloc);
    const StringView src(text.data(), text.size());
    Loaded           out;
    out.for_a       = find_op(src, "core.for", 0U);
    out.for_b       = find_op(src, "core.for", 1U);
    out.for_launch  = find_op(src, "core.for", 2U);
    out.for_combine = find_op(src, "core.for", 3U);
    out.pfor_owner  = find_op(src, "task.parallel_for", 1U);
    REQUIRE(out.for_a.line != 0U);
    REQUIRE(out.for_b.line > out.for_a.line);
    REQUIRE(out.for_launch.line > out.for_b.line);
    REQUIRE(out.for_combine.line > out.for_launch.line);
    REQUIRE(out.pfor_owner.line > out.for_combine.line);

    Context ctx(alloc);
    register_dialects(ctx);
    const ParseResult pr = parse(ctx, src, ctx.register_file(kFile));
    REQUIRE(pr.ok);
    DiagnosticEngine diag(ctx, alloc);
    AnalysisManager  am(alloc);
    PassManager      pm(alloc);
    pm.add_pass(cse_pass());
    pm.run(ctx, *pr.module, am, diag);
    REQUIRE_FALSE(diag.has_errors());

    const Array<u8> blob = serialize(ctx, *pr.module, alloc);
    register_dialects(loaded);
    const ParseResult lr = deserialize(loaded, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(lr.ok);
    out.module = lr.module;
    return out;
}

exec::ExecResult run_reference(Context& ctx, const Module& m, const char* entry)
{
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    exec::ExecResult r = in.invoke(m, StringView(entry), {});
    return r;
}

// Compile and run `entry`; its fault must resolve to `at`. The provenance is a view into the plan's own tables, so it
// is checked here, while the plan is alive.
void check_plan_fault(Context& ctx, const Module& m, const char* entry, plan::RunError want, TextPos at,
                      memory::IAllocator* alloc)
{
    const plan::CompileResult cr = plan::compile(ctx, m, StringView(entry), alloc);
    REQUIRE(cr.ok());
    const plan::RunResult rr = plan::run(cr.plan, {}, alloc);
    CHECK(rr.error == want);
    REQUIRE(rr.fault.valid());
    check_at(ctx, plan::instr_provenance(cr.plan, rr.fault), at);
}
} // namespace

TEST_CASE("diag 8a: a parallel body error names its authored body op on every job split", "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       loaded(&root);
    const Loaded                  l = author_optimize_reload(loaded, &root);
    Array<Origin>                 storage(&root);

    // The reference and the plan run the indices in order: index 0 faults at loop B.
    const exec::ExecResult er = run_reference(loaded, *l.module, "pfor_fault");
    CHECK(er.error == exec::ExecError::BadForStep);
    check_at(loaded, resolve_provenance(loaded, er.op, storage), l.for_b);
    check_plan_fault(loaded, *l.module, "pfor_fault", plan::RunError::BadForStep, l.for_b, &root);

    // The provider runs indices 4..7 (which fault at loop A) concurrently with 0 and 1; it must still report index 0.
    const u32 splits[5] = {1U, 2U, 3U, 4U, 8U};
    for (const u32 nj : splits)
    {
        memory::GrowableTlsfAllocator palloc;
        host::HostProvider            prov(&palloc, nj);
        const exec::ExecResult        rp = prov.execute(loaded, *l.module, "pfor_fault", {});
        CHECK(rp.error == exec::ExecError::BadForStep);
        REQUIRE(rp.op != nullptr);
        CHECK(rp.op == er.op);
        check_at(loaded, resolve_provenance(loaded, rp.op, storage), l.for_b);
    }
}

TEST_CASE("diag 8a: a pooled launch body error is blamed on the body op, not the await", "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       loaded(&root);
    const Loaded                  l = author_optimize_reload(loaded, &root);
    Array<Origin>                 storage(&root);

    const exec::ExecResult er = run_reference(loaded, *l.module, "launch_fault");
    CHECK(er.error == exec::ExecError::BadForStep);
    check_at(loaded, resolve_provenance(loaded, er.op, storage), l.for_launch);
    check_plan_fault(loaded, *l.module, "launch_fault", plan::RunError::BadForStep, l.for_launch, &root);

    memory::GrowableTlsfAllocator palloc;
    host::HostProvider            prov(&palloc, 4U);
    const exec::ExecResult        rp = prov.execute(loaded, *l.module, "launch_fault", {});
    CHECK(prov.pooled_count() == 1U); // the body really ran on a worker, so its error crossed the token
    CHECK(rp.error == exec::ExecError::BadForStep);
    REQUIRE(rp.op != nullptr);
    CHECK(rp.op == er.op);
    check_at(loaded, resolve_provenance(loaded, rp.op, storage), l.for_launch);
}

TEST_CASE("diag 8a: a map_reduce fold-step error names the combine op", "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       loaded(&root);
    const Loaded                  l = author_optimize_reload(loaded, &root);
    Array<Origin>                 storage(&root);

    const exec::ExecResult er = run_reference(loaded, *l.module, "fold_fault");
    CHECK(er.error == exec::ExecError::BadForStep);
    check_at(loaded, resolve_provenance(loaded, er.op, storage), l.for_combine);
    check_plan_fault(loaded, *l.module, "fold_fault", plan::RunError::BadForStep, l.for_combine, &root);

    memory::GrowableTlsfAllocator palloc;
    host::HostProvider            prov(&palloc, 3U);
    const exec::ExecResult        rp = prov.execute(loaded, *l.module, "fold_fault", {});
    CHECK(rp.error == exec::ExecError::BadForStep);
    REQUIRE(rp.op != nullptr);
    CHECK(rp.op == er.op);
    check_at(loaded, resolve_provenance(loaded, rp.op, storage), l.for_combine);
}

TEST_CASE("diag 8a: an error raised by the parallel op itself still names that op", "[ceir][host][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       loaded(&root);
    const Loaded                  l = author_optimize_reload(loaded, &root);
    Array<Origin>                 storage(&root);

    const exec::ExecResult er = run_reference(loaded, *l.module, "owner_fault");
    CHECK(er.error == exec::ExecError::BadForStep);
    check_at(loaded, resolve_provenance(loaded, er.op, storage), l.pfor_owner);
    check_plan_fault(loaded, *l.module, "owner_fault", plan::RunError::BadForStep, l.pfor_owner, &root);

    memory::GrowableTlsfAllocator palloc;
    host::HostProvider            prov(&palloc, 4U);
    const exec::ExecResult        rp = prov.execute(loaded, *l.module, "owner_fault", {});
    CHECK(rp.error == exec::ExecError::BadForStep);
    REQUIRE(rp.op != nullptr);
    CHECK(rp.op == er.op);
    check_at(loaded, resolve_provenance(loaded, rp.op, storage), l.pfor_owner);
}
