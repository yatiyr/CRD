// crd-fuzz-ckir-binary: the CKIR graph blob loader (crd::kir::deserialize_graph) under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md). Oracle: an accepted blob serializes to bytes that deserialize again
// and serialize identically, the D1 content-hash determinism contract. Seeds: the D1 scale kernel and the raster
// fragment entry as serialized, their truncations, a flipped byte and plain garbage.
#include <crd/fuzz/harness.hpp>

#include <crd/containers/array.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_serialize.hpp>

namespace kir = crd::kir;

namespace
{
using crd::containers::Array;
using crd::containers::ConstSpan;
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
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    crd::fuzz::BudgetAllocator root;
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
