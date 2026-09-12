// test_large_graph.cpp — CEIR-35d Q6: LARGE-GRAPH scalability CORRECTNESS. Build a synthetic ~8k-op CEIR arith graph
// (a chain of arith.addi over const-i summing to a known value), run the FULL compiler on it (cook_program: lower /
// verify / optimize / serialize) at scale, load the cooked blob, and execute it through the REFERENCE Interpreter —
// asserting the EXACT numeric result and completion. This is the scale-CORRECTNESS gate: it proves the compiler +
// executor handle a graph an order of magnitude larger than the hand-authored ones WITHOUT a pathological blowup
// (no O(N^2) hang, no fixed-size-buffer overflow, no per-operand recursion stack overflow — the reference eval is
// block-linear). The result assertion holds whether or not the optimizer const-folds the chain: folded ⇒ the compiler
// still ingested + folded ~8k ops (O(N) at scale); not folded ⇒ the whole stack (deserialize + eval) runs at scale.
// The time-SCALING measurement (does compile/exec time grow linearly) is the 35d Q7 perf board — this is correctness.
// ⛔ ASCII test names (ctest-by-name); no std containers.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/hot_reload.hpp>   // ReloadSet — the proven cook->load->module path (reused, not re-derived)
#include <crd/ceir/cook/program_cook.hpp> // cook_program / CookResult
#include <crd/ceir/exec.hpp>              // Interpreter / install_builtin_semantics / ExecResult
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility> // std::move

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)  -- ReloadSet / AssetId / cook_program
using namespace crd::ceir::exec; // NOLINT(google-build-using-namespace)  -- Interpreter / install_builtin_semantics
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;
using crd::u64;
using crd::u8;

namespace
{
void registrar(Context& c, void* /*user*/)
{
    (void)arith::register_arith_ops(c);
    (void)core::register_core_ops(c);
    (void)func::register_dialect(c);
}
ConstSpan<u8> span_of(const Array<u8>& b) { return ConstSpan<u8>(b.data(), b.size()); }
} // namespace

TEST_CASE("ceir 35d: a large ~8k-op arith graph cooks, loads, and executes to the exact sum", "[ceir][cook][ceir35d]")
{
    crd::memory::GrowableTlsfAllocator root;
    constexpr i64 n     = 4096;   // graph size: n const + n addi = ~8192 ops (an order of magnitude past the authored graphs)
    constexpr u64 asset = 3600U;

    // COMPILER at scale: build @f() -> i64 { acc = 0; for i in 1..n: acc = addi(acc, const i); return acc } and cook it
    // (lower / verify / optimize / serialize). acc = sum(0..n) = n(n+1)/2. The builder loop is iterative (no builder-side
    // recursion); the SCALE is entirely on the compiler passes over the ~8k-op body.
    Array<u8> blob(&root);
    {
        Context    c(&root);
        const OpId cst = c.intern_op("arith", "const");
        const OpId add = c.intern_op("arith", "addi");
        (void)arith::register_arith_ops(c);
        (void)core::register_core_ops(c);
        (void)func::register_dialect(c);
        Module* const m = c.create_module();
        m->body()->append(c.create_block(0U));
        const TypeId     ty = c.type_i64();
        Operation* const f  = func::create_func(c, *m, "f", Visibility::Public, 0U, ty);
        m->body()->first_block()->append(f);
        Block* const b = func::func_body_block(f);

        const auto konst = [&](i64 v) -> Value* {
            Operation* const op = c.create_operation(cst, {}, 1U, ty);
            c.set_attr(op, "value", c.attr_int(v));
            b->append(op);
            return op->result(0U);
        };

        Value* acc = konst(0);
        for (i64 i = 1; i <= n; ++i)
        {
            Value* const operands[2] = {acc, konst(i)};
            Operation* const a       = c.create_operation(add, ConstSpan<Value*>(operands, 2U), 1U, ty);
            b->append(a);
            acc = a->result(0U);
        }
        Value* const rv[1] = {acc};
        b->append(func::create_return(c, ConstSpan<Value*>(rv, 1U)));

        CookResult cr = cook_program(c, *m, asset, &root, &root);
        REQUIRE(cr.ok()); // the compiler handled the ~8k-op graph (no blowup, no reject)
        blob = std::move(cr.blob);
    } // the build Context is freed here; only the cooked blob survives into the load below

    // EXECUTOR at scale: load the cooked blob (deserialize) + invoke through the reference interpreter.
    ReloadSet set(&root, &registrar, nullptr);
    REQUIRE(set.add(AssetId{asset}, span_of(blob)).ok());
    Generation* const g = set.generation(AssetId{asset});
    REQUIRE(g != nullptr);
    Interpreter in(*g->ctx);
    install_builtin_semantics(in);
    const ExecResult r = in.invoke(*g->program.module, StringView("f"), ConstSpan<i64>());
    REQUIRE(r.ok());
    REQUIRE(r.values.size() == 1U);
    const i64 expected = n * (n + 1) / 2; // 4096 * 4097 / 2 = 8390656 (fits i64, no overflow)
    CHECK(r.values[0] == expected);
}
