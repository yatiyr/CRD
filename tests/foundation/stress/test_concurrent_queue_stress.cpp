// MPMC stress for crd::containers::ConcurrentQueue (detour D-002 v3).
//
// P producers + C consumers (P == C == workers/2). Each producer pushes a
// fixed run of unique tokens, spinning on try_push when the bounded queue is
// full. Consumers loop try_pop until every token has been consumed, spinning
// when empty. Invariants checked by the oracle (main thread, quiescent):
//   - exactly `total` items consumed;
//   - every (producer, seq) token seen exactly once (a double-pop or a lost
//     token shows up here);
//   - XOR checksum of consumed tokens == XOR of produced tokens.
//
// Lane: RunMode::Threads ONLY (TSan-on-Linux can instrument it). The Fibers lane
// was removed: the producer/consumer loops busy-spin on try_push/try_pop with
// std::this_thread::yield(), which yields the OS thread but NOT the fiber — there
// is no cooperative fiber-yield in the public jobs API. With more spinning
// producer fibers than scheduler worker threads (e.g. a 2-core CI runner: 1–2
// runners vs 4 producer jobs that fill the bounded queue and then spin forever),
// the consumer fibers never get scheduled → deadlock. A pure lock-free data
// structure does not need fiber-shaped coverage; the Threads lane is the
// normative one and is already TSan-instrumented.
//
// `seen` is a frozen Array for the parallel phase (debug-only guard, v2): each
// token's slot is written by exactly the consumer that popped it.

#include "stress_harness.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/concurrent_queue.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

constexpr u32 kCapacityPow2 = 1024U; // small enough that producers actually hit "full"
constexpr u32 kWorkers      = 8U;    // 4 producers + 4 consumers — independent of the host core count
constexpr u32 kProducers    = kWorkers / 2U; // consumers = workers - producers
constexpr u32 kSeqBits      = 40U;           // token = (producer_idx << 40) | seq

constexpr u64 make_token(u32 producer_idx, u64 seq) noexcept
{
    return (static_cast<u64>(producer_idx) << kSeqBits) | seq;
}

void drive(crd::stress::RunMode mode)
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "cq-stress"};

    crd::stress::Config cfg = crd::stress::bounded(mode);
    cfg.num_workers = kWorkers;
    const u64 per_producer = cfg.iterations_per_round; // tokens each producer pushes per round
    const u64 total_per_round = per_producer * kProducers;

    crd::containers::ConcurrentQueue<u64> queue(kCapacityPow2, &alloc);

    crd::containers::Array<u8> seen(&alloc);
    seen.resize(static_cast<usize>(total_per_round), u8{0});

    std::atomic<u64> consumed{0};
    std::atomic<u64> produced_xor{0};
    std::atomic<u64> consumed_xor{0};
    crd::stress::FailSink sink;

    const auto work = [&](u32 worker, u64 /*iterations*/, crd::stress::Rng& /*rng*/)
    {
        if (worker < kProducers)
        {
            const u32 producer_idx = worker;
            u64 local_xor = 0;
            for (u64 seq = 0; seq < per_producer; ++seq)
            {
                const u64 token = make_token(producer_idx, seq);
                local_xor ^= token;
                while (!queue.try_push(token))
                {
                    std::this_thread::yield(); // bounded queue full — let a consumer drain
                }
            }
            produced_xor.fetch_xor(local_xor, std::memory_order_relaxed);
        }
        else
        {
            u64 local_xor = 0;
            for (;;)
            {
                u64 token = 0;
                if (queue.try_pop(token))
                {
                    local_xor ^= token;
                    const u32 producer_idx = static_cast<u32>(token >> kSeqBits);
                    const u64 seq = token & ((u64{1} << kSeqBits) - 1U);
                    if (producer_idx >= kProducers || seq >= per_producer)
                    {
                        sink.fail("popped a malformed token", seq, worker);
                    }
                    else
                    {
                        const usize idx = static_cast<usize>(producer_idx) * static_cast<usize>(per_producer) +
                                          static_cast<usize>(seq);
                        ++seen[idx]; // this token popped by exactly one consumer → no race on this byte
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
                else if (consumed.load(std::memory_order_acquire) >= total_per_round)
                {
                    break; // everything has been consumed; queue is (or will be) empty
                }
                else
                {
                    std::this_thread::yield();
                }
            }
            consumed_xor.fetch_xor(local_xor, std::memory_order_relaxed);
        }
    };

    const auto oracle = [&](u32 /*round*/)
    {
        if (consumed.load() == 0U)
        {
            // Either before any work, or just-reset after the previous round's verify.
            CHECK(queue.empty());
            return;
        }
        if (consumed.load() != total_per_round)
        {
            sink.fail("consumed count != produced count", consumed.load(), 0U);
            return;
        }
        if (produced_xor.load() != consumed_xor.load())
        {
            sink.fail("produced XOR checksum != consumed XOR checksum", consumed_xor.load(), 0U);
            return;
        }
        for (usize i = 0; i < seen.size(); ++i)
        {
            if (seen[i] != u8{1})
            {
                sink.fail("token consumed != once", static_cast<u64>(i), 0U);
                return;
            }
        }
        CHECK(queue.empty());

        // Reset for the next round.
        consumed.store(0, std::memory_order_relaxed);
        produced_xor.store(0, std::memory_order_relaxed);
        consumed_xor.store(0, std::memory_order_relaxed);
        for (usize i = 0; i < seen.size(); ++i)
        {
            seen[i] = u8{0};
        }
    };

    {
        crd::containers::FrozenView<u8> seen_guard(seen); // no structural mutation of `seen` during the parallel pass
        crd::stress::run(cfg, work, oracle);
    }
    CRD_STRESS_ORACLE_OK(sink);
}
} // namespace

TEST_CASE("ConcurrentQueue stress -- MPMC token round-trip", "[stress][containers]")
{
    drive(crd::stress::RunMode::Threads); // Threads-only — see the header comment on why there is no fibers lane.
}

TEST_CASE("ConcurrentQueue stress -- MPMC soak", "[stress][containers][.soak]")
{
    drive(crd::stress::RunMode::Threads);
}
