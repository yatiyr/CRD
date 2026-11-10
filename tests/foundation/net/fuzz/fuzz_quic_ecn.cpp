// crd-fuzz-net-quic-ecn: QUIC's ECN validation of crd-net (NET-PATH; docs/design/network-foundation.md#ecn) under the
// bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: operations of two bytes, an operation (its low three bits the kind, the rest a parameter) and a parameter,
// on one validator: packets marked as marking() says in any space, time, losses of any codepoint, ACK frames with
// honest, absorbed, inflated, falling, short, re-marked or arbitrary counts (present or not, raising the largest or
// not, newly acknowledging any marked packets), new paths with any codepoint, and marking() asked alone.
//
// Oracles, against a model of the packets sent marked per space and of the counts last accepted. marking() names the
// path's codepoint only while testing or validated, and Not-ECT otherwise; testing marks at most
// kQuicEcnTestingPackets packets. A frame that raises the largest on a live path is judged by the restated rule
// (counts missing, falling, above what was sent, short of what was acknowledged, in that order): a failure is that
// failure, for good; a pass records the counts and validates the path when it acknowledged marked packets. A frame
// that does not raise the largest, or reaches an Off or failed path, changes nothing. A congestion signal is a CE
// rise on a validated path. Reported counts never fall and never pass what was sent.
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_ecn.hpp>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::net;

constexpr u64 kMs = 1000000U;
constexpr u64 kTestingNs = 300U * kMs;

struct World
{
    QuicEcnValidator ecn;
    Ecn codepoint = Ecn::NotEct;
    EcnCounts sent[kQuicSpaces];
    EcnCounts reported[kQuicSpaces];
    u64 testing_marks = 0U; // packets marked while testing
    u64 now = 0U;
};

[[nodiscard]] bool live(QuicEcnState state)
{
    return state != QuicEcnState::Off && state != QuicEcnState::Failed;
}

void check(const World& w)
{
    const QuicEcnState state = w.ecn.state();
    CRD_FUZZ_REQUIRE((state == QuicEcnState::Failed) == (w.ecn.failure() != QuicEcnFailure::None));
    CRD_FUZZ_REQUIRE((state == QuicEcnState::Off) == (w.codepoint != Ecn::Ect0 && w.codepoint != Ecn::Ect1));
    CRD_FUZZ_REQUIRE(w.testing_marks <= kQuicEcnTestingPackets);
    for (usize index = 0U; index < kQuicSpaces; ++index)
    {
        const EcnCounts& got = w.ecn.reported(static_cast<QuicSpace>(index));
        const EcnCounts& want = w.reported[index];
        CRD_FUZZ_REQUIRE(got.ect0 == want.ect0 && got.ect1 == want.ect1 && got.ce == want.ce);
        const EcnCounts& sent = w.sent[index];
        CRD_FUZZ_REQUIRE(got.ect0 <= sent.ect0 && got.ect1 <= sent.ect1 &&
                         got.ect0 + got.ect1 + got.ce <= sent.ect0 + sent.ect1);
    }
}

void op_send(World& w, QuicSpace space, u8 p)
{
    for (u8 packet = 0U; packet <= (p & 7U); ++packet)
    {
        const QuicEcnState before = w.ecn.state();
        const Ecn mark = w.ecn.marking(w.now, kTestingNs);
        const QuicEcnState state = w.ecn.state();
        // Asking may end the testing period, which never validates or revives anything.
        CRD_FUZZ_REQUIRE(state == before || (before == QuicEcnState::Testing &&
                                             (state == QuicEcnState::Unknown || state == QuicEcnState::Failed)));
        const bool marks = state == QuicEcnState::Testing || state == QuicEcnState::Capable;
        CRD_FUZZ_REQUIRE(mark == (marks ? w.codepoint : Ecn::NotEct));
        w.ecn.on_sent(space, mark, w.now);
        EcnCounts& sent = w.sent[quic_space_index(space)];
        sent.ect0 += (mark == Ecn::Ect0) ? 1U : 0U;
        sent.ect1 += (mark == Ecn::Ect1) ? 1U : 0U;
        w.testing_marks += (state == QuicEcnState::Testing && mark != Ecn::NotEct) ? 1U : 0U;
        CRD_FUZZ_REQUIRE(w.testing_marks < kQuicEcnTestingPackets || w.ecn.state() != QuicEcnState::Testing);
    }
}

