// test_ckir_serialize_determinism.cpp — the D1 SERIALIZATION-DETERMINISM gate (D-007).
//
// `serialize_graph` is the CONTENT-HASH SOURCE for the whole cook: the shader cache key, the D3 variant-matrix dedup
// (`cook_variant_matrix{,_parallel}`) and the D8 container's unique-bundle table all hash its bytes. That only works if
// the blob is a PURE FUNCTION OF THE GRAPH'S CONTENT — two structurally identical graphs must serialize byte-identically
// regardless of the stack/heap history that happened to precede them.
//
// SCAR (2026-07-25): it wasn't. The pools were blasted raw (`wbytes(arr.data(), n * sizeof(KNode))`), so every KNode /
// KStmt / KType / KEntry PADDING byte — indeterminate, because the builders default-initialize (`KNode n;`) — landed in
// the hash. Identical variant keys therefore hashed DIFFERENTLY whenever the surrounding allocation layout differed,
// which is exactly what win-asan's randomized layout does: the D3/D5/D6/D8/D10/D12 dedup counts went wrong under
// asan/shipping while win-debug's stabler layout masked it. Fixed by writing a CANONICAL, packed, padding-free record
// per element (see ckir_serialize.hpp). These tests dirty the stack with two different patterns between two otherwise
// identical builds and demand byte-identical blobs — they FAIL on the raw-POD encoding and pass on the canonical one.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_serialize.hpp>

#include <crd/core/platform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstring>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
// Fill a big stack region with `pattern`, so the KNode/KStmt/KEntry locals the builders default-initialize sit on
// KNOWN-DIFFERENT garbage between the two builds. noinline + volatile so the optimizer cannot elide it.
CRD_NOINLINE void dirty_stack(crd::u8 pattern) noexcept
{
    constexpr crd::usize k_bytes = 48U * 1024U;
    volatile crd::u8     buf[k_bytes];
    for (crd::usize i = 0; i < k_bytes; ++i) { buf[i] = pattern; }
    volatile crd::u8 sink = 0;
    for (crd::usize i = 0; i < k_bytes; i += 512U) { sink = static_cast<crd::u8>(sink + buf[i]); }
    (void)sink;
}

// The D3 variant kernel (tests/gpu-context-vulkan D3): out[lid] = in[lid] * scale.
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

// A raster (vertex + fragment) graph — exercises KEntry's out[] array, StageIn/StageOut, textures and struct types, i.e.
// the padding-heavy corners the compute kernel does not reach.
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

// Serialize a freshly-built graph after dirtying the stack with `pattern`.
crd::containers::Array<crd::u8> blob_of_scale(crd::u8 pattern, double scale, crd::memory::IAllocator* a)
{
    dirty_stack(pattern);
    kir::KGraph       g(a);
    const kir::KEntry e = build_scale(g, scale);
    return kir::serialize_graph(g, e, a);
}

crd::containers::Array<crd::u8> blob_of_raster(crd::u8 pattern, crd::memory::IAllocator* a)
{
    dirty_stack(pattern);
    kir::KGraph       g(a);
    const kir::KEntry e = build_raster_fs(g);
    return kir::serialize_graph(g, e, a);
}

// Report the FIRST differing offset (a diagnostic that names the padding field when it regresses).
int first_diff(const crd::containers::Array<crd::u8>& x, const crd::containers::Array<crd::u8>& y)
{
    const crd::usize n = x.size() < y.size() ? x.size() : y.size();
    for (crd::usize i = 0; i < n; ++i)
    {
        if (x[i] != y[i]) { return static_cast<int>(i); }
    }
    return x.size() == y.size() ? -1 : static_cast<int>(n);
}
} // namespace

TEST_CASE("D-007 D1: serialize_graph is a pure function of graph CONTENT (no stack/heap history in the bytes)",
          "[kir][serialize][determinism]")
{
    crd::memory::TlsfAllocator a1(4U << 20U);
    crd::memory::TlsfAllocator a2(4U << 20U);

    // Same kernel, two different stack histories AND two different allocators ⇒ byte-identical blob.
    const auto b_aa = blob_of_scale(0xAAU, 1.0, &a1);
    const auto b_55 = blob_of_scale(0x55U, 1.0, &a2);
    CHECK(b_aa.size() == b_55.size());
    CHECK(first_diff(b_aa, b_55) == -1);

    // ...and a DIFFERENT kernel must still differ (the gate must not be trivially satisfied by an empty blob).
    const auto b_two = blob_of_scale(0xAAU, 2.0, &a1);
    CHECK(b_two.size() == b_aa.size());
    CHECK(first_diff(b_two, b_aa) != -1);
}

