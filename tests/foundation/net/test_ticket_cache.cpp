// crd-net-tests: a client's bounded store of TLS session tickets (4.2a;
// docs/design/network-foundation.md#ticket-cache): the configurations refused, each ticket handed out once and newest
// first, the bound per binding and overall, the size limit at its edge, the age limit, and that a ticket's bytes are
// gone from memory when it is dropped, after it was handed out, and on destruction.

#include "ingress_fixture.hpp"

#include <crd/net/ticket_cache.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::containers::ConstSpan;
using crd::crypto::TlsBinding;

constexpr usize kSmall = 32U;

TlsBinding binding_of(u8 value)
{
    TlsBinding binding;
    binding.bytes[0] = value;
    binding.bytes[31] = 0x5AU;
    return binding;
}

// A ticket of `size` bytes, each `fill`.
struct Ticket
{
    Ticket(u8 fill, usize size) : bytes(size) { std::memset(data, fill, sizeof(data)); }

    [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(data, bytes); }

    u8 data[256] = {};
    usize bytes = 0U;
};

// Hands out blocks and holds them past deallocate, so a test can read what the cache left behind.
class Retaining final : public memory::IAllocator
{
public:
    ~Retaining() override
    {
        for (usize index = 0U; index < m_count; ++index)
        {
            std::free(m_block[index]);
        }
    }

    void* allocate(usize size, usize) override
    {
        REQUIRE(m_count < 2U);
        m_block[m_count] = std::malloc(size);
        m_size[m_count] = size;
        return m_block[m_count++];
    }

    void deallocate(void*) noexcept override { ++m_freed; }
    [[nodiscard]] bool owns(const void*) const noexcept override { return true; }

    // Whether the second block, the tickets', holds a byte of `fill` anywhere.
    [[nodiscard]] bool holds(u8 fill) const
    {
        const auto* const bytes = static_cast<const u8*>(m_block[1]);
        for (usize index = 0U; index < m_size[1]; ++index)
        {
            if (bytes[index] == fill)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] usize freed() const { return m_freed; }

private:
    void* m_block[2] = {};
    usize m_size[2] = {};
    usize m_count = 0U;
    usize m_freed = 0U;
};

TicketCacheConfig small(u32 entries, u32 per_binding)
{
    TicketCacheConfig config;
    config.entries = entries;
    config.per_binding = per_binding;
    config.ticket_bytes = kSmall;
    config.max_age_ns = 1000U;
    return config;
}

bool first_byte(ConstSpan<u8> ticket, u8 fill)
{
    return !ticket.empty() && ticket.data()[0] == fill;
}
} // namespace

TEST_CASE("Ticket cache: the configurations refused and every allocation refused", "[net][4.2a][ticket-cache]")
{
    CountingAllocator allocator;
    const auto ready = [&](const TicketCacheConfig& config)
    {
        const TicketCache cache(allocator, config);
        return cache.ready();
    };
    CHECK(ready(TicketCacheConfig{}));
    CHECK(ready(small(kTicketCacheMaxEntries, kTicketCacheMaxEntries)));
    CHECK_FALSE(ready(small(kTicketCacheMaxEntries + 1U, 1U)));
    CHECK_FALSE(ready(small(4U, 0U)));
    CHECK_FALSE(ready(small(4U, 5U)));
    const auto refused = [&](auto change)
    {
        TicketCacheConfig config = small(4U, 1U);
        change(config);
        return !ready(config);
    };
    CHECK(refused([](TicketCacheConfig& c) { c.ticket_bytes = 0U; }));
    CHECK(refused([](TicketCacheConfig& c) { c.ticket_bytes = kTicketCacheMaxBytes + 1U; }));
    CHECK(refused([](TicketCacheConfig& c) { c.max_age_ns = 0U; }));
    CHECK(refused([](TicketCacheConfig& c) { c.max_age_ns = kTicketMaxAgeNs + 1U; }));
    CHECK_FALSE(refused([](TicketCacheConfig& c) { c.ticket_bytes = kTicketCacheMaxBytes; }));
    CHECK_FALSE(refused([](TicketCacheConfig& c) { c.max_age_ns = kTicketMaxAgeNs; }));

    for (usize allowed = 0U; allowed < 2U; ++allowed)
    {
        CountingAllocator refusing;
        refusing.refuse_after(allowed);
        const TicketCache cache(refusing, small(4U, 1U));
        CHECK_FALSE(cache.ready());
    }

    // No entries: ready, and it keeps nothing whatever the other fields say.
    TicketCacheConfig none = small(0U, 0U);
    none.ticket_bytes = 0U;
    TicketCache empty(allocator, none);
    REQUIRE(empty.ready());
    const Ticket ticket(0x11U, kSmall);
    empty.keep(binding_of(1U), ticket.view());
    empty.advance(10U);
    CHECK(empty.size() == 0U);
    CHECK(empty.take(binding_of(1U)).empty());
    CHECK(empty.counters().kept == 0U);
}

