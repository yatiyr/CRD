// CEIR-4b — the §27 determinism + §28 numerical-semantics gate. Determinism is a per-op-KIND class carried on OpInfo
// (declared via the 2a schema, checked against the active CompilerMode); numerical semantics are a per-op-INSTANCE
// packed `numerics` int attribute. This proves: the schema→op_determinism round-trip, the full 6×4 mode-legality matrix,
// the find_determinism_violation module walk, that the compiler mode is NOT serialized (content purity), and the numerics
// pack/unpack + out-of-range rejection + attr survival through text AND binary. Host-only, ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/test_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir;
using crd::containers::ConstSpan;

TEST_CASE("ceir semantics: op_determinism round-trips the 2a schema; EMPTY!=UNKNOWN", "[ceir][semantics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    (void)test::register_test_ops(ctx);
    func::register_dialect(ctx);

    CHECK(ctx.op_determinism(ctx.intern_op("arith", "addi")) == DeterminismClass::BitExact);      // declared BitExact
    CHECK(ctx.op_determinism(ctx.intern_op("arith", "const")) == DeterminismClass::BitExact);
    CHECK(ctx.op_determinism(ctx.intern_op("test", "dummy")) == DeterminismClass::DeterministicWithinTarget);
    CHECK(ctx.op_determinism(ctx.intern_op("test", "kinds")) == DeterminismClass::Unspecified);    // Pure, undeclared
    CHECK(ctx.op_determinism(ctx.intern_op("func", "call")) == DeterminismClass::Unspecified);     // undeclared

    // ⛔ EMPTY!=UNKNOWN: an unregistered kind returns Unspecified but is UNKNOWN (op_info==nullptr) — a strict mode must
    // treat it as a violation, which it does because Unspecified fails Deterministic/Certified.
    const OpId unknown = ctx.intern_op("plugin", "widget");
    CHECK(ctx.op_determinism(unknown) == DeterminismClass::Unspecified);
    CHECK(ctx.op_info(unknown) == nullptr);
}

TEST_CASE("ceir semantics: determinism_satisfies_mode is the full 6x4 legality matrix", "[ceir][semantics]")
{
    const DeterminismClass all[6] = {DeterminismClass::Unspecified,           DeterminismClass::BitExact,
                                     DeterminismClass::DeterministicWithinTarget, DeterminismClass::DeterministicWithinBackend,
                                     DeterminismClass::Nondeterministic,      DeterminismClass::ExternalNondeterminism};
    for (const DeterminismClass c : all) // Normal + Fast constrain determinism not at all
    {
        CHECK(determinism_satisfies_mode(c, CompilerMode::Normal));
        CHECK(determinism_satisfies_mode(c, CompilerMode::Fast));
    }
    // Deterministic: only the three deterministic tiers
    CHECK_FALSE(determinism_satisfies_mode(DeterminismClass::Unspecified, CompilerMode::Deterministic));
    CHECK(determinism_satisfies_mode(DeterminismClass::BitExact, CompilerMode::Deterministic));
    CHECK(determinism_satisfies_mode(DeterminismClass::DeterministicWithinTarget, CompilerMode::Deterministic));
    CHECK(determinism_satisfies_mode(DeterminismClass::DeterministicWithinBackend, CompilerMode::Deterministic));
    CHECK_FALSE(determinism_satisfies_mode(DeterminismClass::Nondeterministic, CompilerMode::Deterministic));
    CHECK_FALSE(determinism_satisfies_mode(DeterminismClass::ExternalNondeterminism, CompilerMode::Deterministic));
    // CertifiedDeterministic: BitExact ONLY
    CHECK_FALSE(determinism_satisfies_mode(DeterminismClass::Unspecified, CompilerMode::CertifiedDeterministic));
    CHECK(determinism_satisfies_mode(DeterminismClass::BitExact, CompilerMode::CertifiedDeterministic));
    CHECK_FALSE(determinism_satisfies_mode(DeterminismClass::DeterministicWithinTarget, CompilerMode::CertifiedDeterministic));
    CHECK_FALSE(determinism_satisfies_mode(DeterminismClass::DeterministicWithinBackend, CompilerMode::CertifiedDeterministic));
    CHECK_FALSE(determinism_satisfies_mode(DeterminismClass::Nondeterministic, CompilerMode::CertifiedDeterministic));
    CHECK_FALSE(determinism_satisfies_mode(DeterminismClass::ExternalNondeterminism, CompilerMode::CertifiedDeterministic));
}

