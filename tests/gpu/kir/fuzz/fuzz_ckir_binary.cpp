// crd-fuzz-ckir-binary: the CKIR graph blob loader (crd::kir::deserialize_graph) under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md). Oracle: an accepted blob serializes to bytes that deserialize again
// and serialize identically, the D1 content-hash determinism contract, and (DIAG.10a) writes as `.ckir` text that
// reads back to the same blob, the identity contract of ckir_asset.hpp. Seeds: the D1 scale kernel and the raster
// fragment entry as serialized, their truncations, a flipped byte and plain garbage.
#include <crd/fuzz/harness.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp>
#include <crd/kir/ckir_serialize.hpp>

#include <cstdio>

namespace kir = crd::kir;

namespace
{
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::u8;
using crd::usize;

kir::KEntry build_scale(kir::KGraph& g, double scale)
{
    const int  inbuf  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int  outbuf = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int  lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const auto sh1    = kir::make_shape({1});
    const int  mark   = g.kernel_stmt_mark();
    g.stmt_buffer_store(outbuf, lid,
                        g.binary(kir::KOp::Mul, g.buffer_load(inbuf, lid), g.constant(scale, sh1, kir::DType::F32)));
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 32;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

kir::KEntry build_raster_fs(kir::KGraph& g)
{
    const auto sh1 = kir::make_shape({1});
    const int  uv  = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0);
    const int  tex = g.texture(2, 0);
    const int  smp = g.sampler(2, 1);
    const int  col = g.tex_sample(tex, smp, uv);
    const int  lit = g.binary(kir::KOp::Mul, col, g.constant(0.75, sh1, kir::DType::F32));
    kir::KEntry e;
    e.stage  = kir::KStage::Fragment;
    e.n_out  = 1;
    e.out[0] = kir::KStageOutput{lit, 0, kir::Interp::Smooth};
    return e;
}

void add_blob_and_variants(crd::fuzz::SeedSink& sink, const char* name, const Array<u8>& blob, crd::memory::IAllocator* alloc)
{
    sink.add(name, blob.data(), blob.size());
    for (const usize cut : {usize{0}, usize{4}, usize{8}, usize{20}, blob.size() / 2U, blob.size() - 1U})
    {
        if (cut < blob.size())
        {
            sink.add(name, blob.data(), cut);
        }
    }
    Array<u8> flipped(alloc);
    for (usize i = 0U; i < blob.size(); ++i)
    {
        flipped.push_back(blob[i]);
    }
    if (flipped.size() > 24U)
    {
        flipped[24] = static_cast<u8>(flipped[24] ^ 0xFFU); // past the magic, version and record-width manifest
        sink.add(name, flipped.data(), flipped.size());
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    crd::fuzz::BudgetAllocator root;
    kir::KGraph                graph(&root);
    kir::KEntry                entry;
    if (!kir::deserialize_graph(ConstSpan<u8>(data, size), graph, entry))
    {
        return 0;
    }
    const Array<u8> blob = kir::serialize_graph(graph, entry, &root);
    kir::KGraph     again(&root);
    kir::KEntry     entry_again;
    CRD_FUZZ_REQUIRE(kir::deserialize_graph(ConstSpan<u8>(blob.data(), blob.size()), again, entry_again));
    const Array<u8> reblob = kir::serialize_graph(again, entry_again, &root);
    CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(blob.data(), blob.size(), reblob.data(), reblob.size()));

    // DIAG.10a cross-format oracle (ckir_asset.hpp: serialize_graph(ckir_read(ckir_write(g))) == serialize_graph(g)):
    // every graph the blob loader accepts has a text form that the text loader reads back to the same blob.
    const String              written = kir::ckir_write(graph, entry, &root);
    kir::KGraph               from_text(&root);
    kir::KEntry               entry_text;
    const kir::CkirReadResult read = kir::ckir_read(StringView(written.data(), written.size()), from_text, entry_text);
    if (!read.ok)
    {
        std::fprintf(stderr, "fuzz: ckir_read rejected ckir_write output: %s at byte %zu\n%.*s\n", read.error,
                     static_cast<std::size_t>(read.error_offset), static_cast<int>(written.size()), written.data());
    }
    CRD_FUZZ_REQUIRE(read.ok);
    const Array<u8> text_blob = kir::serialize_graph(from_text, entry_text, &root);
    CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(blob.data(), blob.size(), text_blob.data(), text_blob.size()));
    return 0;
}

