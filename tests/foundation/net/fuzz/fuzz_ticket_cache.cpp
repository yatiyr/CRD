// crd-fuzz-net-ticket-cache: a client's bounded store of TLS session tickets in crd-net (4.2a;
// docs/design/network-foundation.md#ticket-cache) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: four bytes of limits (entries, per binding, ticket size, age), then operations, each an operation byte and the
// parameter bytes it reads (0 past the end): keep a ticket of any size under one of four bindings, take one, advance
// the time by any step, forwards or back.
//
// Oracles. A model of the contract, written apart from the cache, predicts every result: the ticket take() returns,
// byte for byte (the newest of its binding, once), the size, and each counter. The size never passes the entries or a
// binding's share, and nothing allocates after construction.
#include <crd/fuzz/harness.hpp>
#include <crd/net/ticket_cache.hpp>

#include <cstring>
#include <new>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::crypto::TlsBinding;
using crd::fuzz::BudgetAllocator;
using namespace crd::net;

constexpr usize kMaxEntries = 8U;
constexpr usize kMaxBytes = 64U;

struct Input
{
    const u8* data;
    usize size;
    usize at = 0U;

    [[nodiscard]] bool more() const { return at < size; }
    u8 byte() { return (at < size) ? data[at++] : u8{0}; }
};

// The ticket `id` of `size` bytes: its content tells every ticket from every other.
void fill(u8* out, u64 id, usize size)
{
    for (usize index = 0U; index < size; ++index)
    {
        out[index] = static_cast<u8>((id * 197U) + (index * 13U) + 1U);
    }
}

TlsBinding binding_of(u8 index)
{
    TlsBinding binding;
    binding.bytes[0] = static_cast<u8>(index + 1U);
    return binding;
}

struct Model
{
    struct Held
    {
        u64 id = 0U; // 0: free
        u64 kept_ns = 0U;
        usize bytes = 0U;
        u8 binding = 0U;
    };

    Held held[kMaxEntries];
};

class Run
{
public:
    void go(Input& in)
    {
        const u32 entries = (in.byte() % static_cast<u32>(kMaxEntries)) + 1U;
        const u32 per_binding = (in.byte() % entries) + 1U;
        const usize ticket_bytes = (in.byte() % kMaxBytes) + 1U;
        const u64 max_age = u64{in.byte()} + 1U;
        TicketCacheConfig config;
        config.entries = entries;
        config.per_binding = per_binding;
        config.ticket_bytes = ticket_bytes;
        config.max_age_ns = max_age;
        BudgetAllocator allocator;
        TicketCache cache(allocator, config);
        CRD_FUZZ_REQUIRE(cache.ready());
        const usize peak = allocator.peak_bytes();
        m_entries = entries;
        m_max_age = max_age;
        while (in.more())
        {
            const u8 operation = in.byte();
            const u8 binding = static_cast<u8>((operation >> 2U) % 4U);
            switch (operation & 3U)
            {
                case 0U:
                {
                    keep(cache, binding, in.byte() % (kMaxBytes + 8U), ticket_bytes, per_binding);
                    break;
                }
                case 1U:
                {
                    take(cache, binding);
                    break;
                }
                case 2U:
                {
                    advance(cache, u64{in.byte()});
                    break;
                }
                default:
                {
                    cache.advance(m_now / 2U); // a step back is ignored
                    break;
                }
            }
            CRD_FUZZ_REQUIRE(cache.size() == held());
            CRD_FUZZ_REQUIRE(cache.size() <= entries);
            const TicketCacheCounters& counters = cache.counters();
            CRD_FUZZ_REQUIRE(counters.kept == m_kept && counters.taken == m_taken);
            CRD_FUZZ_REQUIRE(counters.too_large == m_too_large && counters.displaced == m_displaced);
            CRD_FUZZ_REQUIRE(counters.expired == m_expired);
        }
        CRD_FUZZ_REQUIRE(allocator.peak_bytes() == peak);
    }

private:
    [[nodiscard]] u32 held() const
    {
        u32 count = 0U;
        for (usize index = 0U; index < m_entries; ++index)
        {
            count += (m_model.held[index].id != 0U) ? 1U : 0U;
        }
        return count;
    }

