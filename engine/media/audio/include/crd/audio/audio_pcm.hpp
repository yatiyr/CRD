#pragma once

// audio_pcm.hpp — GEO-10 (D-007 row 75): `AudioPcm` — the OWNED intermediate every audio codec decodes into
// and encodes from (the MED-band architecture: decode → owned intermediate → encode, ONE transcode engine).
//
// Two sample domains, honestly separated:
//   - INTEGER (bits_per_sample 8/16/24/32): `isamples`, sign-extended to i32 — the BIT-EXACT domain WAV/AIFF/
//     FLAC round-trip gates live in (a 24-bit sample re-encodes to the same 3 bytes, always);
//   - FLOAT (bits_per_sample == 0): `fsamples` — WAV fmt-3 sources AND the engine's processing intermediate.
// `to_f32` normalizes integers by 2^(bits-1) (16/24-bit values are EXACT in f32 — the mantissa holds them);
// the reverse quantization lives with the encoders that need it (explicit, never implicit).
//
// Codecs are span-based and allocator-aware (no filesystem — the caller does I/O), zero 3rd-party.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::audio
{

struct AudioPcm
{
    crd::u32 sample_rate     = 0;
    crd::u16 channels        = 0;
    crd::u16 bits_per_sample = 0; // 8/16/24/32 integer; 0 = FLOAT data (fsamples is live)

    crd::containers::Array<crd::i32> isamples; // interleaved, sign-extended (integer domain)
    crd::containers::Array<crd::f32> fsamples; // interleaved (float domain)

    explicit AudioPcm(crd::memory::IAllocator* a) : isamples(a), fsamples(a) {}
    AudioPcm(AudioPcm&&)            = default;
    AudioPcm& operator=(AudioPcm&&) = default;

    [[nodiscard]] bool is_float() const noexcept { return bits_per_sample == 0; }
    [[nodiscard]] crd::u64 frame_count() const noexcept
    {
        if (channels == 0) { return 0; }
        const crd::usize n = is_float() ? fsamples.size() : isamples.size();
        return static_cast<crd::u64>(n) / channels;
    }
    [[nodiscard]] bool valid() const noexcept
    {
        if (sample_rate == 0 || channels == 0) { return false; }
        if (is_float()) { return fsamples.size() > 0 && fsamples.size() % channels == 0; }
        if (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32)
        {
            return false;
        }
        return isamples.size() > 0 && isamples.size() % channels == 0;
    }
};

// Normalize into `out` (sized by the call): integer lanes divide by 2^(bits-1); float lanes copy verbatim.
void pcm_to_f32(const AudioPcm& pcm, crd::containers::Array<crd::f32>& out);

} // namespace crd::audio
