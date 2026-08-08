// CEIR-1h - the MALFORMED-input rejection corpus (permanent, per §119/§167). Both loaders must REJECT bad input via
// the ParseResult{ok=false, error_offset} contract and NEVER crash - ASan/UBSan is the proof of memory-safety. The
// corpus includes the two "killer" cases the 1h fuzz slice surfaced (a huge textual def-id, a corrupt binary count),
// which used to OOM before the loaders were hardened. A systematic single-byte-corruption SWEEP over a valid text and
// a valid blob then converts the hand table into class coverage. Host-only. ASCII-only test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include "rich_graph.hpp" // crd::ceir::test::build_rich

using namespace crd::ceir;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::u32;
using crd::u8;
using crd::usize;

namespace
{
[[nodiscard]] ConstSpan<u8> span(const Array<u8>& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
} // namespace

TEST_CASE("ceir malformed: bad TEXT is rejected, never a crash", "[ceir][malformed]")
{
    crd::memory::MallocAllocator root;
    auto rejects = [&root](const char* src) {
        Context    ctx(&root);
        const auto pr = parse(ctx, StringView(src));
        return !pr.ok && pr.module == nullptr;
    };

    CHECK(rejects(""));                                       // empty
    CHECK(rejects("garbage"));                                // not 'module'
    CHECK(rejects("module"));                                 // no body
    CHECK(rejects("module {"));                               // unterminated region
    CHECK(rejects("module { ^bb0:"));                         // unterminated block
    CHECK(rejects("module { ^bb0: test.x(%9) }"));            // dangling operand
    CHECK(rejects("module { ^bb0: test.x( }"));               // truncated operands
    CHECK(rejects("module { ^bb0: nodialect() }"));           // op name is not 'dialect.op'
    CHECK(rejects("module { ^bb0: %0 = a.b() : !t1\n%0 = c.d() : !t1 }")); // duplicate SSA id
    CHECK(rejects(R"(module { ^bb0: t.x() {s = "unterminated } })"));      // unterminated string literal
    CHECK(rejects("module { ^bb0: } trailing"));             // trailing junk after the module
    // KILLER (used to OOM): a def id far larger than the text -> must reject, not allocate a multi-GB array
    CHECK(rejects("module { ^bb0: %4000000000 = t.x() : !t1 }"));
    CHECK(rejects("module { ^bb0(%4000000000 : !t1): }"));   // ...same, as a block-arg def
}

TEST_CASE("ceir malformed: bad BINARY is rejected, never a crash", "[ceir][malformed]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Array<u8>              good = serialize(ctx, *test::build_rich(ctx), &root);

    auto rejects = [&root](const Array<u8>& bytes) {
        Context    c(&root);
        const auto pr = deserialize(c, span(bytes));
        return !pr.ok && pr.module == nullptr;
    };
    auto mutated = [&root, &good](usize truncate_to, int at, u8 xor_val) {
        Array<u8> b(&root);
        const usize n = truncate_to == 0U ? good.size() : truncate_to;
        for (usize i = 0; i < n && i < good.size(); ++i) { b.push_back(good[i]); }
        if (at >= 0 && static_cast<usize>(at) < b.size()) { b[static_cast<usize>(at)] ^= xor_val; }
        return b;
    };

    CHECK(rejects(Array<u8>(&root)));                 // empty
    CHECK(rejects(mutated(0U, 0, 0x01U)));            // bad magic (byte 0)
    CHECK(rejects(mutated(0U, 4, 0xFFU)));            // bad version (byte 4)
    CHECK(rejects(mutated(6U, -1, 0U)));              // truncated header (6 bytes)
    CHECK(rejects(mutated(good.size() / 2U, -1, 0U))); // truncated mid-body

    // trailing junk after the last chunk
    {
        Array<u8> b(&root);
        for (usize i = 0; i < good.size(); ++i) { b.push_back(good[i]); }
        b.push_back(0x7FU);
        CHECK(rejects(b));
    }
    // KILLER (used to OOM): inflate a u32 near the tail to a huge value. The tail of the BODY chunk holds the LAST
    // op's counts (num_operands / num_results / num_regions / num_attrs); a corrupt count there used to drive a
    // multi-GB allocation. The systematic byte-flip sweep (next test) covers the counts near the BODY start (block /
    // region counts); this loop hammers the tail. The hardened decoder must bound every count and reject, never alloc.
    {
        for (usize off = good.size() > 40U ? good.size() - 40U : 0U; off + 4U <= good.size(); off += 1U)
        {
            Array<u8> b(&root);
            for (usize i = 0; i < good.size(); ++i) { b.push_back(good[i]); }
            b[off]      = 0xFFU; // inflate a byte to make some nearby count enormous
            b[off + 1U] = 0xFFU;
            b[off + 2U] = 0xFFU;
            b[off + 3U] = 0xFFU;
            Context    c(&root);
            const auto pr = deserialize(c, span(b)); // must not crash / OOM (assert-free; ASan is the proof)
            (void)pr;
        }
        CHECK(true); // reached here = no crash across the whole tail sweep
    }
}

TEST_CASE("ceir malformed: a single-byte corruption of any position never crashes a loader", "[ceir][malformed]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module&                      m = *test::build_rich(ctx);

    const String    text = print(ctx, m, &root);
    const Array<u8> blob = serialize(ctx, m, &root);

    usize done = 0;
    // flip every byte of the text and parse - some corruptions still parse validly; the ONLY invariant is no crash
    for (usize i = 0; i < text.size(); ++i)
    {
        String mut(text.data(), text.size(), &root);
        mut.data()[i] = static_cast<char>(static_cast<u8>(mut.data()[i]) ^ 0xFFU);
        Context    c(&root);
        const auto pr = parse(c, StringView(mut.data(), mut.size()));
        (void)pr;
        ++done;
    }
    // flip every byte of the blob and deserialize
    for (usize i = 0; i < blob.size(); ++i)
    {
        Array<u8> mut(blob, &root);
        mut[i] ^= 0xFFU;
        Context    c(&root);
        const auto pr = deserialize(c, span(mut));
        (void)pr;
        ++done;
    }
    CHECK(done == text.size() + blob.size()); // every position swept without a crash
}