namespace
{
[[nodiscard]] bool blob_loads(const u8* data, usize size)
{
    crd::fuzz::BudgetAllocator root;
    kir::KGraph                graph(&root);
    kir::KEntry                entry;
    return kir::deserialize_graph(ConstSpan<u8>(data, size), graph, entry);
}

// `graph` serialized with node `index` changed by `edit`: the shape of the DIAG.10a non-canonical seeds.
template <class Edit>
void add_edited_node_blob(crd::fuzz::SeedSink& sink, const char* name, const kir::KGraph& graph,
                          const kir::KEntry& entry, usize index, Edit edit, crd::memory::IAllocator* alloc)
{
    Array<kir::KNode> nodes(alloc);
    for (usize i = 0U; i < graph.serial_nodes().size(); ++i)
    {
        nodes.push_back(graph.serial_nodes()[i]);
    }
    edit(nodes[index]);
    kir::KGraph copy(alloc);
    copy.serial_restore(nodes.data(), nodes.size(), graph.serial_ext().data(), graph.serial_ext().size(),
                        graph.serial_sfields().data(), graph.serial_sfields().size(), graph.serial_sbegin().data(),
                        graph.serial_sbegin().size(), graph.serial_stmts().data(), graph.serial_stmts().size(),
                        graph.n_inputs());
    const Array<u8> blob = kir::serialize_graph(copy, entry, alloc);
    sink.add(name, blob.data(), blob.size());
}

// DIAG.10a seeds: records the graph builder and the text reader never produce. Each loaded before the fix; a dim past
// the rank then wrote text that read back to another blob, and an out-of-range enum indexed past the writer's name
// tables. Each must now be refused, which the sink checks.
void add_non_canonical_seeds(crd::fuzz::SeedSink& out, crd::memory::IAllocator* alloc)
{
    crd::fuzz::VerdictSink sink(out, &blob_loads, nullptr, 0U);
    kir::KGraph            graph(alloc);
    const kir::KEntry      entry = build_raster_fs(graph);
    const usize            last  = graph.serial_nodes().size() - 1U;
    add_edited_node_blob(sink, "dim-past-rank", graph, entry, last, [](kir::KNode& n) { n.shape.dims[7] = 2; },
                         alloc);
    add_edited_node_blob(sink, "rank-past-max", graph, entry, last, [](kir::KNode& n) { n.shape.rank = 9; }, alloc);
    add_edited_node_blob(sink, "op-out-of-range", graph, entry, last,
                         [](kir::KNode& n) { n.op = static_cast<kir::KOp>(250); }, alloc);
    add_edited_node_blob(sink, "dtype-out-of-range", graph, entry, last,
                         [](kir::KNode& n) { n.type.scalar = static_cast<kir::DType>(200); }, alloc);
    add_edited_node_blob(sink, "tier-out-of-range", graph, entry, last,
                         [](kir::KNode& n) { n.tier = static_cast<kir::DetTier>(7); }, alloc);
    kir::KEntry stray = entry;
    stray.out[3]      = kir::KStageOutput{0, 2, kir::Interp::Flat};
    const Array<u8> stray_blob = kir::serialize_graph(graph, stray, alloc);
    sink.add("stray-stage-output", stray_blob.data(), stray_blob.size());
    kir::KEntry stage = entry;
    stage.stage       = static_cast<kir::KStage>(kir::kStageCount);
    const Array<u8> stage_blob = kir::serialize_graph(graph, stage, alloc);
    sink.add("stage-out-of-range", stage_blob.data(), stage_blob.size());
}
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    crd::fuzz::BudgetAllocator root;
    add_non_canonical_seeds(sink, &root);
    {
        kir::KGraph       graph(&root);
        const kir::KEntry entry = build_scale(graph, 2.0);
        add_blob_and_variants(sink, "scale-kernel", kir::serialize_graph(graph, entry, &root), &root);
    }
    {
        kir::KGraph       graph(&root);
        const kir::KEntry entry = build_raster_fs(graph);
        add_blob_and_variants(sink, "raster-fragment", kir::serialize_graph(graph, entry, &root), &root);
    }
    sink.add_text("garbage", "KGRF but not a blob");
    sink.add_text("empty", "");
}