    void keep(TicketCache& cache, u8 binding, usize size, usize limit, u32 per_binding)
    {
        u8 ticket[kMaxBytes + 8U];
        const u64 id = ++m_next;
        fill(ticket, id, size);
        cache.keep(binding_of(binding), ConstSpan<u8>(ticket, size));
        if (size == 0U || size > limit)
        {
            ++m_too_large;
            return;
        }
        ++m_kept;
        usize oldest = m_entries;
        usize own_oldest = m_entries;
        usize free_entry = m_entries;
        u32 own = 0U;
        for (usize index = 0U; index < m_entries; ++index)
        {
            const Model::Held& held = m_model.held[index];
            if (held.id == 0U)
            {
                free_entry = index;
                continue;
            }
            if (oldest == m_entries || held.id < m_model.held[oldest].id)
            {
                oldest = index;
            }
            if (held.binding == binding)
            {
                ++own;
                if (own_oldest == m_entries || held.id < m_model.held[own_oldest].id)
                {
                    own_oldest = index;
                }
            }
        }
        CRD_FUZZ_REQUIRE(own <= per_binding);
        usize victim = free_entry;
        if (own == per_binding)
        {
            victim = own_oldest;
        }
        else if (free_entry == m_entries)
        {
            victim = oldest;
        }
        if (m_model.held[victim].id != 0U)
        {
            ++m_displaced;
        }
        m_model.held[victim] = Model::Held{id, m_now, size, binding};
    }

    void take(TicketCache& cache, u8 binding)
    {
        const ConstSpan<u8> got = cache.take(binding_of(binding));
        usize newest = m_entries;
        for (usize index = 0U; index < m_entries; ++index)
        {
            const Model::Held& held = m_model.held[index];
            if (held.id != 0U && held.binding == binding && (newest == m_entries || held.id > m_model.held[newest].id))
            {
                newest = index;
            }
        }
        if (newest == m_entries)
        {
            CRD_FUZZ_REQUIRE(got.empty());
            return;
        }
        const Model::Held held = m_model.held[newest];
        u8 expected[kMaxBytes + 8U];
        fill(expected, held.id, held.bytes);
        CRD_FUZZ_REQUIRE(got.size() == held.bytes && std::memcmp(got.data(), expected, held.bytes) == 0);
        m_model.held[newest] = Model::Held{};
        ++m_taken;
    }

    void advance(TicketCache& cache, u64 step)
    {
        m_now += step;
        cache.advance(m_now);
        for (usize index = 0U; index < m_entries; ++index)
        {
            Model::Held& held = m_model.held[index];
            if (held.id != 0U && m_now - held.kept_ns >= m_max_age)
            {
                held = Model::Held{};
                ++m_expired;
            }
        }
    }

    Model m_model;
    usize m_entries = 0U;
    u64 m_max_age = 0U;
    u64 m_now = 0U;
    u64 m_next = 0U;
    u64 m_kept = 0U;
    u64 m_taken = 0U;
    u64 m_too_large = 0U;
    u64 m_displaced = 0U;
    u64 m_expired = 0U;
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Input in{data, size};
    Run run;
    run.go(in);
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Keep two under one binding and one under another, take the newest, the older, then none.
    const u8 plain[] = {0x07U, 0x03U, 0x3FU, 0x20U, 0x00U, 0x10U, 0x00U,
                        0x18U, 0x04U, 0x20U, 0x01U, 0x01U, 0x01U, 0x05U};
    sink.add("plain", plain, sizeof(plain));
    // A binding's share replacing its own oldest, and a full cache replacing the oldest overall.
    const u8 bounded[] = {0x02U, 0x00U, 0x0FU, 0xFFU, 0x00U, 0x08U, 0x00U, 0x08U, 0x04U,
                          0x08U, 0x08U, 0x08U, 0x0CU, 0x08U, 0x01U, 0x05U, 0x09U, 0x0DU};
    sink.add("bounded", bounded, sizeof(bounded));
    // Sizes at and past the limit, and tickets expiring between keeps.
    const u8 aged[] = {0x03U, 0x03U, 0x07U, 0x09U, 0x00U, 0x07U, 0x00U, 0x08U, 0x00U, 0x09U,
                       0x02U, 0x05U, 0x00U, 0x04U, 0x02U, 0x05U, 0x03U, 0x02U, 0x09U, 0x01U};
    sink.add("aged", aged, sizeof(aged));
}
