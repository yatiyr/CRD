// CEIR-5z BAND-5 GATE (sec 118): a pinned NONTRIVIAL program -- a bounded core.for loop, a core.match selecting the
// accumulation per iteration, a value-producing core.if, ceir.func calls, and a sec 20 state<T> accumulator that persists
// ACROSS calls -- executes in the reference executor to a byte-pinned result, IDENTICAL builder-built and text-parsed.
// This is the whole band: the executor + the round-trip + state/control-flow semantics, in one assertion. ASCII names.
//
// ⛔ The program is defined ONCE, in the SHARED corpus builder (corpus.hpp `build_5z`), and REUSED here — the CEIR-11b
// differential (test_plan.cpp) runs the SAME program through the compiled plan. No copy-pasted IR that could drift.
//
//   @main(6) with half=3 -> acc total = sum(iv<3 ? 2*iv : iv) = (0+2+4)+(3+4+5) = 18 ; ok=1 -> bonus=100 -> 18+100 = 118.
//   The in-loop cell (build_5z's cells[0]) = sum iv = 15.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

#include "corpus.hpp" // the SHARED corpus builder (build_5z) — defined ONCE, reused by the CEIR-11b differential

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;

TEST_CASE("ceir band5 gate: a pinned program executes identically builder-built and text-parsed", "[ceir][gate5]")
{
    crd::memory::GrowableTlsfAllocator root;

    // ---- BUILDER form (from the shared corpus builder) ----
    Context           ctx_a(&root);
    const corpus::Kit o_a(ctx_a);
    const corpus::Built a = corpus::build_5z(ctx_a, o_a);
    REQUIRE(ctx_a.find_structure_error(*a.m).kind == StructureErrorKind::None); // the program is structurally sound

    exec::Interpreter in_a(ctx_a);
    exec::install_builtin_semantics(in_a);
    i64                    n6[1] = {6};
    const exec::ExecResult r_a = in_a.invoke(*a.m, "main", ConstSpan<i64>(n6, 1U));
    REQUIRE(r_a.ok());
    REQUIRE(r_a.values.size() == 1U);
    CHECK(r_a.values[0] == 118); // acc total (0+2+4)+(3+4+5)=18; ok -> bonus 100; 18+100
    REQUIRE(a.cells.size() >= 1U);
    i64 cell2v = -1;
    REQUIRE(in_a.cell_value(a.cells[0], cell2v)); // the in-loop cell, builder form only (pointers don't survive round-trip)
    CHECK(cell2v == 15);                          // sum 0..5

    // ---- TEXT-PARSED form (dialects registered in the parse context -- the CEIR-5d trait-is-registry-state finding) ----
    const String text = print(ctx_a, *a.m, &root);
    Context      ctx_b(&root);
    const corpus::Kit o_b(ctx_b);
    (void)o_b;
    const ParseResult pr = parse(ctx_b, text);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    REQUIRE(ctx_b.find_structure_error(*pr.module).kind == StructureErrorKind::None);

    exec::Interpreter in_b(ctx_b);
    exec::install_builtin_semantics(in_b);
    const exec::ExecResult r_b = in_b.invoke(*pr.module, "main", ConstSpan<i64>(n6, 1U));
    REQUIRE(r_b.ok());
    REQUIRE(r_b.values.size() == 1U);
    CHECK(r_b.values[0] == 118);

    // ---- BYTE-IDENTICAL output (the sec 118 byte-pin) ----
    const containers::Array<u8> pin_a = exec::pin_values(ConstSpan<i64>(r_a.values.data(), r_a.values.size()), &root);
    const containers::Array<u8> pin_b = exec::pin_values(ConstSpan<i64>(r_b.values.data(), r_b.values.size()), &root);
    REQUIRE(pin_a.size() == pin_b.size());
    REQUIRE(pin_a.size() == 8U); // one i64
    CHECK(std::memcmp(pin_a.data(), pin_b.data(), pin_a.size()) == 0);

    // ---- and the whole program survives a BINARY round-trip too ----
    const containers::Array<u8> blob = serialize(ctx_a, *a.m, &root);
    Context                     ctx_c(&root);
    const corpus::Kit           o_c(ctx_c);
    (void)o_c;
    const ParseResult dr = deserialize(ctx_c, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    exec::Interpreter in_c(ctx_c);
    exec::install_builtin_semantics(in_c);
    const exec::ExecResult r_c = in_c.invoke(*dr.module, "main", ConstSpan<i64>(n6, 1U));
    REQUIRE(r_c.ok());
    CHECK(r_c.values[0] == 118);
}
