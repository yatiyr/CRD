#pragma once

// ckir_vertex_pull.hpp — GEO-1: the VERTEX-PULLING vertex shader (the bindless vertex-feeding path). The VS fetches its
// clip-space position from the storage buffer (set 0 / binding 0 — the same buffer `draw_storage` binds, now VERTEX-visible)
// by `VertexIndex` against the cooked `MeshResource` 48-byte interleaved stride (12 floats: pos3 + nrm3 + uv2 + tan4,
// ADR-0043). storage_load returns U32 words; the reinterpret is cast→I32 then IntBitsToFloat (GLSL `intBitsToFloat` takes a
// signed int; HLSL `asfloat` takes either). Shared by the Vulkan and DX12 draw gates.

#include <crd/kir/ckir.hpp>

namespace crd::gputest
{

// VS: position = float3 at (VertexIndex * 12 + {0,1,2}) of the pulled vertex stream, w = 1. No interpolants (flat-colour FS).
inline void build_vertex_pull_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});

    const int vid    = g.builtin(kir::KBuiltin::VertexIndex); // int
    const int stride = g.constant(12.0, sh, kir::DType::I32); // 48 bytes / 4 = 12 u32 words per cooked vertex
    const int base   = g.binary(kir::KOp::Mul, vid, stride);

    const auto fetch = [&](int word_offset) {
        const int off  = g.constant(static_cast<crd::f64>(word_offset), sh, kir::DType::I32);
        const int idx  = g.binary(kir::KOp::Add, base, off);
        const int word = g.storage_load(idx);           // U32 word from the pulled stream
        const int bits = g.cast(word, kir::DType::I32); // bit-preserving uint -> int (GLSL intBitsToFloat takes int)
        return g.int_bits_to_float(bits);               // the TYPED reinterpret builder (result F32 — raw unary() mistypes)
    };

    const int x   = fetch(0);
    const int y   = fetch(1);
    const int z   = fetch(2);
    const int w   = g.constant(1.0, sh, kir::DType::F32);
    const int pos = g.vec4(x, y, z, w);

    ve.stage    = kir::KStage::Vertex;
    ve.position = pos;
    ve.n_out    = 0;
}

} // namespace crd::gputest
