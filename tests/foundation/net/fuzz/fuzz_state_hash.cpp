// crd-fuzz-net-state-hash: the state-hash exchange of crd-net (NET.3; docs/design/network-foundation.md#state-hashes)
// under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first bytes size the two rings; each three bytes after them are one step of a script: this side records,
// or a report arrives, right or damaged as the input says.
//
// Oracle: an unbounded restatement that remembers every record and report and shares no code with the exchange. It
// decides every verdict, the chain of every record and the divergence named, once.
#include <crd/containers/checksum.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/state_hash.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::net::StateHashExchange;
using crd::net::StateHashReport;
using crd::net::StateHashVerdict;

constexpr usize kMaxRing = 8U;
constexpr usize kMaxScript = 512U;

u64 chain_of(u64 previous, u64 step, u64 hash)
{
    u8 bytes[16];
    for (u32 i = 0U; i < 8U; ++i)
    {
        bytes[i] = static_cast<u8>(step >> (8U * i));
        bytes[8U + i] = static_cast<u8>(hash >> (8U * i));
    }
    return crd::containers::xxh3_64(ConstSpan<u8>(bytes, sizeof(bytes)), previous);
}

// The model: all it has seen, in arrival order.
struct Model
{
    usize local_ring = 1U;
    usize ahead_ring = 1U;
    StateHashReport local[kMaxScript];
    usize local_count = 0U;
    StateHashReport waiting[kMaxScript];
    usize waiting_first = 0U;
    usize waiting_count = 0U;
    bool any_report = false;
    u64 last_report = 0U;
    bool diverged = false;
    bool any_agreed = false;
    u64 agreed_through = 0U;
    u64 diverged_at = 0U;
    u64 dropped = 0U;

    StateHashVerdict compare(const StateHashReport& report)
    {
        // Only the newest `local_ring` records are still held.
        const usize from = (local_count > local_ring) ? (local_count - local_ring) : 0U;
        for (usize i = from; i < local_count; ++i)
        {
            if (local[i].step != report.step)
            {
                continue;
            }
            if (local[i].hash == report.hash && local[i].chain == report.chain)
            {
                any_agreed = true;
                agreed_through = report.step;
                return StateHashVerdict::Agreed;
            }
            diverged = true;
            diverged_at = report.step;
            return StateHashVerdict::Diverged;
        }
        return StateHashVerdict::NotHeld;
    }

    StateHashVerdict on_record(u64 step, u64 hash, StateHashReport& out)
    {
        if (local_count != 0U && step <= local[local_count - 1U].step)
        {
            return StateHashVerdict::Stale;
        }
        const u64 previous = (local_count == 0U) ? 0U : local[local_count - 1U].chain;
        out = StateHashReport{step, hash, chain_of(previous, step, hash)};
        local[local_count++] = out;
        StateHashVerdict verdict = diverged ? StateHashVerdict::AlreadyDiverged : StateHashVerdict::Waiting;
        while (!diverged && waiting_count != 0U && waiting[waiting_first].step <= step)
        {
            verdict = compare(waiting[waiting_first]);
            ++waiting_first;
            --waiting_count;
        }
        return verdict;
    }

    StateHashVerdict on_report(const StateHashReport& report)
    {
        if (diverged)
        {
            return StateHashVerdict::AlreadyDiverged;
        }
        if (any_report && report.step <= last_report)
        {
            return StateHashVerdict::Stale;
        }
        any_report = true;
        last_report = report.step;
        if (local_count != 0U && report.step <= local[local_count - 1U].step)
        {
            return compare(report);
        }
        if (waiting_count == ahead_ring)
        {
            ++waiting_first;
            --waiting_count;
            ++dropped;
        }
        waiting[waiting_first + waiting_count] = report;
        ++waiting_count;
        return StateHashVerdict::Waiting;
    }
};