TEST_CASE("Ticket cache: a ticket is handed out once, the newest of its binding first", "[net][4.2a][ticket-cache]")
{
    CountingAllocator allocator;
    TicketCache cache(allocator, small(8U, 4U));
    REQUIRE(cache.ready());
    const usize calls = allocator.calls();
    const Ticket older(0x21U, 20U);
    const Ticket newer(0x22U, kSmall);
    const Ticket other(0x31U, 1U);
    cache.keep(binding_of(1U), older.view());
    cache.keep(binding_of(2U), other.view());
    cache.keep(binding_of(1U), newer.view());
    CHECK(cache.size() == 3U);
    CHECK(cache.counters().kept == 3U);

    CHECK(cache.take(binding_of(9U)).empty());
    const ConstSpan<u8> first = cache.take(binding_of(1U));
    REQUIRE(first.size() == kSmall);
    CHECK(std::memcmp(first.data(), newer.data, kSmall) == 0);
    CHECK(cache.size() == 2U);
    const ConstSpan<u8> second = cache.take(binding_of(1U));
    REQUIRE(second.size() == 20U);
    CHECK(std::memcmp(second.data(), older.data, 20U) == 0);
    CHECK(cache.take(binding_of(1U)).empty());
    CHECK(cache.counters().taken == 2U);
    CHECK(first_byte(cache.take(binding_of(2U)), 0x31U));
    CHECK(cache.size() == 0U);
    CHECK(allocator.calls() == calls); // keeping and taking allocate nothing
}

TEST_CASE("Ticket cache: the bounds, per binding and overall, displace the oldest", "[net][4.2a][ticket-cache]")
{
    CountingAllocator allocator;
    {
        // One server's flood replaces only its own tickets.
        TicketCache cache(allocator, small(4U, 2U));
        const Ticket mine(0x41U, 8U);
        cache.keep(binding_of(1U), mine.view());
        for (u8 fill = 0x50U; fill < 0x55U; ++fill)
        {
            const Ticket flood(fill, 8U);
            cache.keep(binding_of(2U), flood.view());
        }
        CHECK(cache.size() == 3U);
        CHECK(cache.counters().displaced == 3U);
        CHECK(first_byte(cache.take(binding_of(2U)), 0x54U));
        CHECK(first_byte(cache.take(binding_of(2U)), 0x53U));
        CHECK(cache.take(binding_of(2U)).empty());
        CHECK(first_byte(cache.take(binding_of(1U)), 0x41U));
    }
    {
        // A full cache replaces its oldest; a free entry is taken before any is displaced.
        TicketCache cache(allocator, small(3U, 1U));
        const Ticket a(0x61U, 8U);
        const Ticket b(0x62U, 8U);
        const Ticket c(0x63U, 8U);
        const Ticket d(0x64U, 8U);
        cache.keep(binding_of(1U), a.view());
        cache.keep(binding_of(2U), b.view());
        cache.keep(binding_of(3U), c.view());
        CHECK(cache.counters().displaced == 0U);
        cache.keep(binding_of(4U), d.view());
        CHECK(cache.counters().displaced == 1U);
        CHECK(cache.size() == 3U);
        CHECK(cache.take(binding_of(1U)).empty());
        CHECK(first_byte(cache.take(binding_of(2U)), 0x62U));
        cache.keep(binding_of(5U), a.view()); // the entry 2 left
        CHECK(cache.counters().displaced == 1U);
        CHECK(first_byte(cache.take(binding_of(3U)), 0x63U));
        CHECK(first_byte(cache.take(binding_of(4U)), 0x64U));
        CHECK(first_byte(cache.take(binding_of(5U)), 0x61U));
    }
}

