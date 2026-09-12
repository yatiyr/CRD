#pragma once

// wav.hpp — GEO-10: OUR OWN WAV (RIFF) codec — the MED-5 lossless floor, GEO-10 is its first consumer.
// Decode: fmt 1 (PCM 8u/16/24/32) + fmt 3 (IEEE float 32/64) + the WAVE_FORMAT_EXTENSIBLE wrapper; unknown
// chunks skip by size (LIST/fact/cue survive); refusals are typed. Encode: PCM 16/24 and float 32 — the
// masters GEO-9 timeline renders and GEO-10 graph renders write. Round-trip is BIT-EXACT (gated).

#include <crd/audio/audio_pcm.hpp>

namespace crd::audio
{

enum class WavError : crd::u8
{
    Ok = 0,
    NotRiffWave,       // missing/short RIFF/WAVE header
    MissingFmt,        // no fmt chunk before data
    UnsupportedFormat, // a format tag / bit depth this codec does not speak
    Malformed,         // sizes contradict the payload
};

[[nodiscard]] WavError wav_decode(crd::containers::ConstSpan<crd::u8> bytes, AudioPcm& out);

// Encode `pcm` as PCM (16/24 — pcm.isamples at pcm.bits_per_sample) or float-32 (pcm.bits_per_sample == 0).
// Empty return = refusal (invalid pcm / a bit depth outside the encoder's surface).
[[nodiscard]] crd::containers::Array<crd::u8> wav_encode(const AudioPcm& pcm, crd::memory::IAllocator* alloc);

} // namespace crd::audio
