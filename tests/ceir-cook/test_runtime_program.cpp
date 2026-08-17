// CEIR-7b - the CEIR RuntimeProgram + generation-safe handle. load_program reads a cooked blob and VALIDATES the
// header's declared hashes against the payload (the declared-header-words-validated scar); RuntimeSlot/RuntimeHandle
// (crd-render-asset-core, RAF-11 - CEIR is its FIRST consumer) give generation-tagged staleness detection. The tests:
// the load happy path, generation safety (stale after a same-id re-install; a wrong-id handle is not current), TWO
// generations in TWO Contexts coexisting (7c's inbound pattern), the SPLICE forge (P1 header + P2 program -> content
// mismatch), and an unregistered-dialect load (a typed error, not a hash mismatch). ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/runtime_program.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/resources/crdr.hpp> // the splice forge re-packages chunks directly
#include <crd/resources/resource_id.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::i64;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::renderasset::AssetId;

namespace
{
struct Reg
{
    OpId cst, muli;
    explicit Reg(Context& c) : cst(c.intern_op("arith", "const")), muli(c.intern_op("arith", "muli"))
    {
        (void)arith::register_arith_ops(c);
        (void)core::register_core_ops(c);
        (void)func::register_dialect(c);
    }
};
Block* module_block(Context& c, Module& m)
{
    Block* b = m.body()->first_block();
    if (b == nullptr)
    {
        b = c.create_block(0U);
        m.body()->append(b);
    }
    return b;
}
Operation* konst(Context& c, const Reg& r, Block* b, i64 v)
{
    Operation* const op = c.create_operation(r.cst, {}, 1U, c.type_i32());
    c.set_attr(op, "value", c.attr_int(v));
    b->append(op);
    return op;
}
// @f() -> i32 { return K }  (K distinguishes generations by content).
Module* build_const_fn(Context& c, const Reg& r, i64 k)
{
    Module* const m = c.create_module();
    (void)module_block(c, *m);
    Operation* const f = func::create_func(c, *m, "f", Visibility::Public, 0U, c.type_i32());
    module_block(c, *m)->append(f);
    Block* const b = func::func_body_block(f);
    Value* rv[1] = {konst(c, r, b, k)->result(0U)};
    b->append(func::create_return(c, ConstSpan<Value*>(rv, 1U)));
    return m;
}
} // namespace

TEST_CASE("ceir runtime 7b: a cooked program loads (hash-validated) and installs a current handle", "[ceir][runtime]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    const CookResult cooked = cook_program(a, *build_const_fn(a, ra, 5), 0x42U, &root, &root);
    REQUIRE(cooked.ok());

    Context   b(&root);
    const Reg rb(b); // the caller registers the dialects before load (the hash recompute needs them)
    LoadResult loaded = load_program(b, ConstSpan<u8>(cooked.blob.data(), cooked.blob.size()), &root, &root);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.program.valid());
    CHECK(loaded.program.content_hash == cooked.content_hash);
    CHECK(loaded.program.interface_hash == cooked.interface_hash);

    ProgramSlot         slot;
    const ProgramHandle h = slot.install(&loaded.program, AssetId{42U});
    CHECK(slot.is_current(h));
    CHECK(h.valid());
    CHECK(slot.current() == &loaded.program);
}

TEST_CASE("ceir runtime 7b: a handle minted before a same-id re-install is detectably stale", "[ceir][runtime]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    const CookResult c1 = cook_program(a, *build_const_fn(a, ra, 5), 1U, &root, &root);
    const CookResult c2 = cook_program(a, *build_const_fn(a, ra, 6), 1U, &root, &root);
    REQUIRE(c1.ok());
    REQUIRE(c2.ok());
    Context   b(&root);
    const Reg rb(b);
    LoadResult l1 = load_program(b, ConstSpan<u8>(c1.blob.data(), c1.blob.size()), &root, &root);
    LoadResult l2 = load_program(b, ConstSpan<u8>(c2.blob.data(), c2.blob.size()), &root, &root);
    REQUIRE(l1.ok());
    REQUIRE(l2.ok());

    ProgramSlot         slot;
    const ProgramHandle h1 = slot.install(&l1.program, AssetId{7U});
    CHECK(slot.is_current(h1));
    const ProgramHandle h2 = slot.install(&l2.program, AssetId{7U}); // hot-swap: same id, new generation
    CHECK_FALSE(slot.is_current(h1)); // the OLD handle is now stale (the RAF-11 property)
    CHECK(slot.is_current(h2));

    // a handle whose id does not match the slot is never current (is_current checks BOTH id and generation).
    ProgramHandle wrong = h2;
    wrong.id           = AssetId{999U};
    CHECK_FALSE(slot.is_current(wrong));
}

