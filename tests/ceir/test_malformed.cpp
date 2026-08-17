// CEIR-1h - the MALFORMED-input rejection corpus (permanent, per §119/§167). Both loaders must REJECT bad input via
// the ParseResult{ok=false, error_offset} contract and NEVER crash - ASan/UBSan is the proof of memory-safety. The
// corpus includes the two "killer" cases the 1h fuzz slice surfaced (a huge textual def-id, a corrupt binary count),
// which used to OOM before the loaders were hardened. A systematic single-byte-corruption SWEEP over a valid text and
// a valid blob then converts the hand table into class coverage. Host-only. ASCII-only test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

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
    crd::memory::GrowableTlsfAllocator root;
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
    CHECK(rejects("module { ^bb0: %0 = a.b() : !i32\n%0 = c.d() : !i32 }")); // duplicate SSA id
    CHECK(rejects(R"(module { ^bb0: t.x() {s = "unterminated } })"));      // unterminated string literal
    CHECK(rejects("module { ^bb0: } trailing"));             // trailing junk after the module
    // KILLER (used to OOM): a def id far larger than the text -> must reject, not allocate a multi-GB array
    CHECK(rejects("module { ^bb0: %4000000000 = t.x() : !i32 }"));
    CHECK(rejects("module { ^bb0(%4000000000 : !i32): }"));   // ...same, as a block-arg def

    // CEIR-3a type syntax (section 16): every malformed type is rejected, never a crash.
    CHECK(rejects("module { ^bb0: t.x() : !nope }"));            // unknown type keyword
    CHECK(rejects("module { ^bb0: t.x() : !i }"));               // integer with no width -> unknown keyword
    CHECK(rejects("module { ^bb0: t.x() : !vec<4> }"));          // vector missing 'x element'
    CHECK(rejects("module { ^bb0: t.x() : !vec<4x!f32 }"));      // unterminated aggregate ('>' missing)
    CHECK(rejects("module { ^bb0: t.x() : !mat<4x!f32> }"));     // matrix missing the second dim
    CHECK(rejects("module { ^bb0: t.x() : !struct<> }"));        // struct with no name
    CHECK(rejects("module { ^bb0: t.x() : !result<!i32> }"));    // result needs two arms
    // CEIR-3b generics syntax
    CHECK(rejects("module { ^bb0: t.x() : !param<> }"));         // type param needs a name
    CHECK(rejects("module { ^bb0: t.x() : !trait<> }"));         // trait needs a name
    CHECK(rejects("module { ^bb0: t.x() : !fn<!i32> }"));        // callable needs (params)->(results)
    CHECK(rejects("module { ^bb0: t.x() : !fn<(!i32)-> }"));     // unterminated callable (no result list)
    CHECK(rejects("module { ^bb0: t.x() : !fn<(!i32)(!i32)> }")); // callable missing the arrow
    // CEIR-3c resource/view syntax
    CHECK(rejects("module { ^bb0: t.x() : !buffer<> }"));         // buffer needs a mode
    CHECK(rejects("module { ^bb0: t.x() : !buffer<bogus> }"));    // unknown buffer mode
    CHECK(rejects("module { ^bb0: t.x() : !buffer<plain> }"));    // plain buffer needs an element
    CHECK(rejects("module { ^bb0: t.x() : !image<d9,!f32> }"));   // unknown image dim
    CHECK(rejects("module { ^bb0: t.x() : !sampler<hmm> }"));     // unknown sampler kind
    CHECK(rejects("module { ^bb0: t.x() : !view<!buffer<plain,!f32>,bogus> }")); // unknown view range
    CHECK(rejects("module { ^bb0: t.x() : !view<!buffer<plain,!f32>,mip> }"));   // mip range on a buffer (combination)
    CHECK(rejects("module { ^bb0: t.x() : !view<!i32,byte> }"));                 // view of a non-resource
    // CEIR-3d shape/tensor syntax
    CHECK(rejects("module { ^bb0: t.x() : !dim<> }"));                    // dim needs an extent / name / dyn
    CHECK(rejects("module { ^bb0: t.x() : !shape<!i32> }"));             // a shape member must be a dim
    CHECK(rejects("module { ^bb0: t.x() : !tensor<!f32,!i32> }"));       // a tensor's shape arg must be a Shape
    CHECK(rejects("module { ^bb0: t.x() : !tensor<!shape<>,!shape<>> }")); // a tensor element must not be a Shape
    CHECK(rejects("module { ^bb0: t.x() : !tensor<!f32> }"));            // tensor needs element AND shape
    // CEIR-3e quantity syntax
    CHECK(rejects("module { ^bb0: t.x() : !qty<!f32,> }"));      // empty dimension (dimensionless must be '1')
    CHECK(rejects("module { ^bb0: t.x() : !qty<!f32,L> }"));     // missing exponent after a base
    CHECK(rejects("module { ^bb0: t.x() : !qty<!f32,X1> }"));    // unknown base letter
    CHECK(rejects("module { ^bb0: t.x() : !qty<!f32,L1L1> }"));  // duplicate base
    CHECK(rejects("module { ^bb0: t.x() : !qty<!f32,T1L1> }"));  // bases out of canonical order
    CHECK(rejects("module { ^bb0: t.x() : !qty<!shape<>,L1> }")); // a non-numeric underlying
    // KILLER: pathological type nesting must be depth-rejected, not blow the recursive-descent stack
    {
        String deep(&root);
        deep.append("module { ^bb0: t.x() : ");
        for (u32 i = 0; i < 200U; ++i) { deep.append("!vec<1x"); }
        deep.append("!i32");
        for (u32 i = 0; i < 200U; ++i) { deep.push_back('>'); }
        deep.append(" }");
        Context    ctx(&root);
        const auto pr = parse(ctx, StringView(deep.data(), deep.size()));
        CHECK_FALSE(pr.ok); // rejected by the nesting-depth guard, no stack overflow
    }
}

TEST_CASE("ceir malformed: bad BINARY is rejected, never a crash", "[ceir][malformed]")
{
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