TEST_CASE("Ticket cache: a ticket is kept up to the entry size", "[net][4.2a][ticket-cache]")
{
    CountingAllocator allocator;
    TicketCache cache(allocator, small(2U, 2U));
    const Ticket fits(0x71U, kSmall);
    const Ticket over(0x72U, kSmall + 1U);
    cache.keep(binding_of(1U), over.view());
    cache.keep(binding_of(1U), ConstSpan<u8>{});
    CHECK(cache.size() == 0U);
    CHECK(cache.counters().too_large == 2U);
    cache.keep(binding_of(1U), fits.view());
    CHECK(cache.take(binding_of(1U)).size() == kSmall);
}

TEST_CASE("Ticket cache: no ticket outlives its age", "[net][4.2a][ticket-cache]")
{
    CountingAllocator allocator;
    TicketCache cache(allocator, small(4U, 4U));
    const Ticket a(0x81U, 8U);
    const Ticket b(0x82U, 8U);
    cache.advance(100U);
    cache.keep(binding_of(1U), a.view());
    cache.advance(600U);
    cache.keep(binding_of(1U), b.view());

    // A clock that goes back changes nothing; the age is measured from the keeping.
    cache.advance(50U);
    cache.advance(1099U); // a's age 999
    CHECK(cache.size() == 2U);
    cache.advance(1100U); // a's age 1000
    CHECK(cache.size() == 1U);
    CHECK(cache.counters().expired == 1U);
    cache.advance(1599U);
    CHECK(cache.size() == 1U);
    cache.advance(1600U);
    CHECK(cache.size() == 0U);
    CHECK(cache.counters().expired == 2U);
    CHECK(cache.take(binding_of(1U)).empty());

    // An empty cache's age starts over with its next ticket, and a ticket taken early leaves a younger one its age.
    cache.advance(5000U);
    cache.keep(binding_of(2U), a.view());
    cache.advance(5500U);
    cache.keep(binding_of(3U), b.view());
    CHECK(first_byte(cache.take(binding_of(2U)), 0x81U));
    cache.advance(6000U); // the taken ticket would have been due: nothing is
    CHECK(cache.size() == 1U);
    cache.advance(6499U);
    CHECK(cache.size() == 1U);
    cache.advance(6500U);
    CHECK(cache.size() == 0U);
}

TEST_CASE("Ticket cache: a ticket's bytes do not stay in memory", "[net][4.2a][ticket-cache]")
{
    Retaining allocator;
    {
        TicketCache cache(allocator, small(2U, 1U));
        REQUIRE(cache.ready());
        const Ticket handed(0x91U, kSmall);
        const Ticket displaced(0x92U, kSmall);
        const Ticket replacement(0x93U, kSmall);
        const Ticket expired(0x94U, kSmall);
        const Ticket held(0x95U, kSmall);

        // Handed out: readable until the cache's next call, then gone.
        cache.keep(binding_of(1U), handed.view());
        CHECK(first_byte(cache.take(binding_of(1U)), 0x91U));
        CHECK(allocator.holds(0x91U));
        CHECK(cache.take(binding_of(1U)).empty());
        CHECK_FALSE(allocator.holds(0x91U));

        // Displaced: gone when replaced.
        cache.keep(binding_of(1U), displaced.view());
        CHECK(allocator.holds(0x92U));
        cache.keep(binding_of(1U), replacement.view());
        CHECK_FALSE(allocator.holds(0x92U));
        CHECK(first_byte(cache.take(binding_of(1U)), 0x93U));
        cache.advance(1U); // advance() zeroes what was handed out too
        CHECK_FALSE(allocator.holds(0x93U));

        // Expired: gone when its age passes.
        cache.keep(binding_of(1U), expired.view());
        CHECK(allocator.holds(0x94U));
        cache.advance(1001U);
        CHECK_FALSE(allocator.holds(0x94U));

        // Held: gone on destruction.
        cache.keep(binding_of(1U), held.view());
        CHECK(allocator.holds(0x95U));
    }
    CHECK_FALSE(allocator.holds(0x95U));
    CHECK(allocator.freed() == 2U);
}
