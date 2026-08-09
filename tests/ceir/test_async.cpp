// CEIR-6a (sec 37 / sec 116): the ceir.async ops + the USE-ONCE token-misuse verifier. A token (a TokenProducer result)
// must be consumed EXACTLY once by a TokenConsumer operand slot; violations are Unconsumed / MultiplyConsumed /
// ConsumedByNonConsumer. Trait-keyed (open-world). Plus: async ops are a Synchronization barrier (4d), Synchronization is
// forbidden in an audio-real-time region (the sec 32 flip), and race is Nondeterministic (illegal under Deterministic
// mode). ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/hazard.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/semantics.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;

namespace
{
struct Ops
{
    OpId launch, awaitk, join, race, cancel, addi, cst, yield, scope, cif, cfor;
    explicit Ops(Context& ctx)
        : launch(ctx.intern_op("async", "launch")), awaitk(ctx.intern_op("async", "await")),
          join(ctx.intern_op("async", "join")), race(ctx.intern_op("async", "race")),
          cancel(ctx.intern_op("async", "cancel")), addi(ctx.intern_op("arith", "addi")),
          cst(ctx.intern_op("arith", "const")), yield(ctx.intern_op("core", "yield")),
          scope(ctx.intern_op("core", "scope")), cif(ctx.intern_op("core", "if")), cfor(ctx.intern_op("core", "for"))
    {
        (void)async::register_async_ops(ctx);
        (void)arith::register_arith_ops(ctx);
        (void)core::register_core_ops(ctx);
        (void)func::register_dialect(ctx);
    }
};
Block* body(Context& ctx, Module& m)
{
    Block* b = m.body()->first_block();
    if (b == nullptr)
    {
        b = ctx.create_block(0U);
        m.body()->append(b);
    }
    return b;
}
// async.launch { <empty body> } -> %token
Operation* mklaunch(Context& ctx, const Ops& o, Block* b)
{
    Operation* const l = ctx.create_operation(o.launch, {}, 1U, ctx.type_i32(), 1U);
    l->region(0)->append(ctx.create_block(0U));
    b->append(l);
    return l;
}
// a consumer/producer op over `toks` with `nres` results, appended to `b`.
Operation* op_over(Context& ctx, OpId k, ConstSpan<Value*> toks, Block* b, u32 nres)
{
    Operation* const a = ctx.create_operation(k, toks, nres, nres != 0U ? ctx.type_i32() : TypeId{}, 0U);
    b->append(a);
    return a;
}
Operation* await1(Context& ctx, const Ops& o, Value* tok, Block* b)
{
    Value* t[1] = {tok};
    return op_over(ctx, o.awaitk, ConstSpan<Value*>(t, 1U), b, 0U);
}
Operation* cancel1(Context& ctx, const Ops& o, Value* tok, Block* b)
{
    Value* t[1] = {tok};
    return op_over(ctx, o.cancel, ConstSpan<Value*>(t, 1U), b, 0U);
}
Operation* konst(Context& ctx, const Ops& o, Block* b, i64 v)
{
    Operation* const c = ctx.create_operation(o.cst, {}, 1U, ctx.type_i32());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
} // namespace

TEST_CASE("ceir async: well-formed token flows are sound", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    SECTION("launch -> await")
    {
        Module* const m = ctx.create_module();
        Block* const  b = body(ctx, *m);
        Operation* const l = mklaunch(ctx, o, b);
        (void)await1(ctx, o, l->result(0U), b);
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None);
    }
    SECTION("launch -> cancel (a token is awaited XOR cancelled)")
    {
        Module* const m = ctx.create_module();
        Block* const  b = body(ctx, *m);
        (void)cancel1(ctx, o, mklaunch(ctx, o, b)->result(0U), b);
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None);
    }
    SECTION("a join consumes its inputs once and produces a token that is awaited")
    {
        Module* const    m  = ctx.create_module();
        Block* const     b  = body(ctx, *m);
        Operation* const l0 = mklaunch(ctx, o, b);
        Operation* const l1 = mklaunch(ctx, o, b);
        Value*           tk[2] = {l0->result(0U), l1->result(0U)};
        Operation* const j  = op_over(ctx, o.join, ConstSpan<Value*>(tk, 2U), b, 1U);
        (void)await1(ctx, o, j->result(0U), b);
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None);
    }
    SECTION("a race consumes its inputs and produces an index (not a token)")
    {
        Module* const    m  = ctx.create_module();
        Block* const     b  = body(ctx, *m);
        Operation* const l0 = mklaunch(ctx, o, b);
        Operation* const l1 = mklaunch(ctx, o, b);
        Value*           tk[2] = {l0->result(0U), l1->result(0U)};
        (void)op_over(ctx, o.race, ConstSpan<Value*>(tk, 2U), b, 1U); // the index result is not a token -> untracked
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None);
    }
}

