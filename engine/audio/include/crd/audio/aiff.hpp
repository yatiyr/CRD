#pragma once

// aiff.hpp — GEO-10: OUR OWN AIFF (IFF) codec — big-endian PCM 8/16/24/32 with the classic 80-bit extended
// sample-rate field (parsed exactly, written exactly for integer rates). Decode skips unknown chunks by size;
// encode writes COMM + SSND. Round-trip is BIT-EXACT (gated).

#include <crd/audio/audio_pcm.hpp>

namespace crd::audio
{

enum class AiffError : crd::u8
{
    Ok = 0,
    NotAiff,           // missing/short FORM/AIFF header
    MissingComm,       // no COMM chunk before SSND
    UnsupportedFormat, // a bit depth / compression this codec does not speak (AIFC compressed refuses)
    Malformed,         // sizes contradict the payload
};

[[nodiscard]] AiffError aiff_decode(crd::containers::ConstSpan<crd::u8> bytes, AudioPcm& out);

// Encode integer PCM (16/24). Empty return = refusal.
[[nodiscard]] crd::containers::Array<crd::u8> aiff_encode(const AudioPcm& pcm, crd::memory::IAllocator* alloc);

} // namespace crd::audio
