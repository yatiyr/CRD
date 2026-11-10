// crd-fuzz-net-quic-pmtu: QUIC's path MTU search of crd-net (NET-PATH; docs/design/network-foundation.md#pmtu) under
// the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: operations of two bytes, an operation (its low three bits the kind, the rest a parameter) and a parameter,
// on one search: probes asked for (busy or not) and sent or not, probes acknowledged or lost (the ones in flight, or
// any size: stale, spurious and forged settlements), other packets of any size and number acknowledged or lost, loss
// detections ended, ceilings lowered, time and new paths of any base, ceiling and discovery.
//
// Oracles. The size stays within the floor and the ceiling, and the ceiling never rises. Only an acknowledgement
// raises the size, and only to the size it proves; only a black hole or a lower ceiling lowers it, a black hole to the
// floor. A probe names a size above the size and within the ceiling, never while one is in flight, and none without
// discovery; nothing is probed or found in Disabled. Counters only rise.
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_pmtu.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::net;

struct World
{
    QuicPathMtu pmtu;
    u32 ceiling = kQuicPmtuFloor;
    u32 flight = 0U; // the probe sent and not settled, 0 for none
    u64 number = 0U; // the next packet number
    u64 now = 0U;
    QuicPmtuCounters counters;
};

[[nodiscard]] u32 size_of(u8 p) noexcept
{
    return kQuicPmtuFloor + (u32{p} * 37U); // 1200 to 10635
}

void check(const World& w)
{
    const QuicPathMtu& pmtu = w.pmtu;
    CRD_FUZZ_REQUIRE(pmtu.size() >= kQuicPmtuFloor && pmtu.size() <= pmtu.ceiling());
    CRD_FUZZ_REQUIRE(pmtu.ceiling() <= w.ceiling);
    const QuicPmtuCounters& c = pmtu.counters();
    CRD_FUZZ_REQUIRE(c.probes_sent >= w.counters.probes_sent && c.probes_acked >= w.counters.probes_acked &&
                     c.probes_lost >= w.counters.probes_lost && c.black_holes >= w.counters.black_holes);
}

void settle_probe(World& w, u32 bytes, bool acked)
{
    const u32 before = w.pmtu.size();
    w.flight = (bytes == w.flight) ? 0U : w.flight;
    if (acked)
    {
        w.pmtu.on_probe_acked(bytes, w.now);
        // An acknowledgement raises the size to what it proves, or leaves it.
        CRD_FUZZ_REQUIRE(w.pmtu.size() == before || (w.pmtu.size() == bytes && bytes > before));
    }
    else
    {
        w.pmtu.on_probe_lost(bytes, w.now);
        CRD_FUZZ_REQUIRE(w.pmtu.size() == before); // a lost probe lowers nothing
    }
}