TEST_CASE("ceir async: an unconsumed token is a leaked async op", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = body(ctx, *m);
    Operation* const             l = mklaunch(ctx, o, b); // launched, never awaited or cancelled
    const TokenMisuse            e = ctx.find_token_misuse(*m);
    CHECK(e.kind == TokenMisuseKind::Unconsumed);
    CHECK(e.op == l);
    CHECK(e.value == l->result(0U));
}

TEST_CASE("ceir async: a token consumed twice is MultiplyConsumed (slots, not ops)", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    SECTION("double await")
    {
        Module* const    m = ctx.create_module();
        Block* const     b = body(ctx, *m);
        Operation* const l = mklaunch(ctx, o, b);
        (void)await1(ctx, o, l->result(0U), b);
        Operation* const a2 = await1(ctx, o, l->result(0U), b);
        const TokenMisuse e = ctx.find_token_misuse(*m);
        CHECK(e.kind == TokenMisuseKind::MultiplyConsumed);
        CHECK(e.op == a2); // the SECOND consumer
    }
    SECTION("join(t, t) counts the token TWICE (operand slots)")
    {
        Module* const    m = ctx.create_module();
        Block* const     b = body(ctx, *m);
        Operation* const l = mklaunch(ctx, o, b);
        Value*           tk[2] = {l->result(0U), l->result(0U)}; // same token in two slots
        Operation* const j  = op_over(ctx, o.join, ConstSpan<Value*>(tk, 2U), b, 1U);
        const TokenMisuse e = ctx.find_token_misuse(*m);
        CHECK(e.kind == TokenMisuseKind::MultiplyConsumed);
        CHECK(e.op == j);
    }
}

TEST_CASE("ceir async: a token used by a NON-consumer is ConsumedByNonConsumer (region/func confined)", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    SECTION("token fed to arith")
    {
        Module* const    m = ctx.create_module();
        Block* const     b = body(ctx, *m);
        Operation* const l = mklaunch(ctx, o, b);
        Value*           t2[2] = {l->result(0U), konst(ctx, o, b, 1)->result(0U)};
        Operation* const bad = op_over(ctx, o.addi, ConstSpan<Value*>(t2, 2U), b, 1U);
        const TokenMisuse e  = ctx.find_token_misuse(*m);
        CHECK(e.kind == TokenMisuseKind::ConsumedByNonConsumer);
        CHECK(e.op == bad);
    }
    SECTION("token YIELDED out of a region (structured confinement)")
    {
        Module* const    m = ctx.create_module();
        Block* const     b = body(ctx, *m);
        Operation* const l = mklaunch(ctx, o, b);
        Value*           t[1] = {l->result(0U)};
        Operation* const y = op_over(ctx, o.yield, ConstSpan<Value*>(t, 1U), b, 0U); // yield is not a TokenConsumer
        const TokenMisuse e = ctx.find_token_misuse(*m);
        CHECK(e.kind == TokenMisuseKind::ConsumedByNonConsumer);
        CHECK(e.op == y);
    }
    SECTION("token passed to a func.call (single-function confinement)")
    {
        Module* const    m = ctx.create_module();
        Block* const     b = body(ctx, *m);
        Operation* const l = mklaunch(ctx, o, b);
        Value*           t[1] = {l->result(0U)};
        Operation* const call = func::create_call(ctx, "g", ConstSpan<Value*>(t, 1U), 0U);
        b->append(call);
        const TokenMisuse e = ctx.find_token_misuse(*m);
        CHECK(e.kind == TokenMisuseKind::ConsumedByNonConsumer);
        CHECK(e.op == call);
    }
}

