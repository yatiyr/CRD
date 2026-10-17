// DIAG.10a — the host provider's parallel range and the levels its bodies nest at. The compiled-versus-reference
// comparison (crd-fuzz-ceir-differential) bounded both sequential tiers; these cases hold the crd-jobs provider to the
// same answers. A range spanning more than half of the i64 line has its indices, a range is charged one step of the
// run's budget per index before anything is sized from its count, a map_reduce fold runs on the run's budget, and a
// body run on a separate sub-interpreter (a parallel range on a pool worker, a fold step, a pooled launch body) starts
// at the level of the op that runs it, so it stops at exec::kMaxNesting on the op the reference blames, without running
// out of its fiber's stack. Each case compares the provider with the sequential reference on the same module.
// ⛔ The jobs pool is owned by the listener in test_host_provider.cpp (same binary). ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/host/host_provider.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;

namespace
{
struct Kit
{
    OpId cst, addi, cif, cfor, yield, pfor, mreduce, launch, awaitk;
    explicit Kit(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")), cif(ctx.intern_op("core", "if")),
          cfor(ctx.intern_op("core", "for")), yield(ctx.intern_op("core", "yield")),
          pfor(ctx.intern_op("task", "parallel_for")), mreduce(ctx.intern_op("task", "map_reduce")),
          launch(ctx.intern_op("async", "launch")), awaitk(ctx.intern_op("async", "await"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)async::register_async_ops(ctx);
        (void)task::register_task_ops(ctx);
    }
};

Value* konst(Context& ctx, const Kit& k, Block* b, i64 v)
{
    Operation* const c = ctx.create_operation(k.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c->result(0U);
}

Value* add(Context& ctx, const Kit& k, Block* b, Value* x, Value* y)
{
    Value*           in[2] = {x, y};
    Operation* const op    = ctx.create_operation(k.addi, ConstSpan<Value*>(in, 2U), 1U, ctx.type_i32());
    b->append(op);
    return op->result(0U);
}

void yield_values(Context& ctx, const Kit& k, Block* b, ConstSpan<Value*> vs)
{
    b->append(ctx.create_operation(k.yield, vs, 0U));
}

Value* call(Context& ctx, Block* b, const char* callee, Value* arg)
{
    Value*           a[1] = {arg};
    Operation* const c    = func::create_call(ctx, callee, ConstSpan<Value*>(a, 1U), 1U, ctx.type_i32());
    b->append(c);
    return c->result(0U);
}

Block* add_func(Context& ctx, Module& m, const char* name)
{
    Operation* const f  = func::create_func(ctx, m, name, Visibility::Public, 1U, ctx.type_i32());
    REQUIRE(f != nullptr);
    Block*           mb = m.body()->first_block();
    if (mb == nullptr)
    {
        mb = ctx.create_block(0U);
        m.body()->append(mb);
    }
    mb->append(f);
    return &func::func_body(*f);
}

// In `b`: if (%n) { <then> } else { <else> }. Returns the two blocks for the caller to fill; each ends in a yield.
void if_else(Context& ctx, const Kit& k, Block* b, Value* n, Block*& then_out, Block*& else_out)
{
    Value*           cond[1] = {n};
    Operation* const ifop    = ctx.create_operation(k.cif, ConstSpan<Value*>(cond, 1U), 0U, {}, 2U);
    b->append(ifop);
    then_out = ctx.create_block(0U);
    ifop->region(0)->append(then_out);
    else_out = ctx.create_block(0U);
    ifop->region(1)->append(else_out);
}

void ret(Context& ctx, Block* b, Value* v)
{
    Value* a[1] = {v};
    b->append(func::create_return(ctx, ConstSpan<Value*>(a, 1U)));
}

// @rec(n){ if (n) { call rec(n - 1) }; return n }: two nesting levels per call (the if and the call).
void add_rec(Context& ctx, const Kit& k, Module& m)
{
    Block* const b         = add_func(ctx, m, "rec");
    Value* const n         = b->arg(0U);
    Value* const n_minus_1 = add(ctx, k, b, n, konst(ctx, k, b, -1));
    Block*       thenb     = nullptr;
    Block*       elseb     = nullptr;
    if_else(ctx, k, b, n, thenb, elseb);
    (void)call(ctx, thenb, "rec", n_minus_1);
    yield_values(ctx, k, thenb, {});
    yield_values(ctx, k, elseb, {});
    ret(ctx, b, n);
}

enum class Body : u8
{
    Range,  // parallel_for(0, 4, 1) { (iv): r = call rec(depth); yield r } (each index on a pool worker)
    Fold,   // map_reduce(0, 4, 1, 0) { (iv): yield iv } { (a, x): r = call rec(depth); yield a } (the fold)
    Launch, // t = async.launch { r = call rec(depth); yield r }; await t (a pooled launch body on a pool worker)
};

// @main(d){ if (d) { call main(d - 1) } else { <body calling rec(depth)> }; return d }: `d` levels of descent put the
// op that runs the body at level 2d + 1, so the body's own ops run at 2d + 2 and its deepest if at 2d + 3 + 2*depth.
// The body stays inside kMaxNesting exactly when d + depth <= 30.
Module* build(Context& ctx, const Kit& k, Body body, i64 depth)
{
    Module* const m = ctx.create_module();
    add_rec(ctx, k, *m);
    Block* const b         = add_func(ctx, *m, "main");
    Value* const d         = b->arg(0U);
    Value* const d_minus_1 = add(ctx, k, b, d, konst(ctx, k, b, -1));
    Block*       thenb     = nullptr;
    Block*       elseb     = nullptr;
    if_else(ctx, k, b, d, thenb, elseb);
    (void)call(ctx, thenb, "main", d_minus_1);
    yield_values(ctx, k, thenb, {});
    switch (body)
    {
    case Body::Range:
    {
        Value* range[3] = {konst(ctx, k, elseb, 0), konst(ctx, k, elseb, 4), konst(ctx, k, elseb, 1)};
        Operation* const pf = ctx.create_operation(k.pfor, ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
        elseb->append(pf);
        Block* const pb = ctx.create_block(1U, ctx.type_i32());
        pf->region(0)->append(pb);
        Value* r[1] = {call(ctx, pb, "rec", konst(ctx, k, pb, depth))};
        yield_values(ctx, k, pb, ConstSpan<Value*>(r, 1U));
        break;
    }
    case Body::Fold:
    {
        Value* range[4] = {konst(ctx, k, elseb, 0), konst(ctx, k, elseb, 4), konst(ctx, k, elseb, 1),
                           konst(ctx, k, elseb, 0)};
        Operation* const mr = ctx.create_operation(k.mreduce, ConstSpan<Value*>(range, 4U), 1U, ctx.type_i32(), 2U);
        elseb->append(mr);
        Block* const mapb = ctx.create_block(1U, ctx.type_i32());
        mr->region(0)->append(mapb);
        Value* iv[1] = {mapb->arg(0U)};
        yield_values(ctx, k, mapb, ConstSpan<Value*>(iv, 1U));
        Block* const comb = ctx.create_block(2U, ctx.type_i32());
        mr->region(1)->append(comb);
        (void)call(ctx, comb, "rec", konst(ctx, k, comb, depth));
        Value* acc[1] = {comb->arg(0U)};
        yield_values(ctx, k, comb, ConstSpan<Value*>(acc, 1U));
        break;
    }
    case Body::Launch:
    {
        Operation* const l = ctx.create_operation(k.launch, {}, 1U, ctx.type_i32(), 1U);
        elseb->append(l);
        Block* const lb = ctx.create_block(0U);
        l->region(0)->append(lb);
        Value* r[1] = {call(ctx, lb, "rec", konst(ctx, k, lb, depth))};
        yield_values(ctx, k, lb, ConstSpan<Value*>(r, 1U));
        Value*           tok[1] = {l->result(0U)};
        Operation* const aw     = ctx.create_operation(k.awaitk, ConstSpan<Value*>(tok, 1U), 1U, ctx.type_i32());
        elseb->append(aw);
        break;
    }
    }
    yield_values(ctx, k, elseb, {});
    ret(ctx, b, d);
    return m;
}

struct Outcome
{
    exec::ExecError  error = exec::ExecError::None;
    const Operation* op    = nullptr;
    i64              value = 0;
};

Outcome reference(Context& ctx, const Module& m, i64 d)
{
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    const i64              args[1] = {d};
    const exec::ExecResult r       = in.invoke(m, "main", ConstSpan<i64>(args, 1U));
    return Outcome{r.error, r.op, r.values.size() == 1U ? r.values[0] : 0};
}

Outcome provider(Context& ctx, const Module& m, i64 d, crd::usize* pooled = nullptr)
{
    crd::memory::GrowableTlsfAllocator palloc;
    host::HostProvider                 prov(&palloc, 4U);
    const i64                          args[1] = {d};
    const exec::ExecResult             r       = prov.execute(ctx, m, "main", ConstSpan<i64>(args, 1U));
    if (pooled != nullptr)
    {
        *pooled = prov.pooled_count();
    }
    return Outcome{r.error, r.op, r.values.size() == 1U ? r.values[0] : 0};
}

const char* body_name(Body body)
{
    switch (body)
    {
    case Body::Range: return "parallel range";
    case Body::Fold: return "map_reduce fold";
    case Body::Launch: return "pooled launch";
    }
    return "?";
}

// Counts the memory-acquiring calls made through it (the provider's own allocator: the map outputs, the per-index
// error slots and the pooled tokens; never a worker's scratch). Submitting thread only.
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    explicit CountingAllocator(crd::memory::IAllocator* backing) : m_backing(backing) { m_name = "CountingAllocator"; }
    void* allocate(crd::usize size, crd::usize align = crd::memory::kDefaultAlignment) override
    {
        ++m_allocs;
        return m_backing->allocate(size, align);
    }
    void deallocate(void* p) noexcept override { m_backing->deallocate(p); }
    bool owns(const void* p) const noexcept override { return m_backing->owns(p); }
    void* reallocate(void* p, crd::usize os, crd::usize ns, crd::usize a = crd::memory::kDefaultAlignment) override
    {
        ++m_allocs;
        return m_backing->reallocate(p, os, ns, a);
    }
    void* try_allocate(crd::usize size, crd::usize align = crd::memory::kDefaultAlignment) override
    {
        ++m_allocs;
        return m_backing->try_allocate(size, align);
    }
    [[nodiscard]] crd::u64 allocs() const noexcept { return m_allocs; }

private:
    crd::memory::IAllocator* m_backing;
    crd::u64                 m_allocs = 0;
};

// @main(){ parallel_for(lo, hi, step) { (iv): yield iv }; return 0 }; returns the parallel_for op.
Operation* parallel_range(Context& ctx, const Kit& k, Module& m, i64 lo, i64 hi, i64 step)
{
    Operation* const f  = func::create_func(ctx, m, "main", Visibility::Public, 0U, ctx.type_i32());
    REQUIRE(f != nullptr);
    Block* const     mb = ctx.create_block(0U);
    m.body()->append(mb);
    mb->append(f);
    Block* const     b        = &func::func_body(*f);
    Value*           range[3] = {konst(ctx, k, b, lo), konst(ctx, k, b, hi), konst(ctx, k, b, step)};
    Operation* const pf       = ctx.create_operation(k.pfor, ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    b->append(pf);
    Block* const body = ctx.create_block(1U, ctx.type_i32());
    pf->region(0)->append(body);
    Value* iv[1] = {body->arg(0U)};
    yield_values(ctx, k, body, ConstSpan<Value*>(iv, 1U));
    ret(ctx, b, konst(ctx, k, b, 0));
    return pf;
}
} // namespace

TEST_CASE("diag 10a: a host body runs inside the nesting level of the op that runs it, as in the reference",
          "[ceir][host][diag10a]")
{
    STATIC_REQUIRE(exec::kMaxNesting == 64U);
    for (const Body body : {Body::Range, Body::Fold, Body::Launch})
    {
        INFO(body_name(body));
        crd::memory::GrowableTlsfAllocator root;
        Context                            ctx(&root);
        const Kit                          k(ctx);
        // Ten levels of descent before the body: d + depth = 30 is the deepest body that fits, 31 nests one past it.
        // A body that started again at level 0 (each sub-interpreter did) had 20 more levels than its program.
        Module* const   inside     = build(ctx, k, body, 20);
        const Outcome   ref_inside = reference(ctx, *inside, 10);
        crd::usize      pooled     = 0U;
        const Outcome   got_inside = provider(ctx, *inside, 10, &pooled);
        REQUIRE(ref_inside.error == exec::ExecError::None);
        CHECK(got_inside.error == exec::ExecError::None);
        CHECK(got_inside.value == ref_inside.value);
        CHECK(pooled == (body == Body::Launch ? 1U : 0U)); // the launch body really ran on a pool worker

        Module* const past     = build(ctx, k, body, 21);
        const Outcome ref_past = reference(ctx, *past, 10);
        const Outcome got_past = provider(ctx, *past, 10);
        REQUIRE(ref_past.error == exec::ExecError::NestingTooDeep);
        REQUIRE(ref_past.op != nullptr);
        CHECK(got_past.error == exec::ExecError::NestingTooDeep);
        CHECK(got_past.op == ref_past.op); // the same call op in @rec, on the same module
    }
}

TEST_CASE("diag 10a: a host body can use every nesting level on a pool worker's stack", "[ceir][host][diag10a]")
{
    for (const Body body : {Body::Range, Body::Fold, Body::Launch})
    {
        INFO(body_name(body));
        crd::memory::GrowableTlsfAllocator root;
        Context                            ctx(&root);
        const Kit                          k(ctx);
        // No descent: the body's ops start at level 2 and @rec(30) nests to level 63, the deepest the bound allows.
        // A pool worker ran these bodies on a 64 KiB fiber stack, a fraction of what 62 levels take.
        Module* const deepest = build(ctx, k, body, 30);
        const Outcome ref_ok  = reference(ctx, *deepest, 0);
        const Outcome got_ok  = provider(ctx, *deepest, 0);
        REQUIRE(ref_ok.error == exec::ExecError::None);
        CHECK(got_ok.error == exec::ExecError::None);

        Module* const past     = build(ctx, k, body, 31);
        const Outcome ref_past = reference(ctx, *past, 0);
        const Outcome got_past = provider(ctx, *past, 0);
        REQUIRE(ref_past.error == exec::ExecError::NestingTooDeep);
        CHECK(got_past.error == exec::ExecError::NestingTooDeep);
        CHECK(got_past.op == ref_past.op);

        Module* const hostile     = build(ctx, k, body, 1000000); // ends at the bound, never on the guard page
        const Outcome ref_hostile = reference(ctx, *hostile, 0);
        const Outcome got_hostile = provider(ctx, *hostile, 0);
        REQUIRE(ref_hostile.error == exec::ExecError::NestingTooDeep);
        CHECK(got_hostile.error == exec::ExecError::NestingTooDeep);
        CHECK(got_hostile.op == ref_hostile.op);
    }
}

TEST_CASE("diag 10a: a host parallel range across the whole i64 line has its four indices", "[ceir][host][diag10a]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    const Kit                          k(ctx);
    Module* const                      m  = ctx.create_module();
    Operation* const                   pf = parallel_range(ctx, k, *m, INT64_MIN, INT64_MAX, i64{1} << 62U);

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_task_semantics(in);
    REQUIRE(in.invoke(*m, "main", {}).ok());
    crd::memory::GrowableTlsfAllocator palloc;
    host::HostProvider                 prov(&palloc, 4U);
    REQUIRE(prov.execute(ctx, *m, "main", {}).ok());

    // `hi - lo` overflowed i64 (undefined; it wrapped to -1 and the range came out empty).
    const i64            expected[4] = {INT64_MIN, -(i64{1} << 62U), 0, i64{1} << 62U};
    const ConstSpan<i64> ref_map     = in.map_output(pf);
    const ConstSpan<i64> got_map     = prov.map_output(pf);
    REQUIRE(ref_map.size() == 4U);
    REQUIRE(got_map.size() == 4U);
    for (crd::u32 i = 0U; i < 4U; ++i)
    {
        CHECK(ref_map[i] == expected[i]);
        CHECK(got_map[i] == expected[i]);
    }
}

TEST_CASE("diag 10a: a host parallel range is charged to the run's budget before anything is sized from its count",
          "[ceir][host][diag10a]")
{
    // One step of the run's budget (2^24) per index, as in the reference. The count was cut to 32 bits, so 2^32 + 3
    // indices ran as 3 and 2^33 as none, and the per-index slots were sized from the count before any body ran (for
    // 2^31 indices about 40 GiB, which is why no such range is run here).
    for (const i64 hi : {(i64{1} << 32U) + 3, i64{1} << 33U})
    {
        INFO(hi);
        crd::memory::GrowableTlsfAllocator root;
        Context                            ctx(&root);
        const Kit                          k(ctx);
        Module* const                      m  = ctx.create_module();
        Operation* const                   pf = parallel_range(ctx, k, *m, 0, hi, 1);
        crd::memory::GrowableTlsfAllocator backing;
        CountingAllocator                  palloc(&backing);
        host::HostProvider                 prov(&palloc, 4U);
        const crd::u64                     before = palloc.allocs();
        const exec::ExecResult             r      = prov.execute(ctx, *m, "main", {});
        CHECK(r.error == exec::ExecError::FuelExhausted);
        CHECK(r.op == pf);
        CHECK(palloc.allocs() == before); // refused before a map or an error slot was sized from the count
        CHECK(prov.map_output(pf).empty());
    }
    // A range the budget covers still maps every index.
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    const Kit                          k(ctx);
    Module* const                      m  = ctx.create_module();
    Operation* const                   pf = parallel_range(ctx, k, *m, -128, 128, 1);
    crd::memory::GrowableTlsfAllocator palloc;
    host::HostProvider                 prov(&palloc, 4U);
    REQUIRE(prov.execute(ctx, *m, "main", {}).ok());
    const ConstSpan<i64> map = prov.map_output(pf);
    REQUIRE(map.size() == 256U);
    CHECK(map[0] == -128);
    CHECK(map[255] == 127);
}

TEST_CASE("diag 10a: a host map_reduce fold runs on the run's budget, as in the reference", "[ceir][host][diag10a]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    const Kit                          k(ctx);
    // @main(){ r = map_reduce(0, 8, 1, 0) { (iv): yield iv } { (a, x): for (0, 50, 1) {}; yield a + x }; return r }:
    // each fold step spends about 55 steps, 440 in all. The fold ran on one per-item budget (`sub_fuel`, here 64)
    // although it runs in index order on the submitting thread; it now spends the run's budget, as the reference's
    // does.
    Module* const    m  = ctx.create_module();
    Operation* const f  = func::create_func(ctx, *m, "main", Visibility::Public, 0U, ctx.type_i32());
    REQUIRE(f != nullptr);
    Block* const     mb = ctx.create_block(0U);
    m->body()->append(mb);
    mb->append(f);
    Block* const     b        = &func::func_body(*f);
    Value*           range[4] = {konst(ctx, k, b, 0), konst(ctx, k, b, 8), konst(ctx, k, b, 1), konst(ctx, k, b, 0)};
    Operation* const mr = ctx.create_operation(k.mreduce, ConstSpan<Value*>(range, 4U), 1U, ctx.type_i32(), 2U);
    b->append(mr);
    Block* const mapb = ctx.create_block(1U, ctx.type_i32());
    mr->region(0)->append(mapb);
    Value* iv[1] = {mapb->arg(0U)};
    yield_values(ctx, k, mapb, ConstSpan<Value*>(iv, 1U));
    Block* const comb = ctx.create_block(2U, ctx.type_i32());
    mr->region(1)->append(comb);
    Value* loop[3] = {konst(ctx, k, comb, 0), konst(ctx, k, comb, 50), konst(ctx, k, comb, 1)};
    Operation* const spin = ctx.create_operation(k.cfor, ConstSpan<Value*>(loop, 3U), 0U, {}, 1U);
    comb->append(spin);
    Block* const lb = ctx.create_block(1U, ctx.type_i32());
    spin->region(0)->append(lb);
    yield_values(ctx, k, lb, {});
    Value* sum[1] = {add(ctx, k, comb, comb->arg(0U), comb->arg(1U))};
    yield_values(ctx, k, comb, ConstSpan<Value*>(sum, 1U));
    ret(ctx, b, mr->result(0U));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_task_semantics(in);
    const exec::ExecResult ref = in.invoke(*m, "main", {});
    REQUIRE(ref.ok());
    REQUIRE(ref.values.size() == 1U);
    CHECK(ref.values[0] == 28);

    crd::memory::GrowableTlsfAllocator palloc;
    host::HostProvider                 prov(&palloc, 4U, /*sub_fuel*/ 64U);
    const exec::ExecResult             got = prov.execute(ctx, *m, "main", {});
    REQUIRE(got.ok());
    REQUIRE(got.values.size() == 1U);
    CHECK(got.values[0] == ref.values[0]);
}

TEST_CASE("diag 10a: the host provider's submitting thread spends what the reference spends", "[ceir][host][diag10a]")
{
    // Bodies that dispatch nothing (a range body yielding its index) or one op (a combine adding): the run's budget
    // then pays exactly for the program's own ops, one step per index and, for a map_reduce, one step per fold step
    // plus its add, in both executors. The provider's indices were never charged to the run, and its fold ran on a
    // budget of its own.
    struct Remaining
    {
        static void detach(exec::Interpreter& in, void* user) { *static_cast<crd::u64*>(user) = in.fuel(); }
    };
    for (const bool fold : {false, true})
    {
        const char* const kind = fold ? "map_reduce" : "parallel_for";
        INFO(kind);
        crd::memory::GrowableTlsfAllocator root;
        Context                            ctx(&root);
        const Kit                          k(ctx);
        Module* const                      m = ctx.create_module();
        if (!fold)
        {
            (void)parallel_range(ctx, k, *m, -150, 150, 1);
        }
        else
        {
            Operation* const f  = func::create_func(ctx, *m, "main", Visibility::Public, 0U, ctx.type_i32());
            REQUIRE(f != nullptr);
            Block* const     mb = ctx.create_block(0U);
            m->body()->append(mb);
            mb->append(f);
            Block* const b        = &func::func_body(*f);
            Value* range[4] = {konst(ctx, k, b, -150), konst(ctx, k, b, 150), konst(ctx, k, b, 1), konst(ctx, k, b, 0)};
            Operation* const mr = ctx.create_operation(k.mreduce, ConstSpan<Value*>(range, 4U), 1U, ctx.type_i32(), 2U);
            b->append(mr);
            Block* const mapb = ctx.create_block(1U, ctx.type_i32());
            mr->region(0)->append(mapb);
            Value* iv[1] = {mapb->arg(0U)};
            yield_values(ctx, k, mapb, ConstSpan<Value*>(iv, 1U));
            Block* const comb = ctx.create_block(2U, ctx.type_i32());
            mr->region(1)->append(comb);
            Value* sum[1] = {add(ctx, k, comb, comb->arg(0U), comb->arg(1U))};
            yield_values(ctx, k, comb, ConstSpan<Value*>(sum, 1U));
            ret(ctx, b, mr->result(0U));
        }

        exec::Interpreter in(ctx);
        exec::install_builtin_semantics(in);
        exec::install_task_semantics(in);
        REQUIRE(in.invoke(*m, "main", {}).ok());
        const crd::u64 budget = crd::u64{1} << 24U; // both executors' default run budget
        // Five ops (the range's constants, the op itself and, for parallel_for, the returned constant), 300 indices
        // and, for map_reduce, 300 fold steps of two steps each (the step and the add).
        CHECK(budget - in.fuel() == (fold ? 5U + 300U + 600U : 5U + 300U));

        crd::memory::GrowableTlsfAllocator palloc;
        host::HostProvider                 prov(&palloc, 4U);
        crd::u64                           remaining = 0U;
        const host::HostObserver           observer{nullptr, &Remaining::detach, &remaining};
        REQUIRE(prov.execute(ctx, *m, "main", {}, observer).ok());
        CHECK(remaining == in.fuel());
    }
}
