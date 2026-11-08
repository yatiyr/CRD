#pragma once

// crd-net -- a client's bounded store of TLS session tickets (4.2a; contract:
// docs/design/network-foundation.md#ticket-cache). Sources: RFC 9846 sections 4.6.1 and C.4, RFC 9369 section 5.
//
//   bounds     a fixed number of entries of a fixed size, taken once at construction: nothing a server sends grows it,
//              and a ticket too large for an entry is refused. Under one binding (crypto::TlsBinding: one server name,
//              pins and QUIC version) at most `per_binding` are kept, its own oldest replaced first, so a server
//              issuing many tickets displaces no other server's; a full cache replaces its oldest.
//   use        take() hands the newest ticket of a binding once (section C.4) and forgets it.
//   age        no ticket stays past `max_age_ns`, at most seven days (section 4.6.1). A ticket's shorter lifetime is
//   the
//              provider's check when it is offered. The owner gives the time through advance().
//   secrets    a ticket is its resumption key: a dropped ticket is zeroed at once, the bytes handed out by take() on
//   the
//              cache's next call, and everything on destruction.
//   threads    the owner's thread (a QuicClient's loop).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/tls.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
namespace cont = crd::containers;

inline constexpr u32 kTicketCacheMaxEntries = 4096U;
inline constexpr usize kTicketCacheMaxBytes = usize{1} << 16U;
inline constexpr u64 kTicketMaxAgeNs = 7U * 86400U * u64{1000000000}; // RFC 9846 section 4.6.1

struct TicketCacheConfig
{
    u32 entries = 16U;                // tickets kept at most (0: the cache keeps none; at most kTicketCacheMaxEntries)
    u32 per_binding = 4U;             // of them under one binding (1 to entries)
    usize ticket_bytes = 8192U;       // the largest ticket kept (1 to kTicketCacheMaxBytes)
    u64 max_age_ns = kTicketMaxAgeNs; // 1 to kTicketMaxAgeNs
};

struct TicketCacheCounters
{
    u64 kept = 0U;
    u64 taken = 0U;
    u64 too_large = 0U; // tickets refused for their size
    u64 displaced = 0U; // tickets a newer one replaced
    u64 expired = 0U;   // tickets past max_age_ns
};

class TicketCache final : public crypto::TlsTicketStore
{
public:
    TicketCache(memory::IAllocator& allocator, const TicketCacheConfig& config) noexcept;
    ~TicketCache() override;
    TicketCache(const TicketCache&) = delete;
    TicketCache& operator=(const TicketCache&) = delete;
    TicketCache(TicketCache&&) = delete;
    TicketCache& operator=(TicketCache&&) = delete;

    // False when the configuration is out of range or the allocator refused.
    [[nodiscard]] bool ready() const noexcept { return m_ready; }

    // The time, non-decreasing (an earlier value is ignored): tickets past their age are dropped, at the cost of one
    // comparison unless the oldest may be, and bytes handed out are zeroed.
    void advance(u64 now_ns) noexcept;

    void keep(const crypto::TlsBinding& binding, cont::ConstSpan<u8> ticket) noexcept override;
    [[nodiscard]] cont::ConstSpan<u8> take(const crypto::TlsBinding& binding) noexcept override;

    [[nodiscard]] u32 size() const noexcept { return m_size; }
    [[nodiscard]] const TicketCacheCounters& counters() const noexcept { return m_counters; }

private:
    struct Entry
    {
        crypto::TlsBinding binding;
        u64 order = 0U; // 0: free; else the order it was kept in
        u64 kept_ns = 0U;
        usize bytes = 0U;
    };

    [[nodiscard]] u8* storage(u32 index) const noexcept;
    // Zeroes entry `index` and frees it.
    void wipe(u32 index) noexcept;
    // Wipes the entry take() handed out.
    void scrub() noexcept;

    memory::IAllocator& m_allocator;
    TicketCacheConfig m_config;
    Entry* m_entries = nullptr;
    u8* m_bytes = nullptr; // `entries` * `ticket_bytes`
    u64 m_now_ns = 0U;
    u64 m_oldest_ns = 0U; // no later than every live entry's kept_ns
    u64 m_order = 0U;
    u32 m_size = 0U;
    u32 m_handed = ~u32{0};
    bool m_ready = false;
    TicketCacheCounters m_counters;
};

} // namespace crd::net