TEST_CASE("ceir runtime 7b: two generations loaded in two Contexts coexist independently", "[ceir][runtime]")
{
    crd::memory::GrowableTlsfAllocator root;
    // COOK two distinct generations.
    Context   ck(&root);
    const Reg rck(ck);
    const CookResult c1 = cook_program(ck, *build_const_fn(ck, rck, 5), 1U, &root, &root);
    const CookResult c2 = cook_program(ck, *build_const_fn(ck, rck, 6), 1U, &root, &root);
    REQUIRE(c1.ok());
    REQUIRE(c2.ok());

    // ⛔ 7c's inbound pattern: each generation in ITS OWN Context (frees wholesale on retire).
    Context   ctx_a(&root);
    const Reg ra(ctx_a);
    Context   ctx_b(&root);
    const Reg rb(ctx_b);
    LoadResult l1 = load_program(ctx_a, ConstSpan<u8>(c1.blob.data(), c1.blob.size()), &root, &root);
    LoadResult l2 = load_program(ctx_b, ConstSpan<u8>(c2.blob.data(), c2.blob.size()), &root, &root);
    REQUIRE(l1.ok());
    REQUIRE(l2.ok());

    ProgramSlot         slot;
    const ProgramHandle h1 = slot.install(&l1.program, AssetId{9U});
    const ProgramHandle h2 = slot.install(&l2.program, AssetId{9U});
    CHECK_FALSE(slot.is_current(h1));
    CHECK(slot.is_current(h2));
    // BOTH generations remain alive + readable (ctx_a and ctx_b both live) with DISTINCT content.
    REQUIRE(h1.ptr != nullptr);
    REQUIRE(h2.ptr != nullptr);
    CHECK(h1.ptr->content_hash != h2.ptr->content_hash);
    CHECK(h1.ptr->module != nullptr);
    CHECK(h2.ptr->module != nullptr);
}

TEST_CASE("ceir runtime 7b: a SPLICED blob (one header, another program) is a ContentHashMismatch", "[ceir][runtime]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    const CookResult p1 = cook_program(a, *build_const_fn(a, ra, 5), 1U, &root, &root);
    const CookResult p2 = cook_program(a, *build_const_fn(a, ra, 6), 2U, &root, &root); // a DIFFERENT program
    REQUIRE(p1.ok());
    REQUIRE(p2.ok());

    // SPLICE: P1's 'META' header (declaring P1's hashes) over P2's 'CEIR' program + 'CDEP' deps.
    crd::resources::CrdrFile f1(&root);
    crd::resources::CrdrFile f2(&root);
    REQUIRE(crd::resources::crdr_read(ConstSpan<u8>(p1.blob.data(), p1.blob.size()), f1, &root)
            == crd::resources::CrdrError::Ok);
    REQUIRE(crd::resources::crdr_read(ConstSpan<u8>(p2.blob.data(), p2.blob.size()), f2, &root)
            == crd::resources::CrdrError::Ok);
    const crd::resources::CrdrChunk* const meta1 = crd::resources::crdr_find_chunk(f1, crd::resources::kFourCC_META);
    const crd::resources::CrdrChunk* const prog2 = crd::resources::crdr_find_chunk(f2, crd::resources::kFourCC_CEIR);
    const crd::resources::CrdrChunk* const dep2  = crd::resources::crdr_find_chunk(f2, crd::resources::kFourCC_CDEP);
    REQUIRE(meta1 != nullptr);
    REQUIRE(prog2 != nullptr);
    REQUIRE(dep2 != nullptr);
    crd::resources::CrdrWriter w(&root, crd::resources::ResourceId{0U, 1U}, crd::resources::kFourCC_CEIR);
    w.add_chunk(crd::resources::kFourCC_META, meta1->payload); // P1's declared hashes...
    w.add_chunk(crd::resources::kFourCC_CEIR, prog2->payload); // ...over P2's program
    w.add_chunk(crd::resources::kFourCC_CDEP, dep2->payload);
    const crd::containers::Array<u8> spliced = w.finish();

    Context   b(&root);
    const Reg rb(b);
    const LoadResult loaded = load_program(b, ConstSpan<u8>(spliced.data(), spliced.size()), &root, &root);
    CHECK_FALSE(loaded.ok());
    CHECK(loaded.error == LoadError::ContentHashMismatch); // the header words are validated against the payload
}

