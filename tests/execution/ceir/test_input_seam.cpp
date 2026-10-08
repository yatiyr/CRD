// DIAG.9a -- the host INPUT SEAM (input.hpp). A program reads host-chosen values through the `input` dialect
// (input.random today), and both executors deliver them through one InputSource the host installs: the reference
// interpreter (Interpreter::set_input_source) and the compiled plan (the `inputs` argument of plan::run). The seam
// delivers the raw draw; the op reduces it to [0, bound), so both executors answer the same reduced values for the same
// source. With no source a read fails with a typed InputUnavailable error at the op, never with a made-up value. A
// read is schedule-dependent like a §20 cell, so a parallel body (or a callee it reaches) may not contain one. Expected
// values come from SeededInputs::draw, computed here independently of either executor. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/effect.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/input.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/ceir/semantics.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
constexpr u64 kSeed = 0x5EED0123456789ULL;

// main(%n) = draw(7) * n + draw(3) + draw(7), each reduced to [0, 1000).
constexpr const char* kThreeDraws = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0(%0 : !i32):
        %1 = input.random() {stream = 7, bound = 1000} : !i32
        %2 = input.random() {stream = 3, bound = 1000} : !i32
        %3 = input.random() {stream = 7, bound = 1000} : !i32
        %4 = arith.muli(%1, %0) : !i32
        %5 = arith.addi(%4, %2) : !i32
        %6 = arith.addi(%5, %3) : !i32
        func.return(%6)
    }
})";

void register_dialects(Context& ctx)
{
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)task::register_task_ops(ctx);
    (void)func::register_dialect(ctx);
    (void)input::register_input_ops(ctx);
}

Module* parse_ok(Context& ctx, StringView text)
{
    register_dialects(ctx);
    const ParseResult pr = parse(ctx, text);
    REQUIRE(pr.module != nullptr);
    ctx.assign_stable_ids(*pr.module);
    return pr.module;
}

// The `n`-th input.random op of `m` in pre-order (0-based).
const Operation* nth_random(const Context& ctx, const Region* r, u32& n)
{
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (input::reads_input(ctx, op->kind()))
            {
                if (n == 0U)
                {
                    return op;
                }
                --n;
            }
            for (u32 i = 0U; i < op->num_regions(); ++i)
            {
                if (const Operation* const found = nth_random(ctx, op->region(i), n); found != nullptr)
                {
                    return found;
                }
            }
        }
    }
    return nullptr;
}

const Operation* random_op(const Context& ctx, const Module& m, u32 n)
{
    const Operation* const op = nth_random(ctx, m.body(), n);
    REQUIRE(op != nullptr);
    return op;
}

i64 reduced(u64 seed, u32 stream, u64 n, u64 bound)
{
    return input::reduce_draw(input::SeededInputs::draw(seed, stream, n), bound);
}

exec::ExecResult interpret(Context& ctx, const Module& m, ConstSpan<i64> args, const input::InputSource* source)
{
    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in);
    exec::install_task_semantics(in);
    exec::install_input_semantics(in);
    in.set_input_source(source);
    return in.invoke(m, StringView{"main"}, args);
}

// A source that answers every stream but `refused`.
struct AllBut
{
    u32                refused = 0U;
    u32                reads   = 0U;
    input::InputSource source{&AllBut::next, this};
    static bool        next(input::InputKind /*kind*/, u32 channel, i64& out, void* user)
    {
        auto& self = *static_cast<AllBut*>(user);
        ++self.reads;
        out = 41;
        return channel != self.refused;
    }
};
} // namespace

TEST_CASE("diag 9a input: the seeded source draws each stream independently and deterministically",
          "[ceir][input][diag 9a]")
{
    memory::GrowableTlsfAllocator alloc;
    input::SeededInputs           a(kSeed, &alloc);
    input::SeededInputs           b(kSeed, &alloc);
    i64                           v = 0;

    // a: 7, 3, 7, 7. b: 3 first, then 7 three times. Each stream's sequence is its own.
    i64 a7[3] = {};
    i64 a3    = 0;
    REQUIRE(input::read_input(a.source(), input::InputKind::Random, 7U, a7[0]));
    REQUIRE(input::read_input(a.source(), input::InputKind::Random, 3U, a3));
    REQUIRE(input::read_input(a.source(), input::InputKind::Random, 7U, a7[1]));
    REQUIRE(input::read_input(a.source(), input::InputKind::Random, 7U, a7[2]));
    REQUIRE(input::read_input(b.source(), input::InputKind::Random, 3U, v));
    CHECK(v == a3);
    for (const i64 expected : a7)
    {
        REQUIRE(input::read_input(b.source(), input::InputKind::Random, 7U, v));
        CHECK(v == expected);
    }
    CHECK(a7[0] == input::SeededInputs::draw(kSeed, 7U, 0U));
    CHECK(a7[1] == input::SeededInputs::draw(kSeed, 7U, 1U));
    CHECK(a7[2] == input::SeededInputs::draw(kSeed, 7U, 2U));
    CHECK(a3 == input::SeededInputs::draw(kSeed, 3U, 0U));
    CHECK(a7[0] != a7[1]);
    CHECK(a7[0] != a3);
    CHECK(input::SeededInputs::draw(kSeed + 1U, 7U, 0U) != a7[0]);
    CHECK(a.seed() == kSeed);

    // No source, or one without `next`, has no value.
    CHECK_FALSE(input::read_input(nullptr, input::InputKind::Random, 0U, v));
    const input::InputSource empty{};
    CHECK_FALSE(input::read_input(&empty, input::InputKind::Random, 0U, v));
    CHECK(input::input_kind_name(input::InputKind::Random) == StringView{"random"});
    CHECK(input::reduce_draw(-1, 10U) == static_cast<i64>(~u64{0U} % 10U)); // unsigned remainder
}