TEST_CASE("ceir semantics: find_mode_violation points at the first determinism offender under the active mode", "[ceir][semantics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    func::register_dialect(ctx);
    // a registered, BitExact region-holder — so the OUTER op passes and the walk recurses to find the inner offender
    // (an UNregistered container would itself be Unspecified and be flagged first — which is also correct, but not what
    // this test isolates: the RECURSION into a region).
    Dialect* const d2 = ctx.register_dialect("t2");
    const OpId     hold = d2->register_op("hold", {.determinism = DeterminismClass::BitExact});

    // %c = arith.const (BitExact) ; func.call (Unspecified) — inside a nested region to exercise the recursive walk.
    Module* const m   = ctx.create_module();
    Block* const  b0  = ctx.create_block(0U);
    m->body()->append(b0);
    Operation* const region_op = ctx.create_operation(hold, {}, 0U, {}, 1U);
    b0->append(region_op);
    Block* const inner = ctx.create_block(0U);
    region_op->region(0)->append(inner);
    inner->append(ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_i32()));
    Operation* const call = ctx.create_operation(ctx.intern_op("func", "call"), {}, 0U);
    inner->append(call);

    CHECK(ctx.compiler_mode() == CompilerMode::Normal);      // the default
    CHECK(ctx.find_mode_violation(*m) == nullptr);           // Normal constrains nothing

    ctx.set_compiler_mode(CompilerMode::CertifiedDeterministic);
    CHECK(ctx.find_mode_violation(*m) == call);              // const is BitExact (ok); the Unspecified call is flagged

    ctx.set_compiler_mode(CompilerMode::Deterministic);
    CHECK(ctx.find_mode_violation(*m) == call);              // still the call (Unspecified fails Deterministic too)
}

TEST_CASE("ceir semantics: find_mode_violation also enforces the numerics contract (sec 28)", "[ceir][semantics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);

    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    // a BitExact op whose INSTANCE sets fast_math=On — determinism class is fine, but the numerics knob breaks Certified.
    Operation* const hot = ctx.create_operation(ctx.intern_op("arith", "addi"), {}, 1U, ctx.type_i32());
    b->append(hot);
    NumericalSemantics ns;
    ns.fast_math = Toggle::On;
    ctx.set_numerics(hot, ns);

    ctx.set_compiler_mode(CompilerMode::Normal);
    CHECK(ctx.find_mode_violation(*m) == nullptr);           // Normal admits fast-math
    ctx.set_compiler_mode(CompilerMode::CertifiedDeterministic);
    CHECK(ctx.find_mode_violation(*m) == hot);               // fast_math=On violates Certified even on a BitExact kind
    ctx.set_compiler_mode(CompilerMode::Deterministic);
    CHECK(ctx.find_mode_violation(*m) == hot);

    // a CORRUPT numerics attr (hand-planted out-of-range int) is a violation in EVERY mode, Normal included.
    ctx.set_numerics(hot, NumericalSemantics{}); // clear hot's fast-math so, in Normal, it is not itself a candidate
    Operation* const bad = ctx.create_operation(ctx.intern_op("arith", "muli"), {}, 1U, ctx.type_i32());
    b->append(bad);
    ctx.set_attr(bad, "numerics", ctx.attr_int(static_cast<crd::i64>(0x7ULL << 20U))); // rounding nibble = 7 > max 4
    ctx.set_compiler_mode(CompilerMode::Normal);
    CHECK(ctx.find_mode_violation(*m) == bad); // hot is now clean+legal in Normal; the corrupt attr on `bad` is flagged
}

TEST_CASE("ceir semantics: the compiler mode is session state, never serialized (content purity)", "[ceir][semantics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    b->append(ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_i32()));

    const crd::containers::Array<crd::u8> blob1 = serialize(ctx, *m, &root);
    ctx.set_compiler_mode(CompilerMode::CertifiedDeterministic); // changing the mode must NOT change module content
    const crd::containers::Array<crd::u8> blob2 = serialize(ctx, *m, &root);
    REQUIRE(blob1.size() == blob2.size());
    CHECK(std::memcmp(blob1.data(), blob2.data(), blob1.size()) == 0);
}

