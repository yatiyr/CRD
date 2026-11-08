#pragma once

// crd-net tests -- the stream pattern the QUIC stream tests write and check (test_quic_stream_set.cpp and
// test_quic_connection.cpp).

#include <crd/net/quic_stream_set.hpp>

#include <catch2/catch_test_macros.hpp>

namespace crd::net::testing
{
// The byte at `offset` of a stream carrying pattern `pattern_id`.
inline u8 pattern(u64 pattern_id, u64 offset)
{
    return static_cast<u8>((offset * 31U) + (pattern_id * 7U) + (offset >> 8U));
}

// Reads everything readable on `id`, checking the pattern from `offset`; true once the FIN was read (or before).
inline bool drain(QuicStreamSet& set, u64 id, u64& offset, u64 pattern_id)
{
    if (set.recv_state(id) == QuicRecvState::DataRead)
    {
        return true;
    }
    for (QuicStreamRead read = set.peek(id);; read = set.peek(id))
    {
        for (usize at = 0U; at < read.data.size(); ++at)
        {
            REQUIRE(read.data[at] == pattern(pattern_id, offset + at));
        }
        offset += read.data.size();
        REQUIRE(set.consume(id, read.data.size()) == QuicStreamError::None);
        if (read.fin)
        {
            return true;
        }
        if (read.data.empty())
        {
            return false;
        }
    }
}

// Writes the pattern from `offset` up to `until`, with the FIN there when `fin`; true once all of it was taken.
inline bool write_pattern(QuicStreamSet& set, u64 id, u64& offset, u64 until, u64 pattern_id, bool fin = true)
{
    u8 chunk[1024];
    for (;;)
    {
        const usize want = static_cast<usize>((until - offset < sizeof(chunk)) ? until - offset : sizeof(chunk));
        for (usize at = 0U; at < want; ++at)
        {
            chunk[at] = pattern(pattern_id, offset + at);
        }
        usize taken = 0U;
        const QuicStreamError error =
            set.write(id, containers::ConstSpan<u8>(chunk, want), fin && offset + want == until, taken);
        if (error == QuicStreamError::Ended)
        {
            return true; // finished before
        }
        REQUIRE(error == QuicStreamError::None);
        offset += taken;
        if (taken < want)
        {
            return false;
        }
        if (offset == until)
        {
            return true;
        }
    }
}
} // namespace crd::net::testing