TEST_CASE("diag 9a input: both executors read the seam in program order and reduce the draw identically",
          "[ceir][input][diag 9a]")
{
    memory::GrowableTlsfAllocator alloc;
    Context                       ctx(&alloc);
    const Module* const           m = parse_ok(ctx, StringView{kThreeDraws});

    // The op declares its own input family, a host provider and an external determinism claim.
    const OpId kind    = input::random_kind(ctx);
    bool       reads_rng = false;
    for (const EffectRecord& e : ctx.op_effects(kind))
    {
        reads_rng = reads_rng || e.family == EffectFamily::RandomRead;
    }
    CHECK(reads_rng);
    CHECK(ctx.op_determinism(kind) == DeterminismClass::ExternalNondeterminism);
    REQUIRE(ctx.op_info(kind) != nullptr);
    CHECK(ctx.op_info(kind)->native_provider == StringView{"host"});
    CHECK(input::reads_input(ctx, kind));
    CHECK_FALSE(input::reads_input(ctx, ctx.intern_op("arith", "addi")));

    for (const i64 n : {i64{0}, i64{1}, i64{5}})
    {
        const i64 expected =
            reduced(kSeed, 7U, 0U, 1000U) * n + reduced(kSeed, 3U, 0U, 1000U) + reduced(kSeed, 7U, 1U, 1000U);
        const i64 args[1] = {n};

        input::SeededInputs    ref_src(kSeed, &alloc);
        const exec::ExecResult ref = interpret(ctx, *m, ConstSpan<i64>(args, 1U), ref_src.source());
        REQUIRE(ref.ok());
        REQUIRE(ref.values.size() == 1U);
        CHECK(ref.values[0] == expected);

        plan::CompileResult cr = plan::compile(ctx, *m, StringView{"main"}, &alloc);
        REQUIRE(cr.ok());
        input::SeededInputs   plan_src(kSeed, &alloc);
        const plan::RunResult rr =
            plan::run(cr.plan, ConstSpan<i64>(args, 1U), &alloc, plan::RunHooks{}, nullptr, plan_src.source());
        REQUIRE(rr.ok());
        REQUIRE(rr.values.size() == 1U);
        CHECK(rr.values[0] == expected);
    }
}

TEST_CASE("diag 9a input: with no value for a read both executors fail InputUnavailable at that op",
          "[ceir][input][diag 9a]")
{
    memory::GrowableTlsfAllocator alloc;
    Context                       ctx(&alloc);
    const Module* const           m       = parse_ok(ctx, StringView{kThreeDraws});
    const i64                     args[1] = {2};
    plan::CompileResult           cr      = plan::compile(ctx, *m, StringView{"main"}, &alloc);
    REQUIRE(cr.ok());

    SECTION("no source: the first read fails")
    {
        const exec::ExecResult ref = interpret(ctx, *m, ConstSpan<i64>(args, 1U), nullptr);
        CHECK(ref.error == exec::ExecError::InputUnavailable);
        CHECK(ref.op == random_op(ctx, *m, 0U));
        CHECK(exec::exec_error_name(ref.error) == StringView{"input-unavailable"});

        const plan::RunResult rr = plan::run(cr.plan, ConstSpan<i64>(args, 1U), &alloc);
        CHECK(rr.error == plan::RunError::InputUnavailable);
        REQUIRE(rr.fault.valid());
        CHECK(plan::instr_provenance(cr.plan, rr.fault).op == random_op(ctx, *m, 0U)->stable_id());
        CHECK(plan::run_error_name(rr.error) == StringView{"input-unavailable"});
    }
    SECTION("a host that has no value for stream 3: the second read fails, after one read of stream 7")
    {
        AllBut                 ref_src;
        ref_src.refused          = 3U;
        const exec::ExecResult ref = interpret(ctx, *m, ConstSpan<i64>(args, 1U), &ref_src.source);
        CHECK(ref.error == exec::ExecError::InputUnavailable);
        CHECK(ref.op == random_op(ctx, *m, 1U));
        CHECK(ref_src.reads == 2U);

        AllBut plan_src;
        plan_src.refused         = 3U;
        const plan::RunResult rr = plan::run(cr.plan, ConstSpan<i64>(args, 1U), &alloc, plan::RunHooks{}, nullptr,
                                             &plan_src.source);
        CHECK(rr.error == plan::RunError::InputUnavailable);
        REQUIRE(rr.fault.valid());
        CHECK(plan::instr_provenance(cr.plan, rr.fault).op == random_op(ctx, *m, 1U)->stable_id());
        CHECK(plan_src.reads == 2U);
    }
}