void op_ack(World& w, QuicSpace space, u8 q, u8 p)
{
    const usize index = quic_space_index(space);
    QuicAckOutcome outcome;
    outcome.ect0_packets = p & 7U;
    outcome.ect1_packets = (p >> 3U) & 3U;
    outcome.acked_packets = outcome.ect0_packets + outcome.ect1_packets;
    outcome.advanced = (p & 0x20U) != 0U;
    QuicAck ack;
    ack.ecn = (p & 0x40U) == 0U;
    const EcnCounts& was = w.reported[index];
    const u64 n0 = outcome.ect0_packets;
    const u64 n1 = outcome.ect1_packets;
    ack.ect0 = was.ect0;
    ack.ect1 = was.ect1;
    ack.ce = was.ce + (((p & 0x80U) != 0U) ? 1U : 0U);
    switch ((q >> 2U) & 7U)
    {
        case 0U: // honest
            ack.ect0 += n0;
            ack.ect1 += n1;
            break;
        case 1U: // every one marked CE on the way
            ack.ce += n0 + n1;
            break;
        case 2U: // inflated
            ack.ect0 += n0 + 1U;
            break;
        case 3U: // falling
            ack.ect0 = (ack.ect0 > 0U) ? ack.ect0 - 1U : 0U;
            ack.ce = (ack.ce > 1U) ? ack.ce - 2U : 0U;
            break;
        case 4U: // short
            ack.ect0 += n0 / 2U;
            break;
        case 5U: // re-marked ECT(1)
            ack.ect1 += n0 + n1;
            break;
        default: // arbitrary
            ack.ect0 = p & 0x0FU;
            ack.ect1 = q & 3U;
            ack.ce = p >> 4U;
            break;
    }
    const u64 marked = n0 + n1;
    const QuicEcnState before = w.ecn.state();
    const bool signal = w.ecn.on_ack(space, ack, outcome);
    const QuicEcnState state = w.ecn.state();
    if (!live(before) || !outcome.advanced)
    {
        CRD_FUZZ_REQUIRE(!signal && state == before);
        return;
    }
    const EcnCounts& sent = w.sent[index];
    QuicEcnFailure want = QuicEcnFailure::None;
    if (!ack.ecn)
    {
        want = (marked != 0U) ? QuicEcnFailure::Missing : QuicEcnFailure::None;
    }
    else if (ack.ect0 < was.ect0 || ack.ect1 < was.ect1 || ack.ce < was.ce)
    {
        want = QuicEcnFailure::Decreased;
    }
    else if (ack.ect0 > sent.ect0 || ack.ect1 > sent.ect1 || ack.ect0 + ack.ect1 + ack.ce > sent.ect0 + sent.ect1)
    {
        want = QuicEcnFailure::Exceeded;
    }
    else if (ack.ect0 - was.ect0 + ack.ce - was.ce < n0 || ack.ect1 - was.ect1 + ack.ce - was.ce < n1)
    {
        want = QuicEcnFailure::Short;
    }
    if (want != QuicEcnFailure::None)
    {
        CRD_FUZZ_REQUIRE(state == QuicEcnState::Failed && w.ecn.failure() == want && !signal);
        return;
    }
    const bool rose = ack.ecn && ack.ce > was.ce;
    if (ack.ecn)
    {
        w.reported[index] = EcnCounts{ack.ect0, ack.ect1, ack.ce};
    }
    const QuicEcnState expected = (ack.ecn && marked != 0U) ? QuicEcnState::Capable : before;
    CRD_FUZZ_REQUIRE(state == expected);
    CRD_FUZZ_REQUIRE(signal == (rose && state == QuicEcnState::Capable));
}

void run(World& w, u8 op, u8 p)
{
    const u8 q = static_cast<u8>(op >> 3U);
    const QuicSpace space = static_cast<QuicSpace>(q % kQuicSpaces);
    switch (op & 7U)
    {
        case 0U:
            op_send(w, space, p);
            break;
        case 1U:
            w.now += u64{p} * kMs;
            break;
        case 2U:
        {
            const QuicEcnState before = w.ecn.state();
            w.ecn.on_gone(static_cast<Ecn>(p & 3U));
            // A loss only ever fails an unvalidated path.
            CRD_FUZZ_REQUIRE(w.ecn.state() == before ||
                             (before == QuicEcnState::Unknown && w.ecn.failure() == QuicEcnFailure::Lost));
            break;
        }
        case 3U:
            op_ack(w, space, q, p);
            break;
        case 4U:
            w = World{};
            w.codepoint = static_cast<Ecn>(p & 3U);
            w.ecn.start(w.codepoint);
            break;
        default:
        {
            const QuicEcnState before = w.ecn.state();
            static_cast<void>(w.ecn.marking(w.now, kTestingNs));
            CRD_FUZZ_REQUIRE(w.ecn.state() == before || before == QuicEcnState::Testing);
            break;
        }
    }
    check(w);
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
    // An ECT(0) path validated by honest counts in the Application space, then CE rising.
    u8 honest[2U * 8U];
    put(honest, 0x04U, 0x02U); // start ECT(0)
    put(honest, 0x10U, 0x05U); // six Application packets
    put(honest, 0x13U, 0x23U); // three acknowledged, honest
    put(honest, 0x10U, 0x07U); // eight more
    put(honest, 0x13U, 0xA2U); // two more and one CE
    put(honest, 0x2BU, 0x22U); // two more, every one CE on the way (mode 1)
    put(honest, 0x01U, 0x40U); // time
    put(honest, 0x13U, 0x21U);
    sink.add("validated-then-ce", honest, at);
    // A path whose marks are all lost: testing ends by its packets, then every one goes.
    u8 lost[2U * 14U];
    at = 0U;
    put(lost, 0x04U, 0x02U);
    put(lost, 0x00U, 0x07U);
    put(lost, 0x00U, 0x01U);
    for (u8 i = 0U; i < 10U; ++i)
    {
        put(lost, 0x02U, 0x02U);
    }
    put(lost, 0x03U, 0x21U);
    sink.add("all-marks-lost", lost, at);
    // ECT(1) re-marked, then a time-ended testing period on a new path, and acknowledgements arriving late.
    u8 remark[2U * 8U];
    at = 0U;
    put(remark, 0x04U, 0x01U);
    put(remark, 0x08U, 0x03U);
    put(remark, 0x53U, 0x22U); // Handshake space, mode 2: inflated
    put(remark, 0x04U, 0x02U);
    put(remark, 0x00U, 0x00U);
    put(remark, 0x01U, 0xFFU);
    put(remark, 0x01U, 0x2DU);
    put(remark, 0x05U, 0x00U);
    sink.add("remark-and-time", remark, at);
}
