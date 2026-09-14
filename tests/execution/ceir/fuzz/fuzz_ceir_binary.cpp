// crd-fuzz-ceir-binary: the CEIR binary loader (crd::ceir::deserialize) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md). Oracle: a rejected blob leaves no module; an accepted blob serializes,
// deserializes and serializes byte-identically, the CEIR-1f round-trip contract the unit tests hold. Seeds: the rich
// graph's blob, its truncations, a flipped byte, a corrupt count and plain garbage.
#include <crd/fuzz/harness.hpp>

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/containers/array.hpp>

#include "../rich_graph.hpp" // crd::ceir::test::build_rich

namespace
{
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::u8;
using crd::usize;

[[nodiscard]] ConstSpan<u8> span_of(const Array<u8>& bytes) noexcept
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
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
}
