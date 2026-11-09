#pragma once

// crd-net -- a connection as the application holds it, on either host (quic_host.hpp, qmux_host.hpp; 4.2a). A handle
// carries its slot's generation, so a stale one finds nothing.

#include <crd/core/types.hpp>
#include <crd/net/connection_table.hpp>

namespace crd::net
{
inline constexpr u32 kQuicMaxHostConnections = u32{1} << 20U;

struct QuicHandle
{
    u32 slot = kNoConnection;
    u32 generation = 0U;

    [[nodiscard]] bool valid() const noexcept { return slot != kNoConnection; }
    friend bool operator==(const QuicHandle&, const QuicHandle&) noexcept = default;
};

} // namespace crd::net
