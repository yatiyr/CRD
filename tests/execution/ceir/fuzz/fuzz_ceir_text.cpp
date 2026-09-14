// crd-fuzz-ceir-text: the CEIR textual loader (crd::ceir::parse) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md). Oracle: a rejected input leaves no module; an accepted input prints, re-parses
// and prints byte-identically, the CEIR-1e round-trip contract the unit tests hold. Seeds: the rich graph the
// round-trip tests use plus the hand malformed table of test_malformed.cpp, so the corpus starts on both sides.
#include <crd/fuzz/harness.hpp>

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <cstdio>

#include "../rich_graph.hpp" // crd::ceir::test::build_rich

namespace
{
using crd::containers::String;
using crd::containers::StringView;
using crd::u8;

[[nodiscard]] const u8* bytes_of(const String& text) noexcept
{
    return reinterpret_cast<const u8*>(text.data());
}

const char* const kMalformed[] = {
    "",
    "garbage",
    "module",
    "module {",
    "module { ^bb0:",
    "module { ^bb0: test.x(%9) }",
    "module { ^bb0: test.x( }",
    "module { ^bb0: nodialect() }",
    "module { ^bb0: %0 = a.b() : !i32\n%0 = c.d() : !i32 }",
    "module { ^bb0: t.x() {s = \"unterminated } }",
    "module { ^bb0: } trailing",
    "module { ^bb0: %4000000000 = t.x() : !i32 }",
    "module { ^bb0(%4000000000 : !i32): }",
    "module { ^bb0: t.x() : !nope }",
    "module { ^bb0: t.x() : !vec<4x!f32 }",
    "module { ^bb0: t.x() : !fn<(!i32)-> }",
    "module { ^bb0: t.x() : !buffer<bogus> }",
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    crd::fuzz::BudgetAllocator root;
    crd::ceir::Context         ctx(&root);
    const crd::ceir::ParseResult first = crd::ceir::parse(ctx, StringView(reinterpret_cast<const char*>(data), size));
    if (!first.ok)
    {
        CRD_FUZZ_REQUIRE(first.module == nullptr);
        return 0;
    }
    CRD_FUZZ_REQUIRE(first.module != nullptr);
    const String printed = crd::ceir::print(ctx, *first.module, &root);

    crd::ceir::Context           again(&root);
    const crd::ceir::ParseResult second = crd::ceir::parse(again, StringView(printed.data(), printed.size()));
    if (!second.ok)
    {
        // The finding's reason travels with the abort: what the parser said about the printer's own output.
        std::fprintf(stderr, "fuzz: parse rejected print output: %s at byte %zu\n%.*s\n", second.error,
                     static_cast<std::size_t>(second.error_offset), static_cast<int>(printed.size()), printed.data());
    }
    CRD_FUZZ_REQUIRE(second.ok && second.module != nullptr);
    const String reprinted = crd::ceir::print(again, *second.module, &root);
    CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(bytes_of(printed), printed.size(), bytes_of(reprinted), reprinted.size()));
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    crd::fuzz::BudgetAllocator root;
    crd::ceir::Context         ctx(&root);
    crd::ceir::Module* const   rich = crd::ceir::test::build_rich(ctx);
    const String               text = crd::ceir::print(ctx, *rich, &root);
    sink.add("rich-graph", bytes_of(text), text.size());
    for (const char* const source : kMalformed)
    {
        sink.add_text("malformed", source);
    }
}