TEST_CASE("diag 9a input: an out-of-range stream or bound is refused like a bad constant", "[ceir][input][diag 9a]")
{
    const char* const attrs[] = {"stream = 0, bound = 0", "stream = -1, bound = 4", "stream = 4294967296, bound = 4",
                                 "stream = 0, bound = 4294967296", "stream = 0"};
    for (const char* const a : attrs)
    {
        INFO(a);
        memory::GrowableTlsfAllocator alloc;
        Context                       ctx(&alloc);
        containers::String            text(&alloc);
        text.append("module {\n  ^bb0:\n    func.func() {sym_name = \"main\"} {\n      ^bb0:\n"
                    "        %0 = input.random() {");
        text.append(StringView{a});
        text.append("} : !i32\n        func.return(%0)\n    }\n}\n");
        register_dialects(ctx);
        const ParseResult pr = parse(ctx, StringView{text.data(), text.size()});
        REQUIRE(pr.module != nullptr);
        ctx.assign_stable_ids(*pr.module);

        input::SeededInputs    src(kSeed, &alloc);
        const exec::ExecResult ref = interpret(ctx, *pr.module, {}, src.source());
        CHECK(ref.error == exec::ExecError::UndefinedValue);
        CHECK(ref.op == random_op(ctx, *pr.module, 0U));

        const plan::CompileResult cr = plan::compile(ctx, *pr.module, StringView{"main"}, &alloc);
        CHECK(cr.error == plan::CompileError::BadConst);
        CHECK(cr.op == random_op(ctx, *pr.module, 0U));
    }
}

TEST_CASE("diag 9a input: a parallel body, or a callee it reaches, may not read a host input", "[ceir][input][diag 9a]")
{
    // The body reads directly, or through @draw. Either way the shared pre-flight blames the read itself.
    const char* const bodies[] = {"            %5 = input.random() {stream = 0, bound = 10} : !i32\n",
                                  "            %5 = func.call() {callee = @draw} : !i32\n"};
    for (const char* const body : bodies)
    {
        INFO(body);
        memory::GrowableTlsfAllocator alloc;
        Context                       ctx(&alloc);
        containers::String            text(&alloc);
        text.append("module {\n  ^bb0:\n"
                    "    func.func() {sym_name = \"draw\"} {\n      ^bb0:\n"
                    "        %0 = input.random() {stream = 1, bound = 10} : !i32\n        func.return(%0)\n    }\n"
                    "    func.func() {sym_name = \"main\"} {\n      ^bb0:\n"
                    "        %1 = arith.const() {value = 0} : !i32\n        %2 = arith.const() {value = 4} : !i32\n"
                    "        %3 = arith.const() {value = 1} : !i32\n"
                    "        task.parallel_for(%1, %2, %3) {\n          ^bb0(%4 : !i32):\n");
        text.append(StringView{body});
        text.append("            core.yield(%5)\n        }\n        func.return(%1)\n    }\n}\n");
        register_dialects(ctx);
        const ParseResult pr = parse(ctx, StringView{text.data(), text.size()});
        REQUIRE(pr.module != nullptr);
        ctx.assign_stable_ids(*pr.module);
        const bool             direct = StringView{body}.find(StringView{"input.random"}) != StringView::npos;
        const Operation* const read   = random_op(ctx, *pr.module, direct ? 1U : 0U);

        const exec::PreflightResult pf = exec::preflight_parallel(ctx, *pr.module);
        CHECK(pf.err == exec::ExecError::ParallelBodyStateful);
        CHECK(pf.op == read);

        input::SeededInputs    src(kSeed, &alloc);
        const exec::ExecResult ref = interpret(ctx, *pr.module, {}, src.source());
        CHECK(ref.error == exec::ExecError::ParallelBodyStateful);

        const plan::CompileResult cr = plan::compile(ctx, *pr.module, StringView{"main"}, &alloc);
        CHECK(cr.error == plan::CompileError::ParallelStateful);
        CHECK(cr.op == read);
    }
}
