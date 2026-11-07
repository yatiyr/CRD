// The RTP packets of the FlexFEC tests and board: drawn from a seeded stream, version 2 with the given SSRC and sequence
// number.

#pragma once

#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>

// An RTP version 2 packet of `payload` bytes after the fixed header, its P, X, CC, M and PT bits and its other bytes
// drawn from the stream.
[[nodiscard]] inline crd::containers::Array<crd::u8> rtp_packet(SeededWords& rng, crd::u32 ssrc, crd::u16 sequence,
                                                                crd::usize payload)
{
    crd::containers::Array<crd::u8> p;
    p.resize(12U + payload, crd::u8{0U});
    for (crd::u8& b : p)
    {
        b = static_cast<crd::u8>(rng.next_u32());
    }
    p[0] = static_cast<crd::u8>(0x80U | (p[0] & 0x3FU));
    p[2] = static_cast<crd::u8>(sequence >> 8U);
    p[3] = static_cast<crd::u8>(sequence);
    p[8] = static_cast<crd::u8>(ssrc >> 24U);
    p[9] = static_cast<crd::u8>(ssrc >> 16U);
    p[10] = static_cast<crd::u8>(ssrc >> 8U);
    p[11] = static_cast<crd::u8>(ssrc);
    return p;
}
