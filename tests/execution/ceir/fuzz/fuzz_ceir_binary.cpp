// crd-fuzz-ceir-binary: the CEIR binary loader (crd::ceir::deserialize) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md). Oracle: a rejected blob leaves no module; an accepted blob serializes,
// deserializes and serializes byte-identically, the CEIR-1f round-trip contract the unit tests hold, and (DIAG.10a)
// prints to text that the text loader accepts and prints identically, so every module the binary loader accepts has
// a text form. Seeds: the rich graph's blob, its truncations, a flipped byte, a corrupt count and plain garbage.
#include <crd/fuzz/harness.hpp>

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

#include <cstdio>

#include "../rich_graph.hpp" // crd::ceir::test::build_rich

namespace
{
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::u8;
using crd::usize;

[[nodiscard]] ConstSpan<u8> span_of(const Array<u8>& bytes) noexcept
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

[[nodiscard]] bool blob_loads(const u8* data, usize size)
{
    crd::fuzz::BudgetAllocator   root;
    crd::ceir::Context           ctx(&root);
    const crd::ceir::ParseResult result = crd::ceir::deserialize(ctx, ConstSpan<u8>(data, size));
    return result.ok;
}

// One module holding one op `name` (dialect.op split on the first dot) whose `v` attribute is `value` (none when
// invalid), serialized: the shape of the DIAG.10a unprintable-name seeds.
void add_one_op_blob(crd::fuzz::SeedSink& sink, const char* seed_name, crd::ceir::Context& ctx, StringView dialect,
                     StringView op_name, crd::ceir::AttrId value)
{
    crd::ceir::Module* const    m   = ctx.create_module();
    crd::ceir::Block* const     top = ctx.create_block();
    crd::ceir::Operation* const op  = ctx.create_operation(ctx.intern_op(dialect, op_name), {}, 0U);
    m->body()->append(top);
    if (value.valid())
    {
        ctx.set_attr(op, "v", value);
    }
    top->append(op);
    const Array<u8> blob = crd::ceir::serialize(ctx, *m, ctx.allocator());
    sink.add(seed_name, blob.data(), blob.size());
}

// DIAG.10a seeds: one blob per name the printer writes bare, each carrying a name the text lexer refuses, plus an
// empty symbol ref the parser refuses. Each loaded before the fix and printed text the parser refused (the first two
// shapes came from the cross-format oracle on committed inputs); each must now be refused, which the sink checks.
void add_unprintable_name_seeds(crd::fuzz::SeedSink& out)
{
    crd::fuzz::VerdictSink sink(out, &blob_loads, nullptr, 0U);
    crd::fuzz::BudgetAllocator root;
    using crd::ceir::TypeId;
    {
        crd::ceir::Context ctx(&root);
        add_one_op_blob(sink, "unprintable-op-name", ctx, "tes", ")sink", {});
    }
    {
        crd::ceir::Context ctx(&root);
        add_one_op_blob(sink, "unprintable-trait-name", ctx, "test", "x", ctx.attr_type(ctx.type_trait("2rd", {})));
    }
    {
        crd::ceir::Context ctx(&root);
        const TypeId       fields[1] = {ctx.type_i32()};
        const StringView   labels[1] = {StringView("x")};
        const TypeId       point     = ctx.type_struct("Point 2", ConstSpan<TypeId>(fields, 1U),
                                                       ConstSpan<StringView>(labels, 1U));
        add_one_op_blob(sink, "unprintable-struct-name", ctx, "test", "x", ctx.attr_type(point));
    }
    {
        crd::ceir::Context ctx(&root);
        const TypeId       fields[1] = {ctx.type_i32()};
        const StringView   labels[1] = {StringView("x:y")};
        const TypeId       point     = ctx.type_struct("Point", ConstSpan<TypeId>(fields, 1U),
                                                       ConstSpan<StringView>(labels, 1U));
        add_one_op_blob(sink, "unprintable-field-label", ctx, "test", "x", ctx.attr_type(point));
    }
    {
        crd::ceir::Context ctx(&root);
        const StringView   cases[2] = {StringView("Red"), StringView("Not-Red")};
        const TypeId       colour   = ctx.type_enum("Colour", ConstSpan<StringView>(cases, 2U));
        add_one_op_blob(sink, "unprintable-enum-case", ctx, "test", "x", ctx.attr_type(colour));
    }
    {
        crd::ceir::Context ctx(&root);
        add_one_op_blob(sink, "unprintable-param-name", ctx, "test", "x", ctx.attr_type(ctx.type_param("", {})));
    }
    {
        crd::ceir::Context ctx(&root);
        add_one_op_blob(sink, "unprintable-dim-name", ctx, "test", "x", ctx.attr_type(ctx.type_dim_symbolic("n m")));
    }
    {
        crd::ceir::Context     ctx(&root);
        const crd::ceir::Type params;
        const TypeId          ext = ctx.type_extern(ctx.intern_type_class("my-dialect", "shape"), params);
        add_one_op_blob(sink, "unprintable-type-class", ctx, "test", "x", ctx.attr_type(ext));
    }
    {
        crd::ceir::Context ctx(&root);
        const crd::ceir::AttrId ext = ctx.attr_extern(ctx.intern_attr_class("my-dialect", "meta"), ctx.attr_int(7));
        add_one_op_blob(sink, "unprintable-attr-class", ctx, "test", "x", ext);
    }
    {
        crd::ceir::Context ctx(&root);
        add_one_op_blob(sink, "empty-symbol-ref", ctx, "test", "x", ctx.attr_symbol(""));
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    crd::fuzz::BudgetAllocator   root;
    crd::ceir::Context           ctx(&root);
    const crd::ceir::ParseResult first = crd::ceir::deserialize(ctx, ConstSpan<u8>(data, size));
    if (!first.ok)
    {
        CRD_FUZZ_REQUIRE(first.module == nullptr);
        return 0;
    }
    CRD_FUZZ_REQUIRE(first.module != nullptr);
    const Array<u8> blob = crd::ceir::serialize(ctx, *first.module, &root);

    crd::ceir::Context           again(&root);
    const crd::ceir::ParseResult second = crd::ceir::deserialize(again, span_of(blob));
    CRD_FUZZ_REQUIRE(second.ok && second.module != nullptr);
    const Array<u8> reblob = crd::ceir::serialize(again, *second.module, &root);
    CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(blob.data(), blob.size(), reblob.data(), reblob.size()));

    // DIAG.10a cross-format oracle: the accepted module prints, the text loader accepts the printer's output, and it
    // prints back byte-identically (the text form drops Region::kind and source positions, so text is compared).
    const String                 printed = crd::ceir::print(ctx, *first.module, &root);
    crd::ceir::Context           text_ctx(&root);
    const crd::ceir::ParseResult parsed = crd::ceir::parse(text_ctx, StringView(printed.data(), printed.size()));
    if (!parsed.ok)
    {
        std::fprintf(stderr, "fuzz: parse rejected print output: %s at byte %zu\n%.*s\n", parsed.error,
                     static_cast<std::size_t>(parsed.error_offset), static_cast<int>(printed.size()), printed.data());
    }
    CRD_FUZZ_REQUIRE(parsed.ok && parsed.module != nullptr);
    const String reprinted = crd::ceir::print(text_ctx, *parsed.module, &root);
    CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(reinterpret_cast<const u8*>(printed.data()), printed.size(),
                                            reinterpret_cast<const u8*>(reprinted.data()), reprinted.size()));
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    crd::fuzz::BudgetAllocator root;
    crd::ceir::Context         ctx(&root);
    crd::ceir::Module* const   rich = crd::ceir::test::build_rich(ctx);
    const Array<u8>            blob = crd::ceir::serialize(ctx, *rich, &root);
    sink.add("rich-graph", blob.data(), blob.size());
    for (const usize cut : {usize{0}, usize{4}, usize{8}, usize{16}, usize{32}, blob.size() / 2U, blob.size() - 1U})
    {
        if (cut < blob.size())
        {
            sink.add("rich-graph-truncated", blob.data(), cut);
        }
    }
    Array<u8> flipped(&root);
    for (usize i = 0U; i < blob.size(); ++i)
    {
        flipped.push_back(blob[i]);
    }
    if (flipped.size() > 12U)
    {
        flipped[12] = static_cast<u8>(flipped[12] ^ 0xFFU); // the first count field after magic, version and chunk id
        sink.add("rich-graph-corrupt-count", flipped.data(), flipped.size());
    }
    sink.add_text("garbage", "CEIR but not a blob");
    sink.add_text("empty", "");
    add_unprintable_name_seeds(sink);
}
