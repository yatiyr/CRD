#pragma once

// flac.hpp — GEO-10: OUR OWN FLAC codec (the MED-5 lossless flagship; zero 3rd-party).
//
// Decode: the full mandatory surface — STREAMINFO + skipped metadata, fixed/variable blocking, all block-size/
// rate/bps codes, CONSTANT/VERBATIM/FIXED(0-4)/LPC(1-32) subframes, wasted bits, rice methods 0+1 incl. escape
// partitions, L/S · R/S · M/S decorrelation, CRC-8 + CRC-16 checked per frame, STREAMINFO MD5 verified when
// stamped. Encode: a COMPLIANT SUBSET writer — fixed 4096 blocking, 16/24-bit, stereo decorrelation chosen per
// frame (independent/LS/RS/MS by estimated bits), FIXED + LPC(8, via hesap-dsp aryule) predictors, rice method
// 1 partitions, STREAMINFO with MD5. encode→decode is BIT-EXACT and MD5-verified (gated); the official-decoder
// conformance oracle rides the test suite when present.

#include <crd/audio/audio_pcm.hpp>

namespace crd::audio
{

enum class FlacError : crd::u8
{
    Ok = 0,
    NotFlac,           // missing fLaC marker / no STREAMINFO
    UnsupportedFormat, // a layout outside the decoder's surface (e.g. >2 ch in this build)
    Malformed,         // contradictory sizes / reserved codes / truncated stream
    BadCrc,            // a frame failed CRC-8/CRC-16
    Md5Mismatch,       // decoded PCM does not hash to STREAMINFO's MD5
};

[[nodiscard]] FlacError flac_decode(crd::containers::ConstSpan<crd::u8> bytes, AudioPcm& out);

// Encode integer PCM (16/24-bit, 1-2 channels). Empty return = refusal.
[[nodiscard]] crd::containers::Array<crd::u8> flac_encode(const AudioPcm& pcm, crd::memory::IAllocator* alloc);

} // namespace crd::audio