TEST_CASE("ceir async: a token consumed in BOTH if-branches is flagged (no path-sensitivity)", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = body(ctx, *m);
    Operation* const             l = mklaunch(ctx, o, b);
    Operation* const             cond = konst(ctx, o, b, 1);
    Value*                       ca[1] = {cond->result(0U)};
    Operation* const             ifop = ctx.create_operation(o.cif, ConstSpan<Value*>(ca, 1U), 0U, {}, 2U);
    b->append(ifop);
    Block* const tb = ctx.create_block(0U);
    ifop->region(0)->append(tb);
    (void)await1(ctx, o, l->result(0U), tb); // consumed in THEN
    Block* const eb = ctx.create_block(0U);
    ifop->region(1)->append(eb);
    (void)await1(ctx, o, l->result(0U), eb);                       // ...and in ELSE -> 2 static uses
    CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::MultiplyConsumed);
}

TEST_CASE("ceir async: the use-once discipline keys on the TRAIT, not the op name (open-world)", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    // a hand dialect: hw.spawn (TokenProducer) + hw.wait (TokenConsumer).
    Dialect* const hw    = ctx.register_dialect("hw");
    const OpId     spawn = hw->register_op("spawn", {.traits = flags_of(OpTrait::TokenProducer)});
    const OpId     wait  = hw->register_op("wait", {.traits = flags_of(OpTrait::TokenConsumer)});

    SECTION("spawn -> wait is sound")
    {
        Module* const    m = ctx.create_module();
        Block* const     b = body(ctx, *m);
        Operation* const s = ctx.create_operation(spawn, {}, 1U, ctx.type_i32());
        b->append(s);
        Value* t[1] = {s->result(0U)};
        b->append(ctx.create_operation(wait, ConstSpan<Value*>(t, 1U), 0U));
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None);
    }
    SECTION("an unwaited hand-token is Unconsumed")
    {
        Module* const    m = ctx.create_module();
        Block* const     b = body(ctx, *m);
        Operation* const s = ctx.create_operation(spawn, {}, 1U, ctx.type_i32());
        b->append(s);
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::Unconsumed);
    }
}

TEST_CASE("ceir async: async ops are a Synchronization barrier (4d hazards)", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m  = ctx.create_module();
    Block* const                 b  = body(ctx, *m);
    Operation* const             l0 = mklaunch(ctx, o, b);
    Operation* const             l1 = mklaunch(ctx, o, b);
    // both declare an ambient Synchronization effect -> ResourceClass::Universe rw -> a full barrier (WAW), never reordered.
    CHECK(ctx.ops_hazard(*l0, *l1) == HazardKind::Waw);
}

TEST_CASE("ceir async: a blocking wait is illegal in an audio-real-time region (the sec 32 flip)", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);

    SECTION("an async op in an audio region is flagged (Synchronization forbidden)")
    {
        Module* const    m  = ctx.create_module();
        Block* const     b  = body(ctx, *m);
        Operation* const sc = ctx.create_operation(o.scope, {}, 0U, {}, 1U);
        ctx.set_region_exec(sc, RegionExec{EvalDomain::Unspecified, RealtimeClass::AudioRealTime});
        b->append(sc);
        Block* const rb = ctx.create_block(0U);
        sc->region(0)->append(rb);
        Operation* const l = mklaunch(ctx, o, rb);
        (void)await1(ctx, o, l->result(0U), rb);
        const DomainViolation v = ctx.find_domain_violation(*m);
        REQUIRE(v.op != nullptr);
        CHECK(v.effect == EffectFamily::Synchronization);
    }
    SECTION("a sync-free audio region passes")
    {
        Module* const    m  = ctx.create_module();
        Block* const     b  = body(ctx, *m);
        Operation* const sc = ctx.create_operation(o.scope, {}, 0U, {}, 1U);
        ctx.set_region_exec(sc, RegionExec{EvalDomain::Unspecified, RealtimeClass::AudioRealTime});
        b->append(sc);
        Block* const rb = ctx.create_block(0U);
        sc->region(0)->append(rb);
        (void)konst(ctx, o, rb, 7); // a pure compute op is fine in an audio region
        CHECK(ctx.find_domain_violation(*m).op == nullptr);
    }
}