TEST_CASE("D-007 D1: serialize_graph determinism holds for a RASTER entry (out[]/StageIn/texture/sampler)",
          "[kir][serialize][determinism]")
{
    crd::memory::TlsfAllocator a1(4U << 20U);
    crd::memory::TlsfAllocator a2(4U << 20U);
    const auto                 b_aa = blob_of_raster(0xAAU, &a1);
    const auto                 b_55 = blob_of_raster(0x55U, &a2);
    CHECK(b_aa.size() == b_55.size());
    CHECK(first_diff(b_aa, b_55) == -1);
}

// The cook writes `ShaderReflection` RAW into the bundle's REFL chunk, so its PADDING is part of the cooked bytes. Two cooks
// of the same graph must therefore produce a byte-identical reflection POD — the D10 (parallel == serial) and D12 (recook
// identity) gates compare whole .crdr files. SCAR: they didn't; the 3-byte hole after `stage` carried stack garbage (measured
// at file offset 1881..1883). `ShaderReflection r{}` does NOT fix it under MSVC — only an explicit memset does.
TEST_CASE("D-007 D1: reflect() returns a byte-identical POD for identical graphs (no padding garbage in the REFL chunk)",
          "[kir][serialize][determinism]")
{
    crd::memory::TlsfAllocator a1(4U << 20U);
    crd::memory::TlsfAllocator a2(4U << 20U);

    const auto refl_of = [](crd::u8 pattern, crd::memory::IAllocator* a) {
        dirty_stack(pattern);
        kir::KGraph       g(a);
        const kir::KEntry e = build_scale(g, 1.0);
        return kir::reflect(g, e);
    };
    const kir::ShaderReflection r_aa = refl_of(0xAAU, &a1);
    const kir::ShaderReflection r_55 = refl_of(0x55U, &a2);
    CHECK(std::memcmp(&r_aa, &r_55, sizeof(kir::ShaderReflection)) == 0);
    CHECK(r_aa.n_bindings == 2); // the kernel's in + out storage buffers — the POD is not trivially empty

    // The raster entry exercises the vertex-attribute half of the POD (its own padding hole).
    const auto raster_refl_of = [](crd::u8 pattern, crd::memory::IAllocator* a) {
        dirty_stack(pattern);
        kir::KGraph       g(a);
        const kir::KEntry e = build_raster_fs(g);
        return kir::reflect(g, e);
    };
    const kir::ShaderReflection v_aa = raster_refl_of(0xAAU, &a1);
    const kir::ShaderReflection v_55 = raster_refl_of(0x55U, &a2);
    CHECK(std::memcmp(&v_aa, &v_55, sizeof(kir::ShaderReflection)) == 0);
}

TEST_CASE("D-007 D1: the canonical blob still round-trips (deserialize -> re-serialize is byte-identical)",
          "[kir][serialize][determinism]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph                g(&a);
    const kir::KEntry          e    = build_scale(g, 2.0);
    const auto                 blob = kir::serialize_graph(g, e, &a);

    kir::KGraph g2(&a);
    kir::KEntry e2;
    REQUIRE(kir::deserialize_graph(crd::containers::ConstSpan<crd::u8>(blob.data(), blob.size()), g2, e2));
    CHECK(g2.size() == g.size());
    CHECK(g2.stmt_count() == g.stmt_count());
    CHECK(e2.stage == e.stage);
    CHECK(e2.local_size[0] == e.local_size[0]);
    CHECK(e2.kernel_body_begin == e.kernel_body_begin);
    CHECK(e2.kernel_body_count == e.kernel_body_count);

    const auto blob2 = kir::serialize_graph(g2, e2, &a);
    CHECK(blob2.size() == blob.size());
    CHECK(first_diff(blob2, blob) == -1);
}
