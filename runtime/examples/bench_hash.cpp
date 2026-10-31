// OWN.1's before/after board for the integer hashing and seed-generator moves onto crd-core (FNV-1a, SplitMix64, the
// MMIX LCG step and the golden-ratio combiner): each case times crd-core's function (`owner`) and the loop the moved
// copies carried (`reference`, written out here as it was) in one process, alternating, so scripts/ab-board.py reads
// both arms from one run. A line is `<case> <arm> <ns per item>`. The owner's loops are the copies', so the board
// records parity rather than a speedup: it shows the move cost nothing.

#include <crd/core/fnv1a.hpp>
#include <crd/core/seed_generators.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using crd::u64;
using crd::u8;
using crd::usize;

namespace
{
volatile u64 g_sink = 0U;

u64 reference_fnv1a_64(const u8* bytes, usize n)
{
    u64 h = 0xCBF29CE484222325ULL;
    for (usize i = 0; i < n; ++i)
    {
        h ^= bytes[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

u64 reference_splitmix64(u64& state)
{
    u64 z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

u64 reference_lcg(u64& state)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
}

u64 reference_combine(u64 h, u64 v)
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
    return h;
}

// Nanoseconds per item of `body(items)`, the median of five timed runs after a warm one, each long enough (~20 ms) that
// the clock's resolution does not show.
template <typename Body> double ns_per_item(usize items, Body body)
{
    body(items);
    usize reps = 1U;
    for (;;)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (usize r = 0; r < reps; ++r)
        {
            body(items);
        }
        const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= 2.0e7)
        {
            break;
        }
        reps *= 2U;
    }
    double runs[5] = {};
    for (double& run : runs)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (usize r = 0; r < reps; ++r)
        {
            body(items);
        }
        run = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
              (static_cast<double>(reps) * static_cast<double>(items));
    }
    for (usize i = 1; i < 5U; ++i) // insertion sort of five
    {
        for (usize j = i; j > 0U && runs[j - 1U] > runs[j]; --j)
        {
            const double t = runs[j];
            runs[j] = runs[j - 1U];
            runs[j - 1U] = t;
        }
    }
    return runs[2];
}

// The arms run in alternating order from case to case, so in-process drift lands on both.
bool g_owner_first = true;

template <typename Owner, typename Reference> void row(const char* name, usize items, Owner owner, Reference reference)
{
    double owner_ns = 0.0;
    double reference_ns = 0.0;
    if (g_owner_first)
    {
        owner_ns = ns_per_item(items, owner);
        reference_ns = ns_per_item(items, reference);
    }
    else
    {
        reference_ns = ns_per_item(items, reference);
        owner_ns = ns_per_item(items, owner);
    }
    g_owner_first = !g_owner_first;
    std::printf("%s owner %.4f\n%s reference %.4f\n", name, owner_ns, name, reference_ns);
}

constexpr usize kTextBytes = static_cast<usize>(1) << 16;
constexpr usize kTextSizes[] = {16U, 256U, kTextBytes};
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 22);
    crd::containers::Array<u8> bytes(&alloc);
    bytes.resize(kTextBytes + 8U); // a start offset of up to 7 bytes still reads kTextBytes
    u64 fill = 0x243F6A8885A308D3ULL;
    for (usize i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = static_cast<u8>(crd::mmix_lcg_next(fill) >> 56U);
    }
    const u8* data = bytes.data();

    std::printf("# OWN.1 hashing and seed generators: crd-core (owner) vs the moved copies (reference), ns per item\n");
    for (const usize n : kTextSizes)
    {
        char name[32];
        (void)std::snprintf(name, sizeof(name), "fnv1a64-%zuB", n);
        // Per byte: hash n bytes from a start the previous digest picks, so no call is dead or hoisted.
        const usize calls = kTextBytes / n;
        row(
            name, calls * n,
            [&](usize)
            {
                u64 h = 0U;
                for (usize c = 0; c < calls; ++c)
                {
                    h ^= crd::fnv1a_64(static_cast<const void*>(data + (h & 7U)), n);
                }
                g_sink = h;
            },
            [&](usize)
            {
                u64 h = 0U;
                for (usize c = 0; c < calls; ++c)
                {
                    h ^= reference_fnv1a_64(data + (h & 7U), n);
                }
                g_sink = h;
            });
    }
    const usize stream = kTextBytes;
    row(
        "splitmix64", stream,
        [](usize items)
        {
            u64 state = 12345U;
            u64 acc = 0U;
            for (usize i = 0; i < items; ++i)
            {
                acc += crd::splitmix64_next(state);
            }
            g_sink = acc;
        },
        [](usize items)
        {
            u64 state = 12345U;
            u64 acc = 0U;
            for (usize i = 0; i < items; ++i)
            {
                acc += reference_splitmix64(state);
            }
            g_sink = acc;
        });
    row(
        "mmix-lcg", stream,
        [](usize items)
        {
            u64 state = 12345U;
            u64 acc = 0U;
            for (usize i = 0; i < items; ++i)
            {
                acc ^= crd::mmix_lcg_next(state) >> 11U;
            }
            g_sink = acc;
        },
        [](usize items)
        {
            u64 state = 12345U;
            u64 acc = 0U;
            for (usize i = 0; i < items; ++i)
            {
                acc ^= reference_lcg(state) >> 11U;
            }
            g_sink = acc;
        });
    row(
        "hash-combine", stream,
        [](usize items)
        {
            u64 h = 0U;
            for (usize i = 0; i < items; ++i)
            {
                h = crd::hash_combine(h, i);
            }
            g_sink = h;
        },
        [](usize items)
        {
            u64 h = 0U;
            for (usize i = 0; i < items; ++i)
            {
                h = reference_combine(h, i);
            }
            g_sink = h;
        });
    return 0;
}