TEST_CASE("ceir async: race is Nondeterministic; async ops fail Deterministic compiler mode", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m  = ctx.create_module();
    Block* const                 b  = body(ctx, *m);
    Operation* const             l0 = mklaunch(ctx, o, b);
    Operation* const             l1 = mklaunch(ctx, o, b);
    Value*                       tk[2] = {l0->result(0U), l1->result(0U)};
    (void)op_over(ctx, o.race, ConstSpan<Value*>(tk, 2U), b, 1U);

    // race declares a POSITIVE Nondeterministic claim (the winner is not fixed); launch makes NO determinism claim
    // (Unspecified -- its reproducibility is the provider's concern, pending CEIR-6b).
    CHECK(ctx.op_determinism(o.race) == DeterminismClass::Nondeterministic);
    CHECK(ctx.op_determinism(o.launch) == DeterminismClass::Unspecified);
    // Normal mode: no constraint. Deterministic mode: an async op (no deterministic tier) is flagged -- the FIRST in
    // pre-order (Unspecified fails any mode stricter than Normal, exactly like the Nondeterministic race).
    CHECK(ctx.find_mode_violation(*m) == nullptr);
    ctx.set_compiler_mode(CompilerMode::Deterministic);
    CHECK(ctx.find_mode_violation(*m) == l0);
}

TEST_CASE("ceir async: verifying an UNREGISTERED module vacuously passes (register-to-verify direction)", "[ceir][async]")
{
    // ⛔ The register-to-verify contract's FAILURE DIRECTION is the OPPOSITE of 5d: 5d over-flags an unregistered module
    // (noisy, safe); here an unregistered module has NO TokenProducer traits -> zero tokens tracked -> a leak passes
    // VACUOUSLY (a false NEGATIVE). Can't fix structurally (traits are registry state; name-sniffing breaks I6) -- the
    // consumer MUST register the dialect. Late binding works: OpId is a content hash, has_trait binds at query time.
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    (void)mklaunch(ctx, o, body(ctx, *m));                            // an unconsumed (leaked) token
    REQUIRE(ctx.find_token_misuse(*m).kind == TokenMisuseKind::Unconsumed); // registered ctx: caught

    const String text = print(ctx, *m, &root);
    Context      ctx2(&root);
    (void)arith::register_arith_ops(ctx2); // register OTHER dialects, deliberately NOT async
    (void)core::register_core_ops(ctx2);
    const ParseResult pr = parse(ctx2, text);
    REQUIRE(pr.ok);
    CHECK(ctx2.find_token_misuse(*pr.module).kind == TokenMisuseKind::None); // VACUOUS: no async traits -> nothing tracked
    (void)async::register_async_ops(ctx2);                                   // register async LATE
    CHECK(ctx2.find_token_misuse(*pr.module).kind == TokenMisuseKind::Unconsumed); // now the leak is caught
}

TEST_CASE("ceir async: a call to an async helper flags Synchronization in an audio region (5c effective-effects)", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    // @helper(): launch + await (a blocking wait), consumed correctly.
    Operation* const helper = func::create_func(ctx, *m, "helper", Visibility::Public, 0U);
    body(ctx, *m)->append(helper);
    Block* const     hb = func::func_body_block(helper);
    Operation* const l  = mklaunch(ctx, o, hb);
    (void)await1(ctx, o, l->result(0U), hb);
    hb->append(func::create_return(ctx, {}));
    // an audio-tagged scope that CALLS @helper -- the call's EFFECTIVE effects (5c) transitively include Synchronization.
    Operation* const sc = ctx.create_operation(o.scope, {}, 0U, {}, 1U);
    ctx.set_region_exec(sc, RegionExec{EvalDomain::Unspecified, RealtimeClass::AudioRealTime});
    body(ctx, *m)->append(sc);
    Block* const rb = ctx.create_block(0U);
    sc->region(0)->append(rb);
    rb->append(func::create_call(ctx, "helper", {}, 0U));
    const DomainViolation v = ctx.find_domain_violation(*m);
    REQUIRE(v.op != nullptr);
    CHECK(v.effect == EffectFamily::Synchronization); // the call, resolved through 5c, is a blocking wait in audio
}

