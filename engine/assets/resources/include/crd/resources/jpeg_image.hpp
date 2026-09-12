#pragma once

// jpeg_image.hpp — OUR OWN baseline JPEG decoder (ITU T.81; JFIF) — the other glTF-official image format and the photo
// workhorse. Coverage: baseline + extended-sequential Huffman (SOF0/SOF1) · grayscale + YCbCr 3-component · ALL chroma
// samplings (4:4:4 / 4:2:2 / 4:2:0 / 4:1:1 — general h/v factors ≤ 4) · restart markers (DRI/RSTn) · 0xFF00 byte
// stuffing · multiple DQT/DHT segments · APPn/COM skip. PROGRESSIVE (SOF2), arithmetic coding, hierarchical, and Adobe
// CMYK are `Unsupported` BY NAME — never mis-decoded (progressive is the named follow-on when web-sourced assets demand
// it). IDCT: the exact separable float 8×8 (deterministic under IEEE — no SIMD paths); chroma upsampling: sample
// replication (spec-legal). Output RGBA8 (JFIF full-range BT.601 YCbCr→RGB).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/ldr_image.hpp>

namespace crd::resources
{

[[nodiscard]] bool     jpeg_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept;
[[nodiscard]] LdrError jpeg_decode(crd::containers::ConstSpan<crd::u8> bytes, LdrImage& out, crd::memory::IAllocator* a);

} // namespace crd::resources