TEST_CASE("ceir semantics: numerics pack/unpack round-trips every field; rejects out-of-range", "[ceir][semantics]")
{
    NumericalSemantics n;
    n.ieee              = IeeeMode::Strict;
    n.fast_math         = Toggle::Off;
    n.fma               = Toggle::On;
    n.flush_to_zero     = Toggle::On;
    n.denorm            = DenormMode::Flush;
    n.rounding          = RoundingMode::TowardNegative; // the max rounding enumerator (4)
    n.overflow          = OverflowMode::Trap;           // the max overflow enumerator (3)
    n.int_wrap          = IntWrapMode::Trap;
    n.nan               = NanMode::AssumeNoNaN;          // the max nan enumerator (3)
    n.precision_promote = Toggle::On;
    n.mixed_precision   = Toggle::On;
    n.stochastic_round  = Toggle::On;

    NumericalSemantics back;
    REQUIRE(unpack_numerics(pack_numerics(n), back));
    CHECK(back == n);

    NumericalSemantics def; // all-Inherit packs to 0 and round-trips
    CHECK(pack_numerics(def) == 0);
    NumericalSemantics zero;
    REQUIRE(unpack_numerics(0, zero));
    CHECK(zero == def);

    // ⛔ the decoder arm: a nibble past its field's value count, or any bit at/above 48, is rejected (never a garbage enum)
    NumericalSemantics junk;
    CHECK_FALSE(unpack_numerics(static_cast<crd::i64>(0x7ULL << 20U), junk)); // field 5 (rounding) = 7 > max 4
    CHECK_FALSE(unpack_numerics(static_cast<crd::i64>(1ULL << 48U), junk));   // a bit above the defined 48
}

TEST_CASE("ceir semantics: numerics ride a per-op attr and survive text + binary round-trips", "[ceir][semantics]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);

    NumericalSemantics ns;
    ns.ieee      = IeeeMode::Relaxed;
    ns.fast_math = Toggle::On;
    ns.rounding  = RoundingMode::TowardZero;

    Module* const m  = ctx.create_module();
    Block* const  b  = ctx.create_block(0U);
    m->body()->append(b);
    Operation* const op = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_i32());
    b->append(op);
    ctx.set_numerics(op, ns);

    NumericalSemantics got;
    REQUIRE(ctx.op_numerics(*op, got));
    CHECK(got == ns);

    // an op with NO numerics attr reads back as all-Inherit (absent ⇒ default), still "valid".
    Operation* const bare = ctx.create_operation(ctx.intern_op("arith", "addi"), {}, 1U, ctx.type_i32());
    b->append(bare);
    NumericalSemantics bare_ns;
    REQUIRE(ctx.op_numerics(*bare, bare_ns));
    CHECK(bare_ns == NumericalSemantics{});

    // survives TEXT round-trip (the numerics attr is a plain int attribute)
    const crd::containers::String t1 = print(ctx, *m, &root);
    Context                       ctx2(&root);
    (void)arith::register_arith_ops(ctx2);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    // survives BINARY round-trip
    const crd::containers::Array<crd::u8> blob = serialize(ctx, *m, &root);
    Context                               ctx3(&root);
    (void)arith::register_arith_ops(ctx3);
    const ParseResult dr = deserialize(ctx3, ConstSpan<crd::u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    // the first op of the deserialized module carries the numerics attr, unpacking to the same value
    Operation* const rt = dr.module->body()->first_block()->first_op();
    NumericalSemantics rt_ns;
    REQUIRE(ctx3.op_numerics(*rt, rt_ns));
    CHECK(rt_ns == ns);
}

TEST_CASE("ceir semantics: numerics_satisfies_mode honors the fmad scar (Fast admits FMA/fast-math)", "[ceir][semantics]")
{
    NumericalSemantics fast; // fast-math + FMA on, relaxed IEEE
    fast.fast_math = Toggle::On;
    fast.fma       = Toggle::On;
    fast.ieee      = IeeeMode::Relaxed;
    CHECK(numerics_satisfies_mode(fast, CompilerMode::Normal));           // Normal forbids nothing
    CHECK(numerics_satisfies_mode(fast, CompilerMode::Fast));             // ⛔ Fast MUST admit FMA/fast-math (the GEMM scar)
    CHECK_FALSE(numerics_satisfies_mode(fast, CompilerMode::Deterministic));       // fast-math breaks reproducibility
    CHECK_FALSE(numerics_satisfies_mode(fast, CompilerMode::CertifiedDeterministic));

    NumericalSemantics fma_only; // FMA on, but strict IEEE + no fast-math — reproducible, so Certified accepts it
    fma_only.fma = Toggle::On;
    CHECK(numerics_satisfies_mode(fma_only, CompilerMode::CertifiedDeterministic)); // FMA is deterministic when consistent
    CHECK(numerics_satisfies_mode(fma_only, CompilerMode::Deterministic));
}