Model g_model;
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Model& model = g_model;
    model = Model{};
    model.local_ring = (data[0] % kMaxRing) + 1U;
    model.ahead_ring = (data[1] % kMaxRing) + 1U;
    StateHashReport local_store[kMaxRing];
    StateHashReport ahead_store[kMaxRing];
    StateHashExchange exchange(crd::containers::Span<StateHashReport>(local_store, model.local_ring),
                               crd::containers::Span<StateHashReport>(ahead_store, model.ahead_ring));

    // The honest other side's chain, step by step, which damaged reports leave.
    u64 other_chain = 0U;
    u64 other_step = 0U;
    usize steps = 0U;
    bool restarted = false;
    for (usize at = 2U; at + 3U <= size && steps < kMaxScript / 2U; at += 3U, ++steps)
    {
        const u8 action = data[at];
        const u64 step = data[at + 1U];
        const u64 hash = u64{data[at + 2U]} * 0x9E3779B97F4A7C15ULL;
        if ((action & 15U) == 15U && !restarted)
        {
            restarted = true;
            const usize local_ring = model.local_ring;
            const usize ahead_ring = model.ahead_ring;
            model = Model{};
            model.local_ring = local_ring;
            model.ahead_ring = ahead_ring;
            exchange.restart();
            other_chain = 0U;
            other_step = 0U;
            CRD_FUZZ_REQUIRE(!exchange.diverged());
            continue;
        }
        if ((action & 1U) == 0U)
        {
            StateHashReport got;
            got.step = 0xFFFFU;
            StateHashReport want;
            want.step = 0xFFFFU;
            const StateHashVerdict verdict = exchange.record(step, hash, got);
            CRD_FUZZ_REQUIRE(verdict == model.on_record(step, hash, want));
            CRD_FUZZ_REQUIRE(got.step == want.step);
            CRD_FUZZ_REQUIRE(got.hash == want.hash);
            CRD_FUZZ_REQUIRE(got.chain == want.chain);
        }
        else
        {
            // The other side reports, chained honestly from what it reported before; bit 1 damages the report's hash.
            if (step > other_step)
            {
                other_chain = chain_of(other_chain, step, hash);
                other_step = step;
            }
            StateHashReport report{step, hash, other_chain};
            if ((action & 2U) != 0U)
            {
                report.hash ^= 1U;
            }
            CRD_FUZZ_REQUIRE(exchange.on_report(report) == model.on_report(report));
        }
        CRD_FUZZ_REQUIRE(exchange.diverged() == model.diverged);
        CRD_FUZZ_REQUIRE(exchange.dropped() == model.dropped);
        CRD_FUZZ_REQUIRE(exchange.divergence().any_agreed == model.any_agreed);
        if (model.any_agreed)
        {
            CRD_FUZZ_REQUIRE(exchange.divergence().agreed_through == model.agreed_through);
        }
        if (model.diverged)
        {
            CRD_FUZZ_REQUIRE(exchange.divergence().step == model.diverged_at);
        }
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Both rings of four; this side records steps 1 to 3 and the other reports them, with the same hashes: agreement.
    const u8 agree[] = {3U, 3U, 0U, 1U, 7U, 1U, 1U, 7U, 0U, 2U, 8U, 1U, 2U, 8U, 0U, 3U, 9U, 1U, 3U, 9U};
    sink.add("agree", agree, sizeof(agree));
    // The report arrives first, damaged, and a record shows it.
    const u8 damaged[] = {0U, 0U, 3U, 5U, 7U, 0U, 5U, 7U, 1U, 6U, 8U};
    sink.add("damaged", damaged, sizeof(damaged));
    // The rings overflow, then a restart.
    const u8 overflow[] = {0U, 0U, 1U, 9U, 1U, 1U, 10U, 2U, 0U, 3U, 3U, 0U, 4U, 4U, 15U, 0U, 0U, 0U, 1U, 1U};
    sink.add("overflow", overflow, sizeof(overflow));
    // Rings of three and of five, whose indices no mask could stand in for, past their wrap: each held step and the
    // older.
    u8 wrap[2U + (3U * 24U)] = {2U, 4U};
    for (u8 round = 0U; round < 12U; ++round)
    {
        const usize at = 2U + (6U * usize{round});
        wrap[at] = 0U;
        wrap[at + 1U] = static_cast<u8>(round + 1U);
        wrap[at + 2U] = round;
        wrap[at + 3U] = 1U;
        wrap[at + 4U] = static_cast<u8>(round + 1U);
        wrap[at + 5U] = round;
    }
    sink.add("wrap-agree", wrap, sizeof(wrap));
    // Twelve records, then reports of them from the oldest: given up, then held.
    u8 late[2U + (3U * 24U)] = {4U, 2U};
    for (u8 round = 0U; round < 12U; ++round)
    {
        const usize at = 2U + (3U * usize{round});
        late[at] = 0U;
        late[at + 1U] = static_cast<u8>(round + 1U);
        late[at + 2U] = round;
        const usize report = at + 36U;
        late[report] = 1U;
        late[report + 1U] = static_cast<u8>(round + 1U);
        late[report + 2U] = round;
    }
    sink.add("wrap-late", late, sizeof(late));
    // Twelve reports first, overflowing the ring of three that waits, then the records.
    u8 early[2U + (3U * 24U)] = {4U, 2U};
    for (u8 round = 0U; round < 12U; ++round)
    {
        const usize at = 2U + (3U * usize{round});
        early[at] = 1U;
        early[at + 1U] = static_cast<u8>(round + 1U);
        early[at + 2U] = round;
        const usize record = at + 36U;
        early[record] = 0U;
        early[record + 1U] = static_cast<u8>(round + 1U);
        early[record + 2U] = round;
    }
    sink.add("wrap-early", early, sizeof(early));
}