TEST_CASE("ceir async: a token consumed inside a LOOP body is one static use (edge d, documented gap)", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Block* const                 b = body(ctx, *m);
    Operation* const             l = mklaunch(ctx, o, b); // launched OUTSIDE the loop
    Value* lohilst[3] = {konst(ctx, o, b, 0)->result(0U), konst(ctx, o, b, 3)->result(0U),
                         konst(ctx, o, b, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    b->append(forop);
    Block* const fb = ctx.create_block(1U, ctx.type_i32());
    forop->region(0)->append(fb);
    (void)await1(ctx, o, l->result(0U), fb); // consumed inside the loop body -- ONE static use (though N dynamic awaits)
    CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None); // dynamic multiplicity needs execution -- a gap
}

TEST_CASE("ceir async: async is a SEPARATE installer -- builtin semantics alone leave it NoSemantics", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);
    (void)mklaunch(ctx, o, mb);
    mb->append(func::create_return(ctx, {}));
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in); // arith/core/func only -- async is a SEPARATE installer (CEIR-6b)
    CHECK(in.invoke(*m, "main", {}).error == exec::ExecError::NoSemantics);
}

TEST_CASE("ceir async: SEQUENTIAL-reference execution -- launch runs its body, await returns the yield", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);
    // %t = async.launch { %c = const 42; core.yield %c }
    Operation* const l  = ctx.create_operation(o.launch, {}, 1U, ctx.type_i32(), 1U);
    Block* const     lb = ctx.create_block(0U);
    l->region(0)->append(lb);
    Value* yv[1] = {konst(ctx, o, lb, 42)->result(0U)};
    lb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(yv, 1U), 0U));
    mb->append(l);
    // %r = async.await(%t)
    Value*           tk[1] = {l->result(0U)};
    Operation* const aw    = ctx.create_operation(o.awaitk, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32());
    mb->append(aw);
    Value* rv[1] = {aw->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in); // the SEQUENTIAL async reference (CEIR-6b; jobs-backed parallel async is 6c)
    const exec::ExecResult r = in.invoke(*m, "main", {});
    REQUIRE(r.ok());
    CHECK(r.values[0] == 42); // launch ran the body at launch; await returned its yield
}

TEST_CASE("ceir async: awaiting a FORGED token (not a live session handle) is BadToken", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);
    // %c = const 99 (a plain integer, NOT a launch token); await(%c) is a forged handle -> a TYPED error, not silent empty.
    Value*           tk[1] = {konst(ctx, o, mb, 99)->result(0U)};
    Operation* const aw    = ctx.create_operation(o.awaitk, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32());
    mb->append(aw);
    Value* rv[1] = {aw->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    CHECK(in.invoke(*m, "main", {}).error == exec::ExecError::BadToken); // token handles are session-scoped
}

TEST_CASE("ceir async: a structured scope runs sequentially and forwards its yield", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   ascope = ctx.intern_op("async", "scope");
    Module* const                m = ctx.create_module();
    Operation* const             fmain = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    body(ctx, *m)->append(fmain);
    Block* const mb = func::func_body_block(fmain);
    // %r = async.scope { %t = launch { yield 7 }; %v = await(%t); yield %v }
    Operation* const sc  = ctx.create_operation(ascope, {}, 1U, ctx.type_i32(), 1U);
    Block* const     scb = ctx.create_block(0U);
    sc->region(0)->append(scb);
    Operation* const l  = ctx.create_operation(o.launch, {}, 1U, ctx.type_i32(), 1U);
    Block* const     lb = ctx.create_block(0U);
    l->region(0)->append(lb);
    Value* ly[1] = {konst(ctx, o, lb, 7)->result(0U)};
    lb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(ly, 1U), 0U));
    scb->append(l);
    Value*           tk[1] = {l->result(0U)};
    Operation* const aw    = ctx.create_operation(o.awaitk, ConstSpan<Value*>(tk, 1U), 1U, ctx.type_i32());
    scb->append(aw);
    Value* sy[1] = {aw->result(0U)};
    scb->append(ctx.create_operation(o.yield, ConstSpan<Value*>(sy, 1U), 0U)); // the scope's yield
    mb->append(sc);
    Value* rv[1] = {sc->result(0U)};
    mb->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));

    REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None); // structurally sound
    REQUIRE(ctx.find_token_misuse(*m).kind == TokenMisuseKind::None);       // the token is consumed once, inside the scope
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    const exec::ExecResult r = in.invoke(*m, "main", {});
    REQUIRE(r.ok());
    CHECK(r.values[0] == 7); // scope forwarded the awaited launch yield
}

