// crd-fuzz-ckir-text: the .ckir text loader (crd::kir::ckir_read) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md). Oracle: an accepted text writes back, re-reads and serializes to the same bytes,
// the hash-exact identity contract `serialize_graph(ckir_read(ckir_write(g))) == serialize_graph(g)` the asset tests
// hold. Seeds: the D1 scale kernel and the raster fragment entry as written by ckir_write, plus the hand malformed
// table of test_ckir_asset.cpp.
#include <crd/fuzz/harness.hpp>

#include <crd/containers/array.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp>
#include <crd/kir/ckir_serialize.hpp>

#include <cstdio>

namespace kir = crd::kir;

namespace
{
using crd::containers::Array;
using crd::containers::String;
using crd::containers::StringView;
using crd::u8;

// out[lid] = in[lid] * scale (the D1 scale kernel of the asset tests).
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

const char* const kMalformed[] = {
    "",
    "garbage not a program",
    "KIR1 inputs 0 nodes 1 NotAnOp F32 Scalar 0 0 0 0 0",
    "schema = 1\n[[entry]]\nstage = \"Compute\"\n[[node]]\n",
    "schema = 1\n[[entry]]\nstage = \"Compute\"\n[[node]]\nop = \"Const\"\nin = [\"n7\"]\n",
};
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
    const kir::CkirReadResult  first = kir::ckir_read(StringView(reinterpret_cast<const char*>(data), size), graph, entry);
    if (!first.ok)
    {
        return 0;
    }
    const String    written = kir::ckir_write(graph, entry, &root);
    kir::KGraph     again(&root);
    kir::KEntry     entry_again;
    const kir::CkirReadResult second = kir::ckir_read(StringView(written.data(), written.size()), again, entry_again);
    if (!second.ok)
    {
        // The finding's reason travels with the abort: what the reader said about the writer's own output.
        std::fprintf(stderr, "fuzz: ckir_read rejected ckir_write output: %s at byte %zu\n", second.error,
                     static_cast<std::size_t>(second.error_offset));
    }
    CRD_FUZZ_REQUIRE(second.ok);
    const Array<u8> blob   = kir::serialize_graph(graph, entry, &root);
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
        const String      text  = kir::ckir_write(graph, entry, &root);
        sink.add("scale-kernel", reinterpret_cast<const u8*>(text.data()), text.size());
    }
    {
        kir::KGraph       graph(&root);
        const kir::KEntry entry = build_raster_fs(graph);
        const String      text  = kir::ckir_write(graph, entry, &root);
        sink.add("raster-fragment", reinterpret_cast<const u8*>(text.data()), text.size());
    }
    for (const char* const source : kMalformed)
    {
        sink.add_text("malformed", source);
    }
}
