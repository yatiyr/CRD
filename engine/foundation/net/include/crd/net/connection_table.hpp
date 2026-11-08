#pragma once

// crd-net -- admission stage S2: the table that routes a datagram to its connection by Destination Connection ID
// (4.2a, threat T10; contract: docs/design/network-foundation.md#ingress).
//
//   keying    every slot is chosen by a secret SipHash-2-4 key (crypto::PrfKey), never a public hash: a peer that
//             picks IDs (a client's first Destination Connection ID, or any lookup) cannot aim them at one run of
//             slots, so a lookup costs constant expected time under adversarial IDs. Without a key the table is not
//             ready and routes nothing; it never falls back to a public function.
//   memory    slots for twice the capacity, rounded up to a power of two, from the injected allocator at
//             construction; linear probing with backward-shift deletion keeps no tombstones, so a table that churns
//             stays as fast as a fresh one. Nothing is allocated after construction.
//   threads   one I/O loop owns a table; nothing here is synchronised.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
namespace cont = crd::containers;

inline constexpr u32 kNoConnection = ~u32{0};
inline constexpr u32 kMaxRoutes = u32{1} << 30U;

enum class RouteError : u8
{
    None,
    NotReady,  // the table holds no storage or no key
    Full,      // it holds `capacity` IDs already
    Exists,    // the ID routes somewhere already
    BadLength, // longer than 20 bytes
    BadValue,  // kNoConnection is not a connection
};

class ConnectionIdTable
{
public:
    // Room for `capacity` IDs (1 to kMaxRoutes). `key` is kPrfKeyBytes that cooperating processes share (servers behind
    // one load balancer that hash IDs alike, NET-SIM replaying a run), or empty for a fresh key from the provider.
    ConnectionIdTable(memory::IAllocator& allocator, u32 capacity, cont::ConstSpan<u8> key = {}) noexcept;
    ~ConnectionIdTable();
    ConnectionIdTable(const ConnectionIdTable&) = delete;
    ConnectionIdTable& operator=(const ConnectionIdTable&) = delete;
    ConnectionIdTable(ConnectionIdTable&&) = delete;
    ConnectionIdTable& operator=(ConnectionIdTable&&) = delete;

    [[nodiscard]] bool ready() const noexcept { return m_slots != nullptr; }
    [[nodiscard]] u32 size() const noexcept { return m_size; }
    [[nodiscard]] u32 capacity() const noexcept { return m_capacity; }

    // Routes `id` (0 to 20 bytes) to `connection`.
    [[nodiscard]] RouteError insert(cont::ConstSpan<u8> id, u32 connection) noexcept;
    // The connection `id` routes to; kNoConnection for none.
    [[nodiscard]] u32 find(cont::ConstSpan<u8> id) const noexcept;
    // Stops routing `id`; false when it routed nowhere.
    bool erase(cont::ConstSpan<u8> id) noexcept;

    // The most slots one insert or lookup has walked: evidence that adversarial IDs stay near the expected one or two.
    [[nodiscard]] u32 longest_probe() const noexcept { return m_longest_probe; }

private:
    struct Slot
    {
        u64 hash = 0U;
        u32 connection = kNoConnection; // kNoConnection marks an empty slot
        u8 length = 0U;
        u8 id[crypto::kQuicMaxConnectionIdBytes] = {};
    };

    [[nodiscard]] bool hash_of(cont::ConstSpan<u8> id, u64& out) const noexcept;
    [[nodiscard]] u32 locate(cont::ConstSpan<u8> id, u64 hash) const noexcept;

    memory::IAllocator& m_allocator;
    crypto::PrfKey m_key;
    Slot* m_slots = nullptr;
    u32 m_mask = 0U;
    u32 m_capacity = 0U;
    u32 m_size = 0U;
    mutable u32 m_longest_probe = 0U;
};

} // namespace crd::net