TEST_CASE("ceir async: scope-confinement is LAYERED verification (5b escape + 6a leak), not new code", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   ascope = ctx.intern_op("async", "scope");

    SECTION("a scope-launched token used OUTSIDE the scope is UseBeforeDef (5b blocks escape)")
    {
        Module* const    m  = ctx.create_module();
        Block* const     b  = body(ctx, *m);
        Operation* const sc = ctx.create_operation(ascope, {}, 0U, {}, 1U);
        Block* const     scb = ctx.create_block(0U);
        sc->region(0)->append(scb);
        Operation* const l = mklaunch(ctx, o, scb); // a token defined INSIDE the scope region
        b->append(sc);
        (void)await1(ctx, o, l->result(0U), b); // ...awaited OUTSIDE it -> the value is not visible in the parent
        CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::UseBeforeDef);
    }
    SECTION("a scope-launched token never consumed is Unconsumed (6a catches the leak)")
    {
        Module* const    m  = ctx.create_module();
        Block* const     b  = body(ctx, *m);
        Operation* const sc = ctx.create_operation(ascope, {}, 0U, {}, 1U);
        Block* const     scb = ctx.create_block(0U);
        sc->region(0)->append(scb);
        (void)mklaunch(ctx, o, scb); // launched, never awaited/cancelled -> leaked
        b->append(sc);
        CHECK(ctx.find_token_misuse(*m).kind == TokenMisuseKind::Unconsumed);
    }
}

TEST_CASE("ceir async: a scope is a Synchronization join point -- flagged in an audio region", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    const OpId                   ascope = ctx.intern_op("async", "scope");
    Module* const                m  = ctx.create_module();
    Block* const                 b  = body(ctx, *m);
    // an audio-tagged core.scope containing an async.scope (Synchronization) -> the join-at-exit blocks -> flagged.
    Operation* const owner = ctx.create_operation(o.scope, {}, 0U, {}, 1U); // core.scope region holder
    ctx.set_region_exec(owner, RegionExec{EvalDomain::Unspecified, RealtimeClass::AudioRealTime});
    b->append(owner);
    Block* const rb = ctx.create_block(0U);
    owner->region(0)->append(rb);
    Operation* const asc = ctx.create_operation(ascope, {}, 0U, {}, 1U);
    rb->append(asc);
    asc->region(0)->append(ctx.create_block(0U));
    const DomainViolation v = ctx.find_domain_violation(*m);
    REQUIRE(v.op != nullptr);
    CHECK(v.effect == EffectFamily::Synchronization);
}

TEST_CASE("ceir async: cooperative cancellation is distinct from fuel exhaustion", "[ceir][async]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Ops                    o(ctx);
    // @spin(): for(0, 1000000, 1) { } ; return -- a long loop.
    Module* const    m  = ctx.create_module();
    Operation* const f  = func::create_func(ctx, *m, "spin", Visibility::Public, 0U);
    body(ctx, *m)->append(f);
    Block* const mb = func::func_body_block(f);
    Value* lohilst[3] = {konst(ctx, o, mb, 0)->result(0U), konst(ctx, o, mb, 1000000)->result(0U),
                         konst(ctx, o, mb, 1)->result(0U)};
    Operation* const forop = ctx.create_operation(o.cfor, ConstSpan<Value*>(lohilst, 3U), 0U, {}, 1U);
    mb->append(forop);
    forop->region(0)->append(ctx.create_block(1U, ctx.type_i32()));
    mb->append(func::create_return(ctx, {}));

    SECTION("a set cancel flag -> Cancelled (observed in the step loop, before fuel runs out)")
    {
        std::atomic<bool> cancel{true};
        exec::Interpreter in(ctx, crd::u64{1} << 30U); // huge fuel: only cancel can stop it
        exec::install_builtin_semantics(in);
        in.set_cancel_flag(&cancel);
        CHECK(in.invoke(*m, "spin", {}).error == exec::ExecError::Cancelled);
    }
    SECTION("no cancel + tiny fuel -> FuelExhausted (the contrast)")
    {
        exec::Interpreter in(ctx, 500U); // tiny fuel, no cancel
        exec::install_builtin_semantics(in);
        CHECK(in.invoke(*m, "spin", {}).error == exec::ExecError::FuelExhausted);
    }
}