TEST_CASE("ceir runtime 7b: loading into a Context without the dialects registered is UnregisteredOp", "[ceir][runtime]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    const CookResult cooked = cook_program(a, *build_const_fn(a, ra, 5), 1U, &root, &root);
    REQUIRE(cooked.ok());

    Context b(&root); // ⛔ NO Reg -> the dialects are NOT registered in b
    const LoadResult loaded = load_program(b, ConstSpan<u8>(cooked.blob.data(), cooked.blob.size()), &root, &root);
    CHECK_FALSE(loaded.ok());
    CHECK(loaded.error == LoadError::UnregisteredOp); // a typed caller-must-register error, NOT a hash mismatch
}

TEST_CASE("ceir runtime 7b: a corrupted interface-hash header word (content intact) is InterfaceHashMismatch", "[ceir][runtime]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    const CookResult cooked = cook_program(a, *build_const_fn(a, ra, 5), 1U, &root, &root);
    REQUIRE(cooked.ok());

    // Read the container, COPY the 'META' CookedHeader payload, and flip a byte in the INTERFACE-HASH field. Layout is
    // fixed: magic(4) + type(4) + schema(4) = 12, then iface(8) at [12..19], content(8) at [20..27] -- so byte 12 is the
    // declared interface hash and leaves magic/type/schema (read_cooked_header's checks) AND content untouched. This test
    // also PINS that header-offset assumption: if the layout ever moves, it fails loudly.
    crd::resources::CrdrFile f(&root);
    REQUIRE(crd::resources::crdr_read(ConstSpan<u8>(cooked.blob.data(), cooked.blob.size()), f, &root)
            == crd::resources::CrdrError::Ok);
    const crd::resources::CrdrChunk* const meta = crd::resources::crdr_find_chunk(f, crd::resources::kFourCC_META);
    const crd::resources::CrdrChunk* const prog = crd::resources::crdr_find_chunk(f, crd::resources::kFourCC_CEIR);
    const crd::resources::CrdrChunk* const dep  = crd::resources::crdr_find_chunk(f, crd::resources::kFourCC_CDEP);
    REQUIRE(meta != nullptr);
    REQUIRE(prog != nullptr);
    REQUIRE(dep != nullptr);
    crd::containers::Array<u8> corrupt(&root);
    for (usize i = 0; i < meta->payload.size(); ++i) { corrupt.push_back(meta->payload[i]); }
    REQUIRE(corrupt.size() > 19U);
    corrupt[12] = static_cast<u8>(corrupt[12] ^ 0xFFU); // flip a bit in the DECLARED interface hash

    crd::resources::CrdrWriter w(&root, crd::resources::ResourceId{0U, 1U}, crd::resources::kFourCC_CEIR);
    w.add_chunk(crd::resources::kFourCC_META, ConstSpan<u8>(corrupt.data(), corrupt.size()));
    w.add_chunk(crd::resources::kFourCC_CEIR, prog->payload);
    w.add_chunk(crd::resources::kFourCC_CDEP, dep->payload);
    const crd::containers::Array<u8> forged = w.finish();

    Context   b(&root);
    const Reg rb(b);
    const LoadResult loaded = load_program(b, ConstSpan<u8>(forged.data(), forged.size()), &root, &root);
    CHECK_FALSE(loaded.ok());
    CHECK(loaded.error == LoadError::InterfaceHashMismatch); // content recomputes equal; the declared interface word does not
}