void run(World& w, u8 op, u8 p)
{
    const u8 q = static_cast<u8>(op >> 3U);
    const u32 before = w.pmtu.size();
    const QuicPmtuState state = w.pmtu.state();
    switch (op & 7U)
    {
        case 0U:
        {
            const u32 probe = w.pmtu.probe(w.now, (q & 1U) != 0U);
            CRD_FUZZ_REQUIRE(w.pmtu.size() == before);
            if (probe == 0U)
            {
                break;
            }
            CRD_FUZZ_REQUIRE(w.flight == 0U && w.pmtu.state() != QuicPmtuState::Disabled);
            CRD_FUZZ_REQUIRE(probe > w.pmtu.size() && probe <= w.pmtu.ceiling());
            if ((q & 2U) != 0U)
            {
                w.pmtu.on_probe_sent(probe);
                w.flight = probe;
            }
            break;
        }
        case 1U:
            settle_probe(w, ((q & 1U) != 0U && w.flight != 0U) ? w.flight : size_of(p), true);
            break;
        case 2U:
            settle_probe(w, ((q & 1U) != 0U && w.flight != 0U) ? w.flight : size_of(p), false);
            break;
        case 3U:
        case 4U:
        {
            // A packet sent `q` numbers back (or the next), of any size.
            const u64 number = (w.number > q) ? w.number - q : w.number;
            w.number += (q == 0U) ? 1U : 0U;
            const u32 bytes = ((p & 1U) != 0U) ? kQuicPmtuFloor - (p >> 1U) : size_of(p);
            if ((op & 7U) == 3U)
            {
                w.pmtu.on_acked(number, bytes);
            }
            else
            {
                w.pmtu.on_lost(number, bytes);
            }
            CRD_FUZZ_REQUIRE(w.pmtu.size() == before && w.pmtu.state() == state);
            break;
        }
        case 5U:
        {
            const bool black = w.pmtu.end_detection(w.now);
            if (black)
            {
                CRD_FUZZ_REQUIRE(w.pmtu.size() == kQuicPmtuFloor && before > kQuicPmtuFloor);
                CRD_FUZZ_REQUIRE(state == QuicPmtuState::Searching || state == QuicPmtuState::SearchComplete);
                CRD_FUZZ_REQUIRE(w.pmtu.counters().black_holes == w.counters.black_holes + 1U);
            }
            else
            {
                CRD_FUZZ_REQUIRE(w.pmtu.size() == before && w.pmtu.state() == state);
            }
            break;
        }
        case 6U:
        {
            const u32 bound = size_of(p);
            w.pmtu.limit(bound);
            w.ceiling = w.pmtu.ceiling();
            CRD_FUZZ_REQUIRE(w.pmtu.size() == ((before < bound) ? before : bound));
            break;
        }
        default:
            if ((q & 16U) != 0U)
            {
                w = World{};
                const u32 ceiling = size_of(p);
                w.pmtu.start(((q & 1U) != 0U) ? kQuicIpv6BaseDatagram : size_of(static_cast<u8>(p >> (q & 7U))),
                             ceiling, (q & 2U) == 0U);
                w.ceiling = w.pmtu.ceiling();
                CRD_FUZZ_REQUIRE(w.ceiling == ceiling);
                CRD_FUZZ_REQUIRE((w.pmtu.state() == QuicPmtuState::Disabled) ==
                                 ((q & 2U) != 0U || ceiling == kQuicPmtuFloor));
                CRD_FUZZ_REQUIRE(w.pmtu.state() == QuicPmtuState::Disabled || w.pmtu.size() == kQuicPmtuFloor);
            }
            else
            {
                w.now += (u64{p} << (q & 15U)) * 1000000U; // up to hours: the raise timer passes
            }
            break;
    }
    if (w.pmtu.state() == QuicPmtuState::Disabled)
    {
        CRD_FUZZ_REQUIRE(w.pmtu.probe(w.now, true) == 0U);
    }
    check(w);
    w.counters = w.pmtu.counters();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    World w;
    check(w);
    for (usize at = 0U; at + 1U < size; at += 2U)
    {
        run(w, data[at], data[at + 1U]);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    usize at = 0U;
    const auto put = [&](u8* into, u8 op, u8 p)
    {
        into[at++] = op;
        into[at++] = p;
    };
    // A search on a 1500-byte ceiling: probes asked and sent, the ceiling lost three times, halving acknowledged.
    u8 search[2U * 16U];
    put(search, 0xA7U, 0x08U); // start: base 1200, ceiling 1496, discovery
    for (u8 i = 0U; i < 3U; ++i)
    {
        put(search, 0x10U, 0x00U); // probe, sent
        put(search, 0x0AU, 0x00U); // the one in flight lost
    }
    put(search, 0x10U, 0x00U);
    put(search, 0x09U, 0x00U); // acknowledged
    put(search, 0x10U, 0x00U);
    put(search, 0x09U, 0x00U);
    put(search, 0x7FU, 0xFFU); // time: past the raise timer
    put(search, 0x18U, 0x00U); // probe asked busy
    put(search, 0x0EU, 0x04U); // the ceiling lowered
    put(search, 0x00U, 0x00U);
    put(search, 0x05U, 0x00U);
    sink.add("search-and-limit", search, at);
    // A black hole after a search: three detections of large losses with no large acknowledgement.
    u8 hole[2U * 14U];
    at = 0U;
    put(hole, 0x8FU, 0x0AU); // start: IPv6 base, ceiling 1570, discovery
    put(hole, 0x10U, 0x00U);
    put(hole, 0x09U, 0x00U); // the base
    put(hole, 0x10U, 0x00U);
    put(hole, 0x09U, 0x00U); // the ceiling
    put(hole, 0x03U, 0x08U); // the next packet, 1496 bytes, acknowledged
    for (u8 i = 0U; i < 3U; ++i)
    {
        put(hole, 0x04U, 0x08U); // the next lost
        put(hole, 0x05U, 0x00U); // a detection ends
    }
    put(hole, 0x10U, 0x00U);
    put(hole, 0x0CU, 0x01U); // an older packet of 1200 lost
    sink.add("black-hole", hole, at);
    // No discovery: nothing probes, nothing finds a black hole.
    u8 off[2U * 6U];
    at = 0U;
    put(off, 0x97U, 0x0AU); // start without discovery, base 1274
    put(off, 0xB7U, 0x0AU); // again, base 1200
    put(off, 0x10U, 0x00U);
    put(off, 0x04U, 0x08U);
    put(off, 0x05U, 0x00U);
    put(off, 0x0EU, 0x00U); // the ceiling to the floor
    sink.add("no-discovery", off, at);
}
