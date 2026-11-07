// crd-net -- frames, admission stages S6 and S7, quotas and reassembly (NET.3;
// docs/design/network-foundation.md#frames): known bytes of every header form, each refusal at its boundary and in the
// stages' order, what a refusal spends and reserves, every split of a stream, a hostile datagram path, and shared
// quotas under threads.

#include "admission_fixture.hpp"

#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/net/reassembly.hpp>
#include <crd/net/wire/session_control.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <generated/wire_test.hpp>
#include <initializer_list>
#include <thread>

using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::crypto::AuthorizedSchema;
using crd::crypto::Capability;
using crd::crypto::Delivery;
using crd::crypto::ScopeKind;
using crd::memory::IAllocator;
using crd::memory::TlsfAllocator;
using crd::net::Admission;
using crd::net::admit_action;
using crd::net::admit_verdict_name;
using crd::net::AdmitAction;
using crd::net::AdmittedFrame;
using crd::net::AdmitVerdict;
using crd::net::ChannelState;
using crd::net::DatagramAssembler;
using crd::net::DatagramCursor;
using crd::net::fragment_count;
using crd::net::fragment_shape_holds;
using crd::net::FragmentSlot;
using crd::net::FrameHeader;
using crd::net::FrameRead;
using crd::net::FrameShape;
using crd::net::FrameStep;
using crd::net::gate_after;
using crd::net::gate_cost_ns;
using crd::net::gate_open_at;
using crd::net::GateRule;
using crd::net::Gauge;
using crd::net::HeaderForm;
using crd::net::kAdmitVerdictCount;
using crd::net::kMaxFragments;
using crd::net::kMaxFrameHeaderBytes;
using crd::net::kMaxSessionSchemas;
using crd::net::kNanosPerSecond;
using crd::net::kNeverNs;
using crd::net::PrincipalQuota;
using crd::net::priority_limit;
using crd::net::QuotaLimits;
using crd::net::RateGate;
using crd::net::read_frame_number;
using crd::net::read_frame_tail;
using crd::net::read_route;
using crd::net::Route;
using crd::net::RouteForm;
using crd::net::SendBudget;
using crd::net::SessionSchema;
using crd::net::SharedRateGate;
using crd::net::StreamAssembler;
using crd::net::write_fragment;
using crd::net::write_frame;
using crd::net::write_frame_header;
using crd::net::write_route;
using crd::net::test::authorized_for;
using crd::net::test::Buffers;
using crd::net::test::capability_for;
using crd::net::wire::ChannelClass;
using crd::net::wire::ChannelDesc;
using crd::net::wire::MessageDesc;
using crd::net::wire::WireError;
using crd::net::wire::WireSchema;

namespace control = crd::net::wire::session_control;
namespace test_schema = crd::net::wire::wire_test;

namespace
{
constexpr u64 kSecond = kNanosPerSecond;

// A schema whose bounds are the tests' to choose; admission reads nothing else of it.
constexpr ChannelDesc kChannels[] = {
    {"control", ChannelClass::Control, true, true, false, 0U, 1000U, 10U, 10000U, 0U, false},
    {"state", ChannelClass::State, false, false, true, 3U, 2000U, 100U, 1000000U, 0U, false},
    {"media", ChannelClass::Media, false, false, false, 5U, 100000U, 1000U, 100000000U, 0U, false},
    {"bulk", ChannelClass::Bulk, true, false, false, 4U, 100000U, 1000U, 100000000U, 0U, false},
    {"silent", ChannelClass::Event, true, false, false, 2U, 1000U, 0U, 1000U, 0U, false},
    {"voiceless", ChannelClass::Event, true, false, false, 2U, 1000U, 1000U, 0U, 0U, false},
    {"narrow", ChannelClass::Event, true, false, false, 2U, 1000U, 1000U, 100U, 0U, false},
};

constexpr MessageDesc kMessages[] = {
    {"Hello", 1U, 0U, 0U, 1ULL, 2U, 100U},     {"Late", 2U, 0U, 1U, 1ULL, 0U, 100U},
    {"Snapshot", 3U, 1U, 0U, 2ULL, 0U, 2000U}, {"Voice", 4U, 2U, 0U, 4ULL, 0U, 100000U},
    {"Blob", 5U, 3U, 0U, 8ULL, 0U, 100000U},   {"Quiet", 6U, 4U, 0U, 1ULL, 0U, 1000U},
    {"Mute", 7U, 5U, 0U, 1ULL, 0U, 1000U},     {"Pinch", 8U, 6U, 0U, 1ULL, 0U, 1000U},
    {"Tone", 9U, 2U, 0U, 4ULL, 0U, 100000U},   {"Wider", 300U, 0U, 0U, 16ULL, 0U, 100U},
};

constexpr WireSchema kGame{
    "game",
    {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU, 0x01U},
    static_cast<u8>(ScopeKind::World),
    3U,
    1U,
    31ULL,
    2ULL,
    kChannels,
    kMessages,
};

constexpr ChannelDesc kTransportChannels[] = {
    {"control", ChannelClass::Control, true, true, false, 0U, 1000U, 1000U, 100000U, 0U, false},
    {"event", ChannelClass::Event, true, false, false, 1U, 1000U, 1000U, 100000U, 0U, false},
};

constexpr MessageDesc kTransportMessages[] = {
    {"Ping", 1U, 0U, 0U, 0ULL, 0U, 100U},
    {"OnEvent", 2U, 1U, 0U, 0ULL, 0U, 100U},
    {"Claims", 3U, 0U, 0U, 1ULL, 0U, 100U},
};

constexpr WireSchema kTransport{
    "transport",
    {0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U},
    0U,
    1U,
    0U,
    0ULL,
    0ULL,
    kTransportChannels,
    kTransportMessages,
};

// Channels of kGame by their flat numbers, the transport schema being bound after it.
constexpr u32 kControl = 0U;
constexpr u32 kState = 1U;
constexpr u32 kMedia = 2U;
constexpr u32 kBulk = 3U;
constexpr u32 kSilent = 4U;
constexpr u32 kVoiceless = 5U;
constexpr u32 kNarrow = 6U;

// A little credit: each gate takes a few messages at once, and the tests' narrow gates close after one.
constexpr u64 kCredit = kSecond / 100U;

// One session over kGame, with room to be surprised.
struct Session
{
    explicit Session(const QuotaLimits& limits = QuotaLimits{}) : quota(limits), admission(quota, buffers, channels)
    {
        authorized = authorized_for(kGame);
        capability = capability_for(authorized);
        const SessionSchema schemas[] = {{&kGame, &authorized}, {&kTransport, nullptr}};
        REQUIRE(admission.bind(schemas));
        REQUIRE(admission.announce(ConstSpan<u8>(kGame.id, sizeof(kGame.id)), kGame.major, kGame.minor));
        admission.set_capability(&capability);
    }

    Buffers buffers;
    PrincipalQuota quota;
    ChannelState channels[16];
    Admission admission;
    AuthorizedSchema authorized;
    Capability capability;
};

// A Sized frame of `message` with `length` bytes of body following the pattern, appended to `out` at `at`.
usize put_frame(Span<u8> out, usize at, const MessageDesc& message, u64 object, u32 length, u8 fill = 0x5AU)
{
    FrameHeader header;
    header.message = message.id;
    header.length = length;
    header.object = object;
    const usize used =
        write_frame_header(HeaderForm::Sized, FrameShape{message.operation != 0U, false}, header, out.subspan(at));
    REQUIRE(used != 0U);
    std::memset(out.data() + at + used, fill, length);
    return at + used + length;
}

// A stream carrying `channel`'s preamble.
usize put_preamble(Span<u8> out, u32 channel)
{
    const usize used = write_route(Route{channel, RouteForm::Whole}, out);
    REQUIRE(used != 0U);
    return used;
}

// Opens a stream on `channel` in `assembler`.
void open(StreamAssembler& assembler, u32 channel, u64 now = 0U)
{
    u8 route[8];
    const usize used = put_preamble(route, channel);
    usize at = 0U;
    while (at < used)
    {
        const FrameStep step = assembler.feed(ConstSpan<u8>(route + at, used - at), Delivery::Established, now);
        REQUIRE(step.action == AdmitAction::Continue);
        at += step.consumed;
    }
}

// One datagram holding one whole message of `message`.
usize put_whole(Span<u8> out, u32 channel, const MessageDesc& message, u64 object, u32 length, u8 fill = 0x5AU)
{
    const usize route = write_route(Route{channel, RouteForm::Whole}, out);
    FrameHeader header;
    header.message = message.id;
    header.object = object;
    const usize used =
        write_frame_header(HeaderForm::ToEnd, FrameShape{message.operation != 0U, false}, header, out.subspan(route));
    REQUIRE(used != 0U);
    std::memset(out.data() + route + used, fill, length);
    return route + used + length;
}

constexpr MessageDesc kHello = kMessages[0];
constexpr MessageDesc kLate = kMessages[1];
constexpr MessageDesc kSnapshot = kMessages[2];
constexpr MessageDesc kVoice = kMessages[3];
constexpr MessageDesc kBlob = kMessages[4];
constexpr MessageDesc kQuiet = kMessages[5];
constexpr MessageDesc kMute = kMessages[6];
constexpr MessageDesc kPinch = kMessages[7];
constexpr MessageDesc kTone = kMessages[8];
constexpr MessageDesc kWider = kMessages[9];

// A deterministic stream of words.
struct Mix
{
    u64 state;

    u64 next()
    {
        state += 0x9E3779B97F4A7C15ULL;
        u64 z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
};
} // namespace

// ---- Frames ------------------------------------------------------------------------------------------------------

TEST_CASE("Frames: a route's known bytes and its refusals", "[net][frames]")
{
    u8 out[8];
    CHECK(write_route(Route{0U, RouteForm::Whole}, out) == 1U);
    CHECK(out[0] == 0U);
    CHECK(write_route(Route{3U, RouteForm::Listed}, out) == 1U);
    CHECK(out[0] == 13U);
    CHECK(write_route(Route{31U, RouteForm::Fragment}, out) == 1U);
    CHECK(out[0] == 126U);
    REQUIRE(write_route(Route{32U, RouteForm::Whole}, out) == 2U);
    CHECK(out[0] == 0x80U);
    CHECK(out[1] == 1U);
    CHECK(write_route(Route{0U, RouteForm::Whole}, Span<u8>(out, 0U)) == 0U);

    Route route;
    usize used = 0U;
    REQUIRE(read_route(ConstSpan<u8>(out, 2U), route, used) == FrameRead::Ok);
    CHECK(route.channel == 32U);
    CHECK(route.form == RouteForm::Whole);
    CHECK(used == 2U);
    CHECK(read_route(ConstSpan<u8>(out, 1U), route, used) == FrameRead::Short);
    CHECK(read_route(ConstSpan<u8>(out, 0U), route, used) == FrameRead::Short);

    const u8 padded[] = {3U};
    REQUIRE(read_route(padded, route, used) == FrameRead::Ok);
    CHECK(route.form == RouteForm::Padded);
    const u8 overlong[] = {0x81U, 0U};
    CHECK(read_route(overlong, route, used) == FrameRead::Malformed);
    const u8 too_wide[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0U};
    CHECK(read_route(too_wide, route, used) == FrameRead::Malformed);

    // The largest channel fits the route's five bytes.
    REQUIRE(write_route(Route{0xFFFFFFFFU, RouteForm::Fragment}, out) == crd::net::kMaxRouteBytes);
    REQUIRE(read_route(ConstSpan<u8>(out, crd::net::kMaxRouteBytes), route, used) == FrameRead::Ok);
    CHECK(route.channel == 0xFFFFFFFFU);
    CHECK(route.form == RouteForm::Fragment);
}

TEST_CASE("Frames: each header form's known bytes", "[net][frames]")
{
    FrameHeader header;
    header.message = 300U;
    header.length = 2U;
    header.object = 5U;
    header.chunk = 128U;
    header.sequence = 9U;
    header.index = 1U;

    u8 out[64];
    {
        // The length counts everything after itself: the object and the body.
        const usize used = write_frame_header(HeaderForm::Sized, FrameShape{true, false}, header, out);
        const u8 expected[] = {0xACU, 2U, 3U, 5U};
        REQUIRE(used == sizeof(expected));
        CHECK(std::memcmp(out, expected, used) == 0);
    }
    {
        const usize used = write_frame_header(HeaderForm::Sized, FrameShape{false, false}, header, out);
        const u8 expected[] = {0xACU, 2U, 2U};
        REQUIRE(used == sizeof(expected));
        CHECK(std::memcmp(out, expected, used) == 0);
    }
    {
        const usize used = write_frame_header(HeaderForm::ToEnd, FrameShape{true, false}, header, out);
        const u8 expected[] = {0xACU, 2U, 5U};
        REQUIRE(used == sizeof(expected));
        CHECK(std::memcmp(out, expected, used) == 0);
    }
    {
        const usize used = write_frame_header(HeaderForm::Fragment, FrameShape{true, false}, header, out);
        const u8 expected[] = {0xACU, 2U, 2U, 5U, 0x80U, 1U, 9U, 1U};
        REQUIRE(used == sizeof(expected));
        CHECK(std::memcmp(out, expected, used) == 0);

        u32 number = 0U;
        usize consumed = 0U;
        REQUIRE(read_frame_number(ConstSpan<u8>(out, used), number, consumed) == FrameRead::Ok);
        CHECK(number == 300U);
        CHECK(consumed == 2U);
        FrameHeader tail;
        REQUIRE(read_frame_tail(ConstSpan<u8>(out + 3U, used - 3U), HeaderForm::Fragment, FrameShape{true, false}, tail,
                                consumed) == FrameRead::Ok);
        CHECK(consumed == used - 3U);
        CHECK(tail.object == 5U);
        CHECK(tail.chunk == 128U);
        CHECK(tail.sequence == 9U);
        CHECK(tail.index == 1U);
        // Every proper prefix of the tail is short.
        for (usize size = 0U; size < used - 3U; ++size)
        {
            CHECK(read_frame_tail(ConstSpan<u8>(out + 3U, size), HeaderForm::Fragment, FrameShape{true, false}, tail,
                                  consumed) == FrameRead::Short);
        }
    }

    CHECK(write_frame_header(HeaderForm::Sized, FrameShape{true, false}, header, Span<u8>(out, 3U)) == 0U);

    // The widest stream header is the carry's size, and a header is whole by then.
    FrameHeader widest;
    widest.message = 0xFFFFFFFFU;
    widest.length = 0xFFFFFFFFU - static_cast<u32>(crd::net::kMaxFrameTailBytes);
    widest.object = ~u64{0};
    widest.expanded = 0xFFFFFFFFU;
    CHECK(write_frame_header(HeaderForm::Sized, FrameShape{true, true, true}, widest, out) == kMaxFrameHeaderBytes);
    // A length that with the tail passes what the number holds has no header.
    widest.length += 1U;
    CHECK(write_frame_header(HeaderForm::Sized, FrameShape{true, true, true}, widest, out) == 0U);
}

TEST_CASE("Frames: a fragment's shape", "[net][frames]")
{
    CHECK(fragment_count(1000U, 400U) == 3U);
    CHECK(fragment_count(1200U, 400U) == 3U);
    CHECK(fragment_count(1201U, 400U) == 4U);

    CHECK(fragment_shape_holds(1000U, 400U, 0U, 400U));
    CHECK(fragment_shape_holds(1000U, 400U, 1U, 400U));
    CHECK(fragment_shape_holds(1000U, 400U, 2U, 200U));
    CHECK(fragment_shape_holds(1200U, 400U, 2U, 400U));
    CHECK_FALSE(fragment_shape_holds(1200U, 400U, 3U, 400U)); // past the last, at a whole chunk
    CHECK_FALSE(fragment_shape_holds(1000U, 400U, 3U, 200U)); // past the last
    CHECK_FALSE(fragment_shape_holds(1000U, 400U, 2U, 400U)); // the last, too long
    CHECK_FALSE(fragment_shape_holds(1000U, 400U, 1U, 399U)); // a middle one, too short
    CHECK_FALSE(fragment_shape_holds(400U, 400U, 0U, 400U));  // one piece is the whole form's
    CHECK_FALSE(fragment_shape_holds(400U, 63U, 0U, 63U));    // below the smallest chunk
    CHECK(fragment_shape_holds(400U, 64U, 0U, 64U));
    CHECK(fragment_shape_holds(64U * kMaxFragments, 64U, kMaxFragments - 1U, 64U));
    CHECK_FALSE(fragment_shape_holds((64U * kMaxFragments) + 1U, 64U, 0U, 64U)); // too many pieces

    const u8 body[100] = {};
    u8 out[256];
    FrameHeader header;
    header.message = 4U;
    header.length = 100U;
    header.chunk = 64U;
    CHECK(write_fragment(2U, FrameShape{true, false}, header, body, out) != 0U);
    header.index = 1U;
    CHECK(write_fragment(2U, FrameShape{true, false}, header, body, out) != 0U);
    header.index = 2U;
    CHECK(write_fragment(2U, FrameShape{true, false}, header, body, out) == 0U); // past the last
    header.index = 0U;
    header.length = 99U;
    CHECK(write_fragment(2U, FrameShape{true, false}, header, body, out) == 0U); // not the body's size
    header.length = 100U;
    header.chunk = 0U;
    CHECK(write_fragment(2U, FrameShape{true, false}, header, body, out) == 0U);
    header.chunk = 64U;
    CHECK(write_fragment(2U, FrameShape{true, false}, header, body, Span<u8>(out, 20U)) == 0U); // no room
}

// ---- Gates, gauges and priorities --------------------------------------------------------------------------------

TEST_CASE("Quotas: a gate's arithmetic is exact", "[net][quota]")
{
    CHECK(gate_cost_ns(1U, 10U) == 100000000U);
    CHECK(gate_cost_ns(1U, 3U) == 333333334U);    // rounded up: a budget is a ceiling
    CHECK(gate_cost_ns(1U, 3000000000ULL) == 1U); // never free
    CHECK(gate_cost_ns(0xFFFFFFFFU, 1U) == 0xFFFFFFFFULL * kSecond);

    const GateRule ten{10U, kSecond};
    RateGate gate;
    u32 admitted = 0U;
    while (gate.open_at(ten) <= 0U)
    {
        gate.charge(1U, ten, 0U);
        ++admitted;
    }
    CHECK(admitted == 11U); // a second's worth of credit, and the one that spends it
    CHECK(gate.open_at(ten) == 100000000U);

    // A clear gate takes a message larger than its credit, and the wait after it pays.
    RateGate big;
    big.charge(100U, ten, 0U);
    CHECK(big.open_at(ten) == 9U * kSecond);

    // Idle time is credit only up to the tolerance.
    RateGate idle;
    idle.charge(1U, ten, 5U * kSecond);
    CHECK(idle.open_at(ten) == (4U * kSecond) + 100000000U);
    CHECK(gate_after(0U, 1U, ten, 5U * kSecond) == (5U * kSecond) + 100000000U);

    // None at all stays shut and charges nothing.
    const GateRule none{0U, kSecond};
    CHECK(gate_open_at(0U, none) == kNeverNs);
    RateGate shut;
    shut.charge(1000U, none, 7U);
    CHECK(shut.open_at(none) == kNeverNs);
    CHECK(shut.open_at(ten) == 0U);

    // The far end of the clock saturates.
    CHECK(gate_after(kNeverNs - 1U, 1U, ten, 0U) == kNeverNs);
    CHECK(gate_after(0U, 1U, ten, kNeverNs) == kNeverNs);
}

TEST_CASE("Quotas: a gate holds its long-run rate on any traffic", "[net][quota]")
{
    const GateRule rule{1000U, kSecond / 4U};
    for (u64 seed = 0U; seed < 32U; ++seed)
    {
        Mix mix{seed};
        RateGate gate;
        u64 now = 0U;
        u64 admitted = 0U;
        u64 largest = 0U;
        for (u32 i = 0U; i < 20000U; ++i)
        {
            now += mix.next() % 2000000U;
            const u32 units = static_cast<u32>(mix.next() % 64U) + 1U;
            if (gate.open_at(rule) <= now)
            {
                gate.charge(units, rule, now);
                admitted += units;
                largest = (units > largest) ? units : largest;
                // Never more than the rate, its credit and one message.
                const u64 allowed = ((now * rule.per_second) / kSecond) + (rule.per_second / 4U) + largest + 1U;
                REQUIRE(admitted <= allowed);
            }
        }
        // Offered far above the rate, it spends close to all of it.
        CHECK(admitted * 100U >= ((now * rule.per_second) / kSecond) * 90U);
    }
}

TEST_CASE("Quotas: a gauge's ceiling is exact", "[net][quota]")
{
    Gauge gauge;
    CHECK(gauge.try_add(7U, 10U));
    CHECK(gauge.try_add(3U, 10U));
    CHECK_FALSE(gauge.try_add(1U, 10U));
    CHECK(gauge.held() == 10U);
    gauge.release(4U);
    CHECK_FALSE(gauge.try_add(5U, 10U));
    CHECK(gauge.try_add(4U, 10U));
    CHECK_FALSE(gauge.try_add(~u64{0}, 20U));
    CHECK(gauge.held() == 10U);
}

TEST_CASE("Quotas: priorities keep headroom for the classes above", "[net][quota]")
{
    CHECK(priority_limit(800U, 0U) == 800U);
    CHECK(priority_limit(800U, 1U) == 700U);
    CHECK(priority_limit(800U, 4U) == 400U);
    CHECK(priority_limit(800U, 7U) == 100U);
    CHECK(priority_limit(800U, 200U) == 100U);
    CHECK(priority_limit(5U, 7U) == 5U);

    SendBudget budget(800U);
    CHECK(budget.try_queue(400U, 4U, true) == SendBudget::Verdict::Queued);
    CHECK(budget.try_queue(1U, 4U, true) == SendBudget::Verdict::Full);
    CHECK(budget.try_queue(1U, 4U, false) == SendBudget::Verdict::Shed);
    CHECK(budget.try_queue(400U, 0U, true) == SendBudget::Verdict::Queued);
    CHECK(budget.try_queue(1U, 0U, true) == SendBudget::Verdict::Full);
    CHECK(budget.queued() == 800U);
    budget.sent(400U);
    CHECK(budget.try_queue(1U, 4U, false) == SendBudget::Verdict::Shed); // still at its share
    budget.sent(1U);
    CHECK(budget.try_queue(1U, 4U, false) == SendBudget::Verdict::Queued);
    CHECK(budget.try_queue(900U, 0U, true) == SendBudget::Verdict::Full); // larger than the whole limit
}

TEST_CASE("Quotas: one principal's sessions share its gates under threads", "[net][quota]")
{
    const GateRule rule{1000U, kSecond};
    SharedRateGate gate;
    Gauge gauge;
    std::atomic<u64> taken{0U};
    constexpr u32 threads = 4U;
    constexpr u32 rounds = 20000U;
    std::thread workers[threads];
    for (std::thread& worker : workers)
    {
        worker = std::thread(
            [&]
            {
                for (u32 i = 0U; i < rounds; ++i)
                {
                    gate.charge(1U, rule, 0U);
                    if (gauge.try_add(3U, 1000U))
                    {
                        taken.fetch_add(1U, std::memory_order_relaxed);
                        gauge.release(3U);
                    }
                }
            });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    // No charge is lost: the next arrival is all of them.
    CHECK(gate.open_at(GateRule{1000U, 0U}) == threads * rounds * gate_cost_ns(1U, 1000U));
    CHECK(gauge.held() == 0U);
    CHECK(taken.load() != 0U);
}

// ---- Binding ------------------------------------------------------------------------------------------------------

TEST_CASE("Admission: bind refuses a schema it cannot trust", "[net][admission]")
{
    Buffers buffers;
    PrincipalQuota quota{QuotaLimits{}};
    ChannelState channels[16];
    Admission admission(quota, buffers, channels);
    AuthorizedSchema authorized = authorized_for(kGame);

    const SessionSchema good[] = {{&kGame, &authorized}, {&kTransport, nullptr}};
    CHECK(admission.bind(good));
    CHECK(admission.channel_count() == 9U);
    CHECK(admission.channel_number(1U, 1U) == 8U);
    CHECK(admission.channel(8U).desc == &kTransportChannels[1]);

    const SessionSchema no_wire[] = {{nullptr, nullptr}};
    CHECK_FALSE(admission.bind(no_wire));
    const SessionSchema unsigned_schema[] = {{&kGame, nullptr}};
    CHECK_FALSE(admission.bind(unsigned_schema));
    const SessionSchema signed_transport[] = {{&kTransport, &authorized}};
    CHECK_FALSE(admission.bind(signed_transport));

    AuthorizedSchema other = authorized;
    other.schema.schema.bytes[3] ^= 1U;
    const SessionSchema other_id[] = {{&kGame, &other}};
    CHECK_FALSE(admission.bind(other_id));
    other = authorized;
    other.schema.kind = ScopeKind::Project;
    const SessionSchema other_kind[] = {{&kGame, &other}};
    CHECK_FALSE(admission.bind(other_kind));
    other = authorized;
    other.schema.major = 2U;
    const SessionSchema other_major[] = {{&kGame, &other}};
    CHECK_FALSE(admission.bind(other_major));

    const SessionSchema too_many[kMaxSessionSchemas + 1U] = {{&kTransport, nullptr},
                                                             {&kTransport, nullptr},
                                                             {&kTransport, nullptr},
                                                             {&kTransport, nullptr},
                                                             {&kTransport, nullptr}};
    CHECK_FALSE(admission.bind(too_many));

    ChannelState small[8];
    Admission tight(quota, buffers, small);
    CHECK_FALSE(tight.bind(good));
    // A refused bind left the last good one in place.
    CHECK(admission.channel_count() == 9U);
}

TEST_CASE("Admission: a schema's revision is announced once", "[net][admission]")
{
    Session session;
    Admission& admission = session.admission;
    ConstSpan<u8> game(kGame.id, sizeof(kGame.id));
    ConstSpan<u8> transport(kTransport.id, sizeof(kTransport.id));

    CHECK(admission.announce(game, 3U, 1U));       // the same again changes nothing
    CHECK_FALSE(admission.announce(game, 3U, 0U)); // another minor
    CHECK_FALSE(admission.announce(game, 2U, 1U)); // another major
    CHECK_FALSE(admission.announce(ConstSpan<u8>(kGame.id, 15U), 3U, 1U));
    const u8 unknown[16] = {};
    CHECK_FALSE(admission.announce(unknown, 3U, 1U));
    CHECK(admission.announce(transport, 1U, 0U));
}

// ---- Stage S6 and S7 over a stream --------------------------------------------------------------------------------

TEST_CASE("Admission: a schema above S18 reads nothing before its announcement", "[net][admission]")
{
    Buffers buffers;
    PrincipalQuota quota{QuotaLimits{}};
    ChannelState channels[16];
    Admission admission(quota, buffers, channels);
    AuthorizedSchema authorized = authorized_for(kGame);
    Capability capability = capability_for(authorized);
    const SessionSchema schemas[] = {{&kGame, &authorized}, {&kTransport, nullptr}};
    REQUIRE(admission.bind(schemas));
    admission.set_capability(&capability);

    u8 bytes[64];
    usize size = put_preamble(bytes, kControl);
    size = put_frame(bytes, size, kHello, 1U, 4U);
    {
        StreamAssembler assembler(admission);
        FrameStep step = assembler.feed(ConstSpan<u8>(bytes, 1U), Delivery::Established, 0U);
        REQUIRE(step.action == AdmitAction::Continue);
        step = assembler.feed(ConstSpan<u8>(bytes + 1U, size - 1U), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::Unannounced);
        CHECK(step.action == AdmitAction::Close);
    }

    // A transport schema is readable: how the announcement itself travels.
    size = put_preamble(bytes, admission.channel_number(1U, 0U));
    size = put_frame(bytes, size, kTransportMessages[0], 0U, 4U);
    {
        StreamAssembler assembler(admission);
        FrameStep step = assembler.feed(ConstSpan<u8>(bytes, 1U), Delivery::Established, 0U);
        REQUIRE(step.action == AdmitAction::Continue);
        step = assembler.feed(ConstSpan<u8>(bytes + 1U, size - 1U), Delivery::Established, 0U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        CHECK(step.frame.writer_minor == 0U);
        admission.finish(step.frame);
    }
    CHECK(quota.pending.held() == 0U);
    CHECK(quota.streams.held() == 0U);
}

TEST_CASE("Admission: a transport message runs below S18, on control channels only", "[net][admission]")
{
    Session session;
    Admission& admission = session.admission;
    admission.set_capability(nullptr);

    u8 bytes[64];
    const u32 control_channel = admission.channel_number(1U, 0U);
    const u32 event_channel = admission.channel_number(1U, 1U);

    struct Case
    {
        u32 channel;
        const MessageDesc* message;
        AdmitVerdict verdict;
    };
    const Case cases[] = {
        {control_channel, &kTransportMessages[0], AdmitVerdict::Admitted},
        {event_channel, &kTransportMessages[1], AdmitVerdict::Undeclared},   // not a control channel
        {control_channel, &kTransportMessages[2], AdmitVerdict::Undeclared}, // an operation below S18
    };
    for (const Case& c : cases)
    {
        StreamAssembler assembler(admission);
        open(assembler, c.channel);
        const usize size = put_frame(bytes, 0U, *c.message, 0U, 3U);
        const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == c.verdict);
        if (step.action == AdmitAction::Deliver)
        {
            admission.finish(step.frame);
        }
    }
    CHECK(session.quota.pending.held() == 0U);
}

TEST_CASE("Admission: each frame refusal on a stream, and what it asks of the caller", "[net][admission]")
{
    Session session;
    u8 bytes[256];

    struct Case
    {
        const char* name;
        u32 channel;
        u32 message;
        u32 length;
        AdmitVerdict verdict;
        AdmitAction action;
    };
    const Case cases[] = {
        {"unknown", kControl, 99U, 2U, AdmitVerdict::UnknownMessage, AdmitAction::Close},
        {"other channel", kControl, kSnapshot.id, 2U, AdmitVerdict::OtherChannel, AdmitAction::Close},
        {"undersized", kControl, kHello.id, 1U, AdmitVerdict::Undersized, AdmitAction::Close},
        {"oversized", kControl, kHello.id, 101U, AdmitVerdict::Oversized, AdmitAction::Close},
        {"at its least", kControl, kHello.id, 2U, AdmitVerdict::Admitted, AdmitAction::Deliver},
        {"at its most", kControl, kHello.id, 100U, AdmitVerdict::Admitted, AdmitAction::Deliver},
        {"a two-byte identifier", kControl, kWider.id, 3U, AdmitVerdict::Admitted, AdmitAction::Deliver},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.name);
        StreamAssembler assembler(session.admission);
        open(assembler, c.channel);
        FrameHeader header;
        header.message = c.message;
        header.length = c.length;
        header.object = 1U;
        const usize used = write_frame_header(HeaderForm::Sized, FrameShape{true, false}, header, bytes);
        std::memset(bytes + used, 1, c.length);
        const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, used + c.length), Delivery::Established, 0U);
        CHECK(step.verdict == c.verdict);
        CHECK(step.action == c.action);
        if (step.action == AdmitAction::Deliver)
        {
            CHECK(step.consumed == used + c.length);
            session.admission.finish(step.frame);
        }
        else
        {
            // A closed stream stays closed, whatever comes.
            const FrameStep again = assembler.feed(ConstSpan<u8>(bytes, 4U), Delivery::Established, 0U);
            CHECK(again.verdict == c.verdict);
            CHECK(again.action == AdmitAction::Close);
            CHECK(again.consumed == 4U);
        }
    }

    // A number too wide for its field.
    {
        StreamAssembler assembler(session.admission);
        open(assembler, kControl);
        const u8 wide[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 1U};
        const FrameStep step = assembler.feed(wide, Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::Malformed);
        CHECK(step.action == AdmitAction::Close);
    }
    // A route that is no stream's.
    {
        StreamAssembler assembler(session.admission);
        const u8 listed[] = {1U};
        CHECK(assembler.feed(listed, Delivery::Established, 0U).verdict == AdmitVerdict::Malformed);
    }
    {
        StreamAssembler assembler(session.admission);
        const u8 beyond[] = {0x40U};
        const FrameStep step = assembler.feed(beyond, Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::BadChannel);
        CHECK(step.action == AdmitAction::Close);
    }
    {
        StreamAssembler assembler(session.admission);
        u8 route[4];
        const usize used = put_preamble(route, kState);
        const FrameStep step = assembler.feed(ConstSpan<u8>(route, used), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::WrongCarrier);
        CHECK(step.action == AdmitAction::Close);
    }
    CHECK(session.quota.streams.held() == 0U);
    CHECK(session.quota.pending.held() == 0U);
    CHECK(session.admission.count(AdmitVerdict::Oversized) == 1U);
    CHECK(session.admission.count(AdmitVerdict::Malformed) == 2U);
}

TEST_CASE("Admission: a message's revision and a newer writer", "[net][admission]")
{
    u8 bytes[256];

    // A writer at minor 0 has no Late.
    {
        Buffers buffers;
        PrincipalQuota quota{QuotaLimits{}};
        ChannelState channels[16];
        Admission admission(quota, buffers, channels);
        AuthorizedSchema authorized = authorized_for(kGame);
        Capability capability = capability_for(authorized);
        const SessionSchema schemas[] = {{&kGame, &authorized}};
        REQUIRE(admission.bind(schemas));
        REQUIRE(admission.announce(ConstSpan<u8>(kGame.id, sizeof(kGame.id)), 3U, 0U));
        admission.set_capability(&capability);
        StreamAssembler assembler(admission);
        open(assembler, kControl);
        const usize size = put_frame(bytes, 0U, kLate, 1U, 3U);
        const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::NotInRevision);
        CHECK(step.action == AdmitAction::Close);
    }

    // A writer at a newer minor: a message this reader has never heard of is stepped over, its fields are not its
    // concern and what follows is read, and a known message may be longer than this reader's most, up to the channel's.
    Buffers buffers;
    PrincipalQuota quota{QuotaLimits{}};
    ChannelState channels[16];
    Admission admission(quota, buffers, channels);
    AuthorizedSchema authorized = authorized_for(kGame);
    Capability capability = capability_for(authorized);
    const SessionSchema schemas[] = {{&kGame, &authorized}};
    REQUIRE(admission.bind(schemas));
    REQUIRE(admission.announce(ConstSpan<u8>(kGame.id, sizeof(kGame.id)), 3U, 9U));
    admission.set_capability(&capability);

    StreamAssembler assembler(admission);
    open(assembler, kControl);
    FrameHeader unknown;
    unknown.message = 77U;
    unknown.length = 5U;
    usize size = write_frame_header(HeaderForm::Sized, FrameShape{false, false}, unknown, bytes);
    std::memset(bytes + size, 0xEE, 5U);
    size += 5U;
    const usize longer_at = size;
    size = put_frame(bytes, size, kHello, 1U, 150U);

    FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NewerMessage);
    CHECK(step.action == AdmitAction::Skip);
    CHECK(step.consumed == longer_at);
    step = assembler.feed(ConstSpan<u8>(bytes + longer_at, size - longer_at), Delivery::Established, 0U);
    REQUIRE(step.verdict == AdmitVerdict::Admitted);
    CHECK(step.frame.writer_minor == 9U);
    CHECK(step.frame.length == 150U);
    admission.finish(step.frame);

    // A newer message split across feeds costs nothing to step over.
    const usize head = write_frame_header(HeaderForm::Sized, FrameShape{false, false}, unknown, bytes);
    step = assembler.feed(ConstSpan<u8>(bytes, head), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NewerMessage);
    step = assembler.feed(ConstSpan<u8>(bytes, 3U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NeedMore);
    CHECK(step.consumed == 3U);
    step = assembler.feed(ConstSpan<u8>(bytes, 2U), Delivery::Established, 0U);
    CHECK(step.consumed == 2U);
    CHECK(buffers.total == 0U);

    // Even a newer writer stays within the channel's most, for a message this reader knows too.
    {
        StreamAssembler known(admission);
        open(known, kControl);
        const usize over = put_frame(bytes, 0U, kHello, 1U, 3U);
        FrameHeader long_hello;
        long_hello.message = kHello.id;
        long_hello.length = 1001U;
        long_hello.object = 1U;
        const usize head_only = write_frame_header(HeaderForm::Sized, FrameShape{true, false}, long_hello, bytes);
        const FrameStep refused = known.feed(ConstSpan<u8>(bytes, head_only), Delivery::Established, 0U);
        CHECK(refused.verdict == AdmitVerdict::Oversized);
        CHECK(over != 0U);
    }

    // A newer message's length counts its header's tail too, which the channel's most does not.
    {
        StreamAssembler at_most(admission);
        open(at_most, kControl);
        unknown.length = 1000U + static_cast<u32>(crd::net::kMaxFrameTailBytes);
        const usize widest = write_frame_header(HeaderForm::Sized, FrameShape{false, false}, unknown, bytes);
        CHECK(at_most.feed(ConstSpan<u8>(bytes, widest), Delivery::Established, 0U).verdict ==
              AdmitVerdict::NewerMessage);
    }
    unknown.length = 1000U + static_cast<u32>(crd::net::kMaxFrameTailBytes) + 1U;
    const usize too_long = write_frame_header(HeaderForm::Sized, FrameShape{false, false}, unknown, bytes);
    step = assembler.feed(ConstSpan<u8>(bytes, too_long), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::Oversized);
    CHECK(step.action == AdmitAction::Close);

    // And stepping over is charged to the principal, whose closed gate holds the reader back.
    QuotaLimits narrow;
    narrow.bytes_per_s = 10U;
    narrow.tolerance_ns = 0U;
    PrincipalQuota narrow_quota{narrow};
    ChannelState narrow_channels[16];
    Admission narrow_admission(narrow_quota, buffers, narrow_channels);
    REQUIRE(narrow_admission.bind(schemas));
    REQUIRE(narrow_admission.announce(ConstSpan<u8>(kGame.id, sizeof(kGame.id)), 3U, 9U));
    narrow_admission.set_capability(&capability);
    StreamAssembler narrow_assembler(narrow_admission);
    open(narrow_assembler, kControl);
    unknown.length = 20U;
    const usize head2 = write_frame_header(HeaderForm::Sized, FrameShape{false, false}, unknown, bytes);
    step = narrow_assembler.feed(ConstSpan<u8>(bytes, head2), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NewerMessage);
    step = narrow_assembler.feed(ConstSpan<u8>(bytes, 20U), Delivery::Established, 0U);
    CHECK(step.consumed == 20U);
    step = narrow_assembler.feed(ConstSpan<u8>(bytes, head2), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::PrincipalBytes);
    CHECK(step.action == AdmitAction::Wait);
    CHECK(step.consumed == 0U);
    CHECK(step.retry_at_ns == 2U * kSecond);
    step = narrow_assembler.feed(ConstSpan<u8>(bytes, head2), Delivery::Established, 2U * kSecond);
    CHECK(step.verdict == AdmitVerdict::NewerMessage);
}

TEST_CASE("Admission: S6's gates, each at its boundary, spending nothing when closed", "[net][admission]")
{
    u8 bytes[512];

    SECTION("the channel's messages per second")
    {
        Session session;
        StreamAssembler assembler(session.admission);
        open(assembler, kControl);
        const usize size = put_frame(bytes, 0U, kHello, 1U, 2U);
        // Ten a second, with a second's credit: eleven pass at once.
        for (u32 i = 0U; i < 11U; ++i)
        {
            const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
            REQUIRE(step.verdict == AdmitVerdict::Admitted);
            session.admission.finish(step.frame);
        }
        FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::ChannelMessages);
        CHECK(step.action == AdmitAction::Wait);
        CHECK(step.consumed == 0U);
        CHECK(step.retry_at_ns == kSecond / 10U);
        // One nanosecond early it is still shut, and asking again spent nothing.
        step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, (kSecond / 10U) - 1U);
        CHECK(step.verdict == AdmitVerdict::ChannelMessages);
        step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, kSecond / 10U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        session.admission.finish(step.frame);
    }
    SECTION("the channel's bytes per second")
    {
        Session session;
        // 100 a second, with a second's credit: the frame that passes the credit still goes, and pays by the wait.
        const usize size = put_frame(bytes, 0U, kPinch, 1U, 60U);
        StreamAssembler narrow(session.admission);
        open(narrow, kNarrow);
        for (u32 i = 0U; i < 2U; ++i)
        {
            const FrameStep step = narrow.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
            REQUIRE(step.verdict == AdmitVerdict::Admitted);
            session.admission.finish(step.frame);
        }
        FrameStep step = narrow.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::ChannelBytes);
        CHECK(step.retry_at_ns == kSecond / 5U);
        CHECK(step.consumed == 0U);
        step = narrow.feed(ConstSpan<u8>(bytes, size), Delivery::Established, kSecond / 5U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        session.admission.finish(step.frame);
    }
    SECTION("a zero budget is no message and no byte")
    {
        Session session;
        {
            StreamAssembler assembler(session.admission);
            open(assembler, kSilent);
            const usize size = put_frame(bytes, 0U, kQuiet, 1U, 1U);
            const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
            CHECK(step.verdict == AdmitVerdict::ChannelMessages);
            CHECK(step.retry_at_ns == kNeverNs);
        }
        {
            StreamAssembler assembler(session.admission);
            open(assembler, kVoiceless);
            const usize size = put_frame(bytes, 0U, kMute, 1U, 1U);
            const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
            CHECK(step.verdict == AdmitVerdict::ChannelBytes);
        }
    }
    SECTION("the principal's messages, bytes and streams")
    {
        QuotaLimits limits;
        limits.messages_per_s = 1U;
        limits.tolerance_ns = kCredit;
        limits.streams = 2U;
        Session session(limits);
        {
            StreamAssembler assembler(session.admission);
            open(assembler, kBulk);
            const usize size = put_frame(bytes, 0U, kBlob, 1U, 8U);
            FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
            REQUIRE(step.verdict == AdmitVerdict::Admitted);
            session.admission.finish(step.frame);
            step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
            CHECK(step.verdict == AdmitVerdict::PrincipalMessages);
            CHECK(step.retry_at_ns == kSecond - kCredit);

            StreamAssembler second(session.admission);
            open(second, kBulk);
            CHECK(session.quota.streams.held() == 2U);
            StreamAssembler third(session.admission);
            u8 route[4];
            const usize used = put_preamble(route, kBulk);
            const FrameStep refused = third.feed(ConstSpan<u8>(route, used), Delivery::Established, 0U);
            CHECK(refused.verdict == AdmitVerdict::StreamLimit);
            CHECK(refused.action == AdmitAction::Skip); // the stream's own end
            const FrameStep again = third.feed(ConstSpan<u8>(bytes, 4U), Delivery::Established, 0U);
            CHECK(again.verdict == AdmitVerdict::StreamLimit);
            CHECK(again.consumed == 4U);
        }
        CHECK(session.quota.streams.held() == 0U);

        QuotaLimits by_bytes;
        by_bytes.bytes_per_s = 8U;
        by_bytes.tolerance_ns = kCredit;
        Session bytes_session(by_bytes);
        StreamAssembler assembler(bytes_session.admission);
        open(assembler, kBulk);
        const usize size = put_frame(bytes, 0U, kBlob, 1U, 8U);
        FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        bytes_session.admission.finish(step.frame);
        step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::PrincipalBytes);
        CHECK(step.retry_at_ns == kSecond - kCredit);
    }
    SECTION("the capability's budget, kept across renewal")
    {
        QuotaLimits limits;
        limits.tolerance_ns = kCredit;
        Session session(limits);
        session.capability.grant.budget.operations_per_second = 1U;
        StreamAssembler assembler(session.admission);
        open(assembler, kBulk);
        const usize size = put_frame(bytes, 0U, kBlob, 1U, 8U);
        FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        session.admission.finish(step.frame);
        step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::BudgetOperations);
        CHECK(step.retry_at_ns == kSecond - kCredit);

        // A renewal does not refill what was spent.
        Capability renewed = session.capability;
        session.admission.set_capability(&renewed);
        step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::BudgetOperations);

        renewed.grant.budget.operations_per_second = 1000U;
        renewed.grant.budget.bytes_per_second = 8U;
        step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, kSecond);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        session.admission.finish(step.frame);
        step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, kSecond + (kSecond / 1000U));
        CHECK(step.verdict == AdmitVerdict::BudgetBytes);

        // A capability that grants no rate admits nothing.
        renewed.grant.budget.operations_per_second = 0U;
        step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 100U * kSecond);
        CHECK(step.verdict == AdmitVerdict::BudgetOperations);
        CHECK(step.retry_at_ns == kNeverNs);
    }
    SECTION("the pending messages, within a class's share")
    {
        QuotaLimits limits;
        limits.pending_messages = 8U;
        Session session(limits);
        StreamAssembler bulk(session.admission);
        open(bulk, kBulk);
        const usize size = put_frame(bytes, 0U, kBlob, 1U, 8U);
        AdmittedFrame held[4];
        for (AdmittedFrame& frame : held)
        {
            const FrameStep step = bulk.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
            REQUIRE(step.verdict == AdmitVerdict::Admitted);
            frame = step.frame;
        }
        // Priority 4 has half of the eight.
        FrameStep step = bulk.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::PendingFull);
        CHECK(step.action == AdmitAction::Wait);
        CHECK(step.retry_at_ns == kNeverNs);
        CHECK(step.consumed == 0U);

        // The control channel still gets through.
        StreamAssembler control_stream(session.admission);
        open(control_stream, kControl);
        const usize small = put_frame(Span<u8>(bytes + 64U, 64U), 0U, kHello, 1U, 2U);
        const FrameStep hello = control_stream.feed(ConstSpan<u8>(bytes + 64U, small), Delivery::Established, 0U);
        REQUIRE(hello.verdict == AdmitVerdict::Admitted);
        session.admission.finish(hello.frame);

        // Finishing one makes room for the same bytes.
        session.admission.finish(held[0]);
        step = bulk.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        session.admission.finish(step.frame);
        for (u32 i = 1U; i < 4U; ++i)
        {
            session.admission.finish(held[i]);
        }
        CHECK(session.quota.pending.held() == 0U);
    }
    SECTION("a principal at its budget can still renew")
    {
        QuotaLimits limits;
        limits.messages_per_s = 1U;
        limits.bytes_per_s = 1U;
        limits.tolerance_ns = kCredit;
        Session session(limits);
        StreamAssembler bulk(session.admission);
        open(bulk, kBulk);
        const usize size = put_frame(bytes, 0U, kBlob, 1U, 8U);
        FrameStep step = bulk.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        session.admission.finish(step.frame);
        step = bulk.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::PrincipalMessages);

        StreamAssembler control_stream(session.admission);
        open(control_stream, kControl);
        const usize small = put_frame(Span<u8>(bytes + 64U, 64U), 0U, kHello, 1U, 2U);
        const FrameStep hello = control_stream.feed(ConstSpan<u8>(bytes + 64U, small), Delivery::Established, 0U);
        REQUIRE(hello.verdict == AdmitVerdict::Admitted);
        session.admission.finish(hello.frame);
    }
}

TEST_CASE("Admission: S7 asks S18's verdict before anything reads the body", "[net][admission]")
{
    u8 bytes[512];

    const auto run = [&](Session& session, const MessageDesc& message, u32 channel, u64 object, Delivery delivery)
    {
        StreamAssembler assembler(session.admission);
        open(assembler, channel);
        const usize size = put_frame(bytes, 0U, message, object, 8U);
        const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), delivery, 0U);
        if (step.verdict == AdmitVerdict::Admitted)
        {
            session.admission.finish(step.frame);
        }
        else if (step.action == AdmitAction::Skip)
        {
            CHECK(step.consumed == size); // the framing holds: the body is stepped over
        }
        CHECK(session.quota.pending.held() == 0U);
        return step.verdict;
    };

    Session session;
    CHECK(run(session, kBlob, kBulk, 1U, Delivery::Established) == AdmitVerdict::Admitted);

    // No capability at all fails closed, on a message with an operation, and gives back what it held.
    session.admission.set_capability(nullptr);
    CHECK(run(session, kBlob, kBulk, 1U, Delivery::Established) == AdmitVerdict::NoCapability);
    session.admission.set_capability(&session.capability);

    // Another audience, another session type.
    Capability other = session.capability;
    other.audience.bytes[0] ^= 1U;
    session.admission.set_capability(&other);
    CHECK(run(session, kBlob, kBulk, 1U, Delivery::Established) == AdmitVerdict::OtherSchema);
    other = session.capability;
    other.grant.kind = ScopeKind::Project;
    CHECK(run(session, kBlob, kBulk, 1U, Delivery::Established) == AdmitVerdict::OtherSchema);
    session.admission.set_capability(&session.capability);

    // A message's bit the signed artefact does not declare.
    AuthorizedSchema narrow = session.authorized;
    narrow.schema.operations = 1ULL; // not bit 3, Blob's
    narrow.schema.idempotent = 0ULL;
    Session narrowed;
    narrowed.authorized = narrow;
    CHECK(run(narrowed, kBlob, kBulk, 1U, Delivery::Established) == AdmitVerdict::Undeclared);

    // Early data runs only what the artefact declares idempotent: bit 1, Snapshot's, and not Blob's bit 3.
    CHECK(run(session, kBlob, kBulk, 1U, Delivery::Early) == AdmitVerdict::Replayable);

    // Bits and objects the grant does not hold.
    Capability no_bit = capability_for(session.authorized, 1ULL);
    session.admission.set_capability(&no_bit);
    CHECK(run(session, kBlob, kBulk, 1U, Delivery::Established) == AdmitVerdict::NotGranted);
    Capability one_object = capability_for(session.authorized);
    one_object.grant.ranges[0] = crd::crypto::ObjectRange{10U, 20U};
    one_object.grant.range_count = 1U;
    session.admission.set_capability(&one_object);
    CHECK(run(session, kBlob, kBulk, 9U, Delivery::Established) == AdmitVerdict::NotGranted);
    CHECK(run(session, kBlob, kBulk, 10U, Delivery::Established) == AdmitVerdict::Admitted);
    CHECK(run(session, kBlob, kBulk, 20U, Delivery::Established) == AdmitVerdict::Admitted);
    CHECK(run(session, kBlob, kBulk, 21U, Delivery::Established) == AdmitVerdict::NotGranted);

    // A refused large frame arriving in pieces is stepped over with no buffer at all, and then the stream reads on.
    Session big;
    Capability read_only = capability_for(big.authorized, 1ULL);
    big.admission.set_capability(&read_only);
    {
        StreamAssembler assembler(big.admission);
        open(assembler, kBulk);
        static u8 large[60000];
        const usize size = put_frame(large, 0U, kBlob, 1U, 50000U);
        usize at = 0U;
        FrameStep step = assembler.feed(ConstSpan<u8>(large, 100U), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::NotGranted);
        CHECK(step.action == AdmitAction::Skip);
        CHECK(step.consumed == 100U);
        at = 100U;
        while (at < size)
        {
            const usize part = ((size - at) < 777U) ? (size - at) : 777U;
            step = assembler.feed(ConstSpan<u8>(large + at, part), Delivery::Established, 0U);
            REQUIRE(step.consumed == part);
            REQUIRE(step.action == AdmitAction::Continue);
            at += part;
        }
        CHECK(big.buffers.total == 0U);
        CHECK(big.quota.reassembly.held() == 0U);
        big.admission.set_capability(&big.capability);
        const usize next = put_frame(bytes, 0U, kBlob, 1U, 8U);
        step = assembler.feed(ConstSpan<u8>(bytes, next), Delivery::Established, 1U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        big.admission.finish(step.frame);
    }

    // A refusal has been charged: refused traffic is not free.
    QuotaLimits limits;
    limits.messages_per_s = 2U;
    limits.tolerance_ns = kCredit;
    Session abusive(limits);
    Capability none = capability_for(abusive.authorized, 1ULL);
    abusive.admission.set_capability(&none);
    {
        StreamAssembler assembler(abusive.admission);
        open(assembler, kBulk);
        const usize size = put_frame(bytes, 0U, kBlob, 1U, 8U);
        CHECK(assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U).verdict ==
              AdmitVerdict::NotGranted);
        const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::PrincipalMessages);
        CHECK(step.retry_at_ns == (kSecond / 2U) - kCredit);
    }
    CHECK(abusive.admission.count(AdmitVerdict::NotGranted) == 1U);
}

TEST_CASE("Admission: every verdict has a name and an action", "[net][admission]")
{
    for (usize index = 0U; index < kAdmitVerdictCount; ++index)
    {
        const auto verdict = static_cast<AdmitVerdict>(index);
        CHECK(admit_verdict_name(verdict) != "unknown");
        for (usize other = 0U; other < index; ++other)
        {
            CHECK(admit_verdict_name(verdict) != admit_verdict_name(static_cast<AdmitVerdict>(other)));
        }
    }
    // Reliable channels cannot lose what they declared; unreliable ones never end the session.
    CHECK(admit_action(AdmitVerdict::ChannelBytes, true) == AdmitAction::Wait);
    CHECK(admit_action(AdmitVerdict::ChannelBytes, false) == AdmitAction::Skip);
    CHECK(admit_action(AdmitVerdict::Oversized, true) == AdmitAction::Close);
    CHECK(admit_action(AdmitVerdict::Oversized, false) == AdmitAction::Skip);
    CHECK(admit_action(AdmitVerdict::NotGranted, true) == AdmitAction::Skip);
    CHECK(admit_action(AdmitVerdict::NotGranted, false) == AdmitAction::Skip);
    CHECK(admit_action(AdmitVerdict::Admitted, false) == AdmitAction::Deliver);
    CHECK(admit_action(AdmitVerdict::NeedMore, true) == AdmitAction::Continue);
    CHECK(admit_action(static_cast<AdmitVerdict>(kAdmitVerdictCount), true) == AdmitAction::Close);
}

// ---- Streams: generated messages, every split ---------------------------------------------------------------------

TEST_CASE("Streams: generated messages through admission and their decoders, at every split", "[net][stream]")
{
    // The session control channel's real messages, over the transport schema.
    u8 token[900];
    for (usize i = 0U; i < sizeof(token); ++i)
    {
        token[i] = static_cast<u8>(i * 7U);
    }
    control::CapabilityPresent present;
    present.token = ConstSpan<u8>(token, sizeof(token));
    control::StateHash hash;
    hash.step = 123456789U;
    hash.hash = 0x0123456789ABCDEFULL;
    hash.chain = 0xFEDCBA9876543210ULL;
    control::GoAway away;
    away.reason = control::GoAwayReason::Overloaded;
    away.retry_after_ms = 1500U;
    away.detail = "busy";

    u8 stream[2048];
    usize size = write_route(Route{0U, RouteForm::Whole}, stream);
    usize used = 0U;
    REQUIRE(write_frame(control::kSchema, HeaderForm::Sized, present, 0U, 0U,
                        Span<u8>(stream + size, sizeof(stream) - size), used) == WireError::Ok);
    size += used;
    REQUIRE(write_frame(control::kSchema, HeaderForm::Sized, hash, 0U, 0U,
                        Span<u8>(stream + size, sizeof(stream) - size), used) == WireError::Ok);
    size += used;
    REQUIRE(write_frame(control::kSchema, HeaderForm::Sized, away, 0U, 0U,
                        Span<u8>(stream + size, sizeof(stream) - size), used) == WireError::Ok);
    size += used;

    const auto run = [&](usize first_part, usize part)
    {
        Buffers buffers;
        PrincipalQuota quota{QuotaLimits{}};
        ChannelState channels[2];
        Admission admission(quota, buffers, channels);
        const SessionSchema schemas[] = {{&control::kSchema, nullptr}};
        REQUIRE(admission.bind(schemas));
        u32 delivered = 0U;
        {
            StreamAssembler assembler(admission);
            usize at = 0U;
            usize give = first_part;
            while (at < size)
            {
                const usize have = (give < (size - at)) ? give : (size - at);
                usize read = 0U;
                while (read < have)
                {
                    const FrameStep step =
                        assembler.feed(ConstSpan<u8>(stream + at + read, have - read), Delivery::Established, 0U);
                    REQUIRE(step.action != AdmitAction::Close);
                    REQUIRE(step.action != AdmitAction::Wait);
                    read += step.consumed;
                    if (step.action == AdmitAction::Deliver)
                    {
                        if (delivered == 0U)
                        {
                            control::CapabilityPresent got;
                            REQUIRE(crd::net::decode_frame(step.frame, got) == WireError::Ok);
                            REQUIRE(got.token.size() == sizeof(token));
                            REQUIRE(std::memcmp(got.token.data(), token, sizeof(token)) == 0);
                            control::StateHash wrong;
                            CHECK(crd::net::decode_frame(step.frame, wrong) == WireError::OutOfRange);
                        }
                        else if (delivered == 1U)
                        {
                            control::StateHash got;
                            REQUIRE(crd::net::decode_frame(step.frame, got) == WireError::Ok);
                            REQUIRE(got.chain == hash.chain);
                        }
                        else
                        {
                            control::GoAway got;
                            REQUIRE(crd::net::decode_frame(step.frame, got) == WireError::Ok);
                            REQUIRE(got.detail == "busy");
                        }
                        ++delivered;
                        admission.finish(step.frame);
                    }
                }
                at += have;
                give = part;
            }
        }
        REQUIRE(delivered == 3U);
        REQUIRE(buffers.live == 0U);
        REQUIRE(quota.pending.held() == 0U);
        REQUIRE(quota.reassembly.held() == 0U);
        REQUIRE(quota.streams.held() == 0U);
        return buffers.total;
    };

    // All at once: zero copies.
    CHECK(run(size, size) == 0U);
    // Every split in two, and every small piece size.
    for (usize first = 1U; first < size; ++first)
    {
        run(first, size);
    }
    for (usize part = 1U; part <= 40U; ++part)
    {
        run(part, part);
    }
}

TEST_CASE("Streams: a route in two pieces, on a session of many channels", "[net][stream]")
{
    static ChannelDesc many[40];
    for (ChannelDesc& channel : many)
    {
        channel.name = "control";
        channel.channel_class = ChannelClass::Control;
        channel.reliable = true;
        channel.max_message_bytes = 100U;
        channel.messages_per_s = 100U;
        channel.bytes_per_s = 10000U;
        channel.integrity = false;
    }
    static const MessageDesc kOnChannel[] = {{"Ping", 1U, 33U, 0U, 0ULL, 0U, 100U}};
    WireSchema schema = kTransport;
    schema.channels = many;
    schema.messages = kOnChannel;

    Buffers buffers;
    PrincipalQuota quota{QuotaLimits{}};
    static ChannelState channels[40];
    Admission admission(quota, buffers, channels);
    const SessionSchema schemas[] = {{&schema, nullptr}};
    REQUIRE(admission.bind(schemas));

    u8 bytes[32];
    const usize route = put_preamble(bytes, 33U);
    REQUIRE(route == 2U);
    const usize size = put_frame(bytes, route, kOnChannel[0], 0U, 4U);

    StreamAssembler assembler(admission);
    FrameStep step = assembler.feed(ConstSpan<u8>(bytes, 1U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NeedMore);
    CHECK(step.consumed == 1U);
    CHECK(quota.streams.held() == 0U);
    step = assembler.feed(ConstSpan<u8>(bytes + 1U, size - 1U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NeedMore);
    CHECK(step.consumed == 1U); // the route's second byte, not what the first feed held
    CHECK(quota.streams.held() == 1U);
    step = assembler.feed(ConstSpan<u8>(bytes + 2U, size - 2U), Delivery::Established, 0U);
    REQUIRE(step.verdict == AdmitVerdict::Admitted);
    CHECK(step.consumed == size - 2U);
    CHECK(step.frame.channel == 33U);
    admission.finish(step.frame);

    // A route beyond the table, in two pieces, is still that route.
    StreamAssembler beyond(admission);
    u8 far_route[4];
    REQUIRE(write_route(Route{1000U, RouteForm::Whole}, far_route) == 2U);
    step = beyond.feed(ConstSpan<u8>(far_route, 1U), Delivery::Established, 0U);
    CHECK(step.action == AdmitAction::Continue);
    step = beyond.feed(ConstSpan<u8>(far_route + 1U, 1U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::BadChannel);
    CHECK(step.action == AdmitAction::Close);
}

TEST_CASE("Streams: a whole frame is a view of the input; a split one is copied once", "[net][stream]")
{
    Session session;
    u8 bytes[256];
    {
        StreamAssembler assembler(session.admission);
        open(assembler, kBulk);
        const usize size = put_frame(bytes, 0U, kBlob, 7U, 16U);
        const FrameStep step = assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        CHECK(step.frame.body.data() == bytes + size - 16U);
        CHECK(step.frame.object == 7U);
        CHECK(step.frame.buffer == nullptr);
        CHECK(session.buffers.total == 0U);
        session.admission.finish(step.frame);

        // A header split across feeds with its body after it is still a view of the last feed.
        const usize part = 2U;
        FrameStep head = assembler.feed(ConstSpan<u8>(bytes, part), Delivery::Established, 1U);
        CHECK(head.verdict == AdmitVerdict::NeedMore);
        CHECK(head.consumed == part);
        const FrameStep rest = assembler.feed(ConstSpan<u8>(bytes + part, size - part), Delivery::Established, 1U);
        REQUIRE(rest.verdict == AdmitVerdict::Admitted);
        CHECK(rest.consumed == size - part);
        CHECK(rest.frame.body.data() == bytes + size - 16U);
        CHECK(session.buffers.total == 0U);
        session.admission.finish(rest.frame);

        // A body split across feeds takes one buffer of its exact length, held against the quota until finished.
        const usize cut = size - 4U;
        const FrameStep first = assembler.feed(ConstSpan<u8>(bytes, cut), Delivery::Established, 2U);
        CHECK(first.verdict == AdmitVerdict::NeedMore);
        CHECK(first.consumed == cut);
        CHECK(session.buffers.live == 1U);
        CHECK(session.quota.reassembly.held() == 16U);
        CHECK(session.quota.pending.held() == 1U);
        const FrameStep last = assembler.feed(ConstSpan<u8>(bytes + cut, 4U), Delivery::Established, 2U);
        REQUIRE(last.verdict == AdmitVerdict::Admitted);
        CHECK(last.frame.buffer != nullptr);
        CHECK(last.frame.body.size() == 16U);
        CHECK(session.quota.reassembly.held() == 16U);
        session.admission.finish(last.frame);
        CHECK(session.buffers.live == 0U);
        CHECK(session.quota.reassembly.held() == 0U);

        // Abandoned mid-body, the stream gives everything back.
        const FrameStep partial = assembler.feed(ConstSpan<u8>(bytes, cut), Delivery::Established, 3U);
        CHECK(partial.verdict == AdmitVerdict::NeedMore);
        CHECK(session.buffers.live == 1U);
    }
    CHECK(session.buffers.live == 0U);
    CHECK(session.quota.reassembly.held() == 0U);
    CHECK(session.quota.pending.held() == 0U);
    CHECK(session.quota.streams.held() == 0U);
}

TEST_CASE("Streams: the reassembly quota and the allocator hold a large frame back", "[net][stream]")
{
    QuotaLimits limits;
    limits.reassembly_bytes = 8000U;
    Session session(limits);
    static u8 bytes[10000];
    const usize size = put_frame(bytes, 0U, kBlob, 1U, 5000U);

    StreamAssembler first(session.admission);
    open(first, kBulk);
    // Priority 4 has half of the 8,000.
    FrameStep step = first.feed(ConstSpan<u8>(bytes, 100U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::ReassemblyFull);
    CHECK(step.action == AdmitAction::Wait);
    CHECK(step.consumed == 0U);
    CHECK(step.retry_at_ns == kNeverNs);
    CHECK(session.buffers.total == 0U);
    CHECK(session.quota.pending.held() == 0U);

    // The same frame all in hand needs no reservation.
    step = first.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
    REQUIRE(step.verdict == AdmitVerdict::Admitted);
    session.admission.finish(step.frame);

    // A frame within its share, then the second one that would pass it.
    const usize fits = put_frame(bytes, 0U, kBlob, 1U, 3000U);
    step = first.feed(ConstSpan<u8>(bytes, 100U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NeedMore);
    CHECK(session.quota.reassembly.held() == 3000U);
    StreamAssembler second(session.admission);
    open(second, kBulk);
    step = second.feed(ConstSpan<u8>(bytes, 100U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::ReassemblyFull);

    CHECK(fits > 100U);

    // When the allocator refuses what the quota allowed, the reader waits and nothing leaks.
    Session starving;
    starving.buffers.refuse_all = true;
    StreamAssembler waiting(starving.admission);
    open(waiting, kBulk);
    step = waiting.feed(ConstSpan<u8>(bytes, 100U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NoMemory);
    CHECK(step.action == AdmitAction::Wait);
    CHECK(step.consumed == 0U);
    CHECK(step.retry_at_ns == kNeverNs);
    CHECK(starving.quota.reassembly.held() == 0U);
    CHECK(starving.quota.pending.held() == 0U);
    starving.buffers.refuse_all = false;
    step = waiting.feed(ConstSpan<u8>(bytes, 100U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NeedMore);
    CHECK(step.consumed == 100U);
}

// ---- Datagrams -----------------------------------------------------------------------------------------------------

namespace
{
struct Datagrams
{
    explicit Datagrams(Session& s, u32 channel, u64 timeout = kSecond)
        : session(s), assembler(s.admission, channel, slots, timeout)
    {
    }

    // Reads one datagram; returns the last step, leaving delivered frames to `on_frame`.
    template <typename F>
    FrameStep read(ConstSpan<u8> bytes, u64 now, const F& on_frame, Delivery delivery = Delivery::Established)
    {
        FrameStep last;
        DatagramCursor cursor;
        last.verdict = session.admission.open_datagram(bytes, now, cursor);
        if (last.verdict != AdmitVerdict::Admitted)
        {
            return last;
        }
        while (cursor.more)
        {
            last = assembler.next(cursor, delivery, now);
            if (last.action == AdmitAction::Deliver)
            {
                on_frame(last.frame);
                session.admission.finish(last.frame);
            }
        }
        return last;
    }

    FrameStep read(ConstSpan<u8> bytes, u64 now = 0U)
    {
        return read(bytes, now, [](const AdmittedFrame&) {});
    }

    Session& session;
    FragmentSlot slots[4];
    DatagramAssembler assembler;
};

// One fragment of a message of `total` bytes, whose every byte is its offset's low byte plus `seed`.
usize put_fragment(Span<u8> out, u32 channel, const MessageDesc& message, u64 object, u32 total, u32 chunk,
                   u64 sequence, u32 index, u8 seed = 0U)
{
    static u8 body[100000];
    for (u32 i = 0U; i < total; ++i)
    {
        body[i] = static_cast<u8>(i + seed);
    }
    FrameHeader header;
    header.message = message.id;
    header.length = total;
    header.object = object;
    header.chunk = chunk;
    header.sequence = sequence;
    header.index = index;
    const usize used =
        write_fragment(channel, FrameShape{message.operation != 0U, false}, header, ConstSpan<u8>(body, total), out);
    REQUIRE(used != 0U);
    return used;
}

bool body_matches(ConstSpan<u8> body, u8 seed = 0U)
{
    for (usize i = 0U; i < body.size(); ++i)
    {
        if (body[i] != static_cast<u8>(i + seed))
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("Datagrams: a whole message, its carrier and its refusals", "[net][datagram]")
{
    Session session;
    Datagrams state(session, kState);
    u8 bytes[4096];

    u32 delivered = 0U;
    const usize size = put_whole(bytes, kState, kSnapshot, 5U, 40U);
    FrameStep step = state.read(ConstSpan<u8>(bytes, size), 0U,
                                [&](const AdmittedFrame& frame)
                                {
                                    ++delivered;
                                    CHECK(frame.body.size() == 40U);
                                    CHECK(frame.body.data() == bytes + size - 40U);
                                    CHECK(frame.object == 5U);
                                });
    CHECK(step.verdict == AdmitVerdict::Admitted);
    CHECK(delivered == 1U);
    CHECK(session.buffers.total == 0U);

    // An empty one: the route alone.
    u8 route[4];
    CHECK(write_route(Route{kState, RouteForm::Whole}, route) == 1U);
    step = state.read(ConstSpan<u8>(route, 1U));
    CHECK(step.verdict == AdmitVerdict::Malformed);
    CHECK(step.action == AdmitAction::Skip);

    DatagramCursor cursor;
    CHECK(session.admission.open_datagram(ConstSpan<u8>(route, 0U), 0U, cursor) == AdmitVerdict::Malformed);
    const u8 beyond[] = {0x40U};
    CHECK(session.admission.open_datagram(beyond, 0U, cursor) == AdmitVerdict::BadChannel);
    u8 reliable[4];
    (void)write_route(Route{kControl, RouteForm::Whole}, reliable);
    CHECK(session.admission.open_datagram(ConstSpan<u8>(reliable, 1U), 0U, cursor) == AdmitVerdict::WrongCarrier);
    CHECK(session.admission.count(AdmitVerdict::BadChannel) == 1U);
    CHECK(session.admission.count(AdmitVerdict::WrongCarrier) == 1U);

    // Too long for the channel, too short for the message, of another channel: dropped, no session ended.
    const usize too_long = put_whole(bytes, kState, kSnapshot, 5U, 2001U);
    step = state.read(ConstSpan<u8>(bytes, too_long));
    CHECK(step.verdict == AdmitVerdict::Oversized);
    CHECK(step.action == AdmitAction::Skip);
    const usize other = put_whole(bytes, kState, kVoice, 5U, 10U);
    CHECK(state.read(ConstSpan<u8>(bytes, other)).verdict == AdmitVerdict::OtherChannel);

    // Datagrams per second, before anything of the message is read; a control-priority channel is exempt.
    QuotaLimits limits;
    limits.datagrams_per_s = 1U;
    limits.tolerance_ns = 0U;
    Session narrow(limits);
    Datagrams narrow_state(narrow, kState);
    const usize good = put_whole(bytes, kState, kSnapshot, 5U, 10U);
    CHECK(narrow_state.read(ConstSpan<u8>(bytes, good)).verdict == AdmitVerdict::Admitted);
    CHECK(narrow_state.read(ConstSpan<u8>(bytes, good)).verdict == AdmitVerdict::PrincipalDatagrams);
    CHECK(narrow.admission.count(AdmitVerdict::PrincipalDatagrams) == 1U);
    CHECK(narrow_state.read(ConstSpan<u8>(bytes, good), kSecond).verdict == AdmitVerdict::Admitted);

    // S7 on a datagram.
    Capability read_only = capability_for(session.authorized, 1ULL);
    session.admission.set_capability(&read_only);
    const usize refused = put_whole(bytes, kState, kSnapshot, 5U, 10U);
    step = state.read(ConstSpan<u8>(bytes, refused));
    CHECK(step.verdict == AdmitVerdict::NotGranted);
    CHECK(step.action == AdmitAction::Skip);
    CHECK(session.quota.pending.held() == 0U);
}

TEST_CASE("Datagrams: several messages in one, and what breaks the list", "[net][datagram]")
{
    Session session;
    Datagrams state(session, kState);
    u8 bytes[1024];

    usize size = write_route(Route{kState, RouteForm::Listed}, bytes);
    size = put_frame(bytes, size, kSnapshot, 1U, 10U, 1U);
    size = put_frame(bytes, size, kSnapshot, 2U, 0U);
    size = put_frame(bytes, size, kSnapshot, 3U, 20U, 3U);

    u32 delivered = 0U;
    u64 objects = 0U;
    const FrameStep last = state.read(ConstSpan<u8>(bytes, size), 0U,
                                      [&](const AdmittedFrame& frame)
                                      {
                                          ++delivered;
                                          objects += frame.object;
                                          for (const u8 byte : frame.body)
                                          {
                                              REQUIRE(byte == static_cast<u8>(frame.object));
                                          }
                                      });
    CHECK(last.verdict == AdmitVerdict::Admitted);
    CHECK(delivered == 3U);
    CHECK(objects == 6U);
    CHECK(session.buffers.total == 0U);

    // A single message in the list form is the whole form's.
    usize single = write_route(Route{kState, RouteForm::Listed}, bytes);
    single = put_frame(bytes, single, kSnapshot, 1U, 10U);
    CHECK(state.read(ConstSpan<u8>(bytes, single)).verdict == AdmitVerdict::Malformed);

    // A length past the end loses what follows.
    usize past = write_route(Route{kState, RouteForm::Listed}, bytes);
    past = put_frame(bytes, past, kSnapshot, 1U, 10U);
    past = put_frame(bytes, past, kSnapshot, 2U, 10U) - 1U;
    delivered = 0U;
    CHECK(state.read(ConstSpan<u8>(bytes, past), 0U, [&](const AdmittedFrame&) { ++delivered; }).verdict ==
          AdmitVerdict::Malformed);
    CHECK(delivered == 1U);

    // A truncated header too.
    usize cut = write_route(Route{kState, RouteForm::Listed}, bytes);
    cut = put_frame(bytes, cut, kSnapshot, 1U, 10U);
    bytes[cut] = 0x83U; // a number with no end
    delivered = 0U;
    CHECK(state.read(ConstSpan<u8>(bytes, cut + 1U), 0U, [&](const AdmittedFrame&) { ++delivered; }).verdict ==
          AdmitVerdict::Malformed);
    CHECK(delivered == 1U);

    // One refused message does not lose the next.
    Capability one_object = capability_for(session.authorized);
    one_object.grant.ranges[0] = crd::crypto::ObjectRange{2U, 2U};
    one_object.grant.range_count = 1U;
    session.admission.set_capability(&one_object);
    usize mixed = write_route(Route{kState, RouteForm::Listed}, bytes);
    mixed = put_frame(bytes, mixed, kSnapshot, 1U, 10U);
    mixed = put_frame(bytes, mixed, kSnapshot, 2U, 10U);
    objects = 0U;
    CHECK(state.read(ConstSpan<u8>(bytes, mixed), 0U, [&](const AdmittedFrame& frame) { objects += frame.object; })
              .verdict == AdmitVerdict::Admitted);
    CHECK(objects == 2U);
    CHECK(session.admission.count(AdmitVerdict::NotGranted) == 1U);
}

TEST_CASE("Datagrams: fragments in any order, each message once", "[net][datagram]")
{
    Session session;
    Datagrams media(session, kMedia);
    u8 bytes[2048];

    const u32 total = 3000U;
    const u32 chunk = 1000U;
    u32 got = 0U;
    const auto on_frame = [&](const AdmittedFrame& frame)
    {
        ++got;
        CHECK(frame.length == total);
        CHECK(body_matches(frame.body));
        CHECK(frame.object == 9U);
        CHECK(frame.buffer != nullptr);
    };

    const u32 order[] = {2U, 0U, 1U};
    FrameStep step;
    for (u32 i = 0U; i < 3U; ++i)
    {
        const usize size = put_fragment(bytes, kMedia, kVoice, 9U, total, chunk, 1U, order[i]);
        step = media.read(ConstSpan<u8>(bytes, size), 0U, on_frame);
        if (i < 2U)
        {
            CHECK(step.verdict == AdmitVerdict::Buffered);
            CHECK(step.action == AdmitAction::Continue);
            CHECK(session.quota.reassembly.held() == total);
            CHECK(session.buffers.live == 1U);
        }
    }
    CHECK(step.verdict == AdmitVerdict::Admitted);
    CHECK(got == 1U);
    CHECK(session.buffers.live == 0U);
    CHECK(session.buffers.total == 1U);
    CHECK(session.quota.reassembly.held() == 0U);
    CHECK(session.quota.pending.held() == 0U);

    // Any piece of it again is stale, and takes nothing.
    const usize again = put_fragment(bytes, kMedia, kVoice, 9U, total, chunk, 1U, 0U);
    step = media.read(ConstSpan<u8>(bytes, again), 0U, on_frame);
    CHECK(step.verdict == AdmitVerdict::StaleFragment);
    CHECK(session.buffers.total == 1U);
    CHECK(got == 1U);

    // A piece repeated while its message is open is a duplicate, written once.
    const usize one = put_fragment(bytes, kMedia, kVoice, 9U, total, chunk, 2U, 1U);
    CHECK(media.read(ConstSpan<u8>(bytes, one)).verdict == AdmitVerdict::Buffered);
    const usize altered = put_fragment(bytes, kMedia, kVoice, 9U, total, chunk, 2U, 1U, 0x40U);
    CHECK(media.read(ConstSpan<u8>(bytes, altered)).verdict == AdmitVerdict::DuplicateFragment);

    // Pieces that disagree with their first.
    const usize tone = put_fragment(bytes, kMedia, kTone, 9U, total, chunk, 2U, 0U);
    CHECK(media.read(ConstSpan<u8>(bytes, tone)).verdict == AdmitVerdict::FragmentConflict);
    const usize object = put_fragment(bytes, kMedia, kVoice, 8U, total, chunk, 2U, 0U);
    CHECK(media.read(ConstSpan<u8>(bytes, object)).verdict == AdmitVerdict::FragmentConflict);
    const usize length = put_fragment(bytes, kMedia, kVoice, 9U, total + 1U, chunk, 2U, 0U);
    CHECK(media.read(ConstSpan<u8>(bytes, length)).verdict == AdmitVerdict::FragmentConflict);
    const usize size_c = put_fragment(bytes, kMedia, kVoice, 9U, total, chunk + 1U, 2U, 0U);
    CHECK(media.read(ConstSpan<u8>(bytes, size_c)).verdict == AdmitVerdict::FragmentConflict);
    {
        const usize early = put_fragment(bytes, kMedia, kVoice, 9U, total, chunk, 2U, 0U);
        const FrameStep replayed = media.read(ConstSpan<u8>(bytes, early), 0U, on_frame, Delivery::Early);
        CHECK(replayed.verdict == AdmitVerdict::FragmentConflict);
    }

    // The rest of it completes it.
    got = 0U;
    for (u32 index : {0U, 2U})
    {
        const usize size = put_fragment(bytes, kMedia, kVoice, 9U, total, chunk, 2U, index);
        step = media.read(ConstSpan<u8>(bytes, size), 0U, on_frame);
    }
    CHECK(step.verdict == AdmitVerdict::Admitted);
    CHECK(got == 1U);
}

TEST_CASE("Datagrams: a fragment no encoder writes", "[net][datagram]")
{
    Session session;
    Datagrams media(session, kMedia);
    u8 bytes[2048];

    const auto send = [&](u32 total, u32 chunk, u32 index, usize piece)
    {
        usize route = write_route(Route{kMedia, RouteForm::Fragment}, bytes);
        FrameHeader header;
        header.message = kVoice.id;
        header.length = total;
        header.object = 1U;
        header.chunk = chunk;
        header.sequence = 1U;
        header.index = index;
        route += write_frame_header(HeaderForm::Fragment, FrameShape{true, false}, header,
                                    Span<u8>(bytes + route, sizeof(bytes) - route));
        return media.read(ConstSpan<u8>(bytes, route + piece)).verdict;
    };
    CHECK(send(1000U, 400U, 3U, 200U) == AdmitVerdict::BadFragment);
    CHECK(send(1000U, 400U, 2U, 400U) == AdmitVerdict::BadFragment);
    CHECK(send(1000U, 400U, 0U, 399U) == AdmitVerdict::BadFragment);
    CHECK(send(400U, 400U, 0U, 400U) == AdmitVerdict::BadFragment);
    CHECK(send(200U, 63U, 0U, 63U) == AdmitVerdict::BadFragment);
    CHECK(send(100000U, 64U, 0U, 64U) == AdmitVerdict::BadFragment);
    CHECK(send(100001U, 1000U, 0U, 1000U) == AdmitVerdict::Oversized); // past the channel's most
    CHECK(session.buffers.total == 0U);

    // A refused first piece reserves nothing and takes no slot.
    Capability read_only = capability_for(session.authorized, 1ULL);
    session.admission.set_capability(&read_only);
    CHECK(send(1000U, 400U, 0U, 400U) == AdmitVerdict::NotGranted);
    CHECK(session.buffers.total == 0U);
    CHECK(session.quota.reassembly.held() == 0U);
    CHECK(session.quota.pending.held() == 0U);
    session.admission.set_capability(nullptr);
    CHECK(send(1000U, 400U, 0U, 400U) == AdmitVerdict::NoCapability);
    CHECK(session.buffers.total == 0U);
}

TEST_CASE("Datagrams: partial messages end: time, room, supersession, quota", "[net][datagram]")
{
    u8 bytes[2048];

    SECTION("time gives a partial message up, and its pieces stay stale")
    {
        Session session;
        Datagrams media(session, kMedia, 100U);
        const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 5U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, one), 1000U).verdict == AdmitVerdict::Buffered);
        media.assembler.sweep(1099U);
        CHECK(session.buffers.live == 1U);
        media.assembler.sweep(1100U);
        CHECK(session.buffers.live == 0U);
        CHECK(session.quota.reassembly.held() == 0U);
        CHECK(session.quota.pending.held() == 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, one), 2000U).verdict == AdmitVerdict::StaleFragment);
        CHECK(session.buffers.total == 1U);
    }
    SECTION("reading a later datagram also gives it up")
    {
        Session session;
        Datagrams media(session, kMedia, 100U);
        const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 5U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, one), 0U).verdict == AdmitVerdict::Buffered);
        const usize other = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 6U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, other), 100U).verdict == AdmitVerdict::Buffered);
        CHECK(session.buffers.live == 1U);
    }
    SECTION("no room gives up the oldest for a newer one, never for an older one")
    {
        Session session;
        Datagrams media(session, kMedia);
        for (u64 sequence = 10U; sequence < 14U; ++sequence)
        {
            const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, sequence, 0U);
            CHECK(media.read(ConstSpan<u8>(bytes, one)).verdict == AdmitVerdict::Buffered);
        }
        CHECK(session.buffers.live == 4U);
        const usize older = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 9U, 0U);
        const FrameStep refused = media.read(ConstSpan<u8>(bytes, older));
        CHECK(refused.verdict == AdmitVerdict::ReassemblyFull);
        CHECK(refused.action == AdmitAction::Skip);
        CHECK(session.buffers.live == 4U);
        const usize newer = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 14U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, newer)).verdict == AdmitVerdict::Buffered);
        CHECK(session.buffers.live == 4U);
        const usize given_up = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 10U, 1U);
        CHECK(media.read(ConstSpan<u8>(bytes, given_up)).verdict == AdmitVerdict::StaleFragment);
    }
    SECTION("a state channel's newest message makes every older one stale")
    {
        Session session;
        Datagrams state(session, kState);
        const usize open_older = put_fragment(bytes, kState, kSnapshot, 1U, 1000U, 400U, 3U, 0U);
        CHECK(state.read(ConstSpan<u8>(bytes, open_older)).verdict == AdmitVerdict::Buffered);
        for (u32 index = 0U; index < 3U; ++index)
        {
            const usize one = put_fragment(bytes, kState, kSnapshot, 1U, 1000U, 400U, 4U, index);
            (void)state.read(ConstSpan<u8>(bytes, one));
        }
        CHECK(session.buffers.live == 0U);
        const usize late = put_fragment(bytes, kState, kSnapshot, 1U, 1000U, 400U, 3U, 1U);
        CHECK(state.read(ConstSpan<u8>(bytes, late)).verdict == AdmitVerdict::StaleFragment);
        const usize never_seen = put_fragment(bytes, kState, kSnapshot, 1U, 1000U, 400U, 2U, 1U);
        CHECK(state.read(ConstSpan<u8>(bytes, never_seen)).verdict == AdmitVerdict::StaleFragment);

        // Only older ones go: a newer partial message survives an older one's completion and completes after it.
        u32 got = 0U;
        const usize newer_piece = put_fragment(bytes, kState, kSnapshot, 1U, 1000U, 400U, 6U, 0U);
        CHECK(state.read(ConstSpan<u8>(bytes, newer_piece)).verdict == AdmitVerdict::Buffered);
        for (u32 index = 0U; index < 3U; ++index)
        {
            const usize one = put_fragment(bytes, kState, kSnapshot, 1U, 1000U, 400U, 5U, index);
            (void)state.read(ConstSpan<u8>(bytes, one), 0U, [&](const AdmittedFrame&) { ++got; });
        }
        CHECK(got == 1U);
        CHECK(session.buffers.live == 1U);
        for (u32 index = 1U; index < 3U; ++index)
        {
            const usize one = put_fragment(bytes, kState, kSnapshot, 1U, 1000U, 400U, 6U, index);
            (void)state.read(ConstSpan<u8>(bytes, one), 0U, [&](const AdmittedFrame&) { ++got; });
        }
        CHECK(got == 2U);
    }
    SECTION("a media channel still takes an older message inside its window")
    {
        Session session;
        Datagrams media(session, kMedia);
        u32 got = 0U;
        for (u64 sequence : {100U, 99U})
        {
            for (u32 index = 0U; index < 3U; ++index)
            {
                const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, sequence, index);
                (void)media.read(ConstSpan<u8>(bytes, one), 0U, [&](const AdmittedFrame&) { ++got; });
            }
        }
        CHECK(got == 2U);
        // Outside the window nobody can tell it was not delivered.
        const usize far_low = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 35U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, far_low)).verdict == AdmitVerdict::StaleFragment);
        const usize in_window = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 36U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, in_window)).verdict == AdmitVerdict::Buffered);
        // A jump past the window forgets it.
        for (u32 index = 0U; index < 3U; ++index)
        {
            const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 1000U, index);
            (void)media.read(ConstSpan<u8>(bytes, one));
        }
        const usize just_behind = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 999U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, just_behind)).verdict == AdmitVerdict::Buffered);
        for (u32 index = 0U; index < 3U; ++index)
        {
            const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 1001U, index);
            (void)media.read(ConstSpan<u8>(bytes, one));
        }
        const usize repeat = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 1000U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, repeat)).verdict == AdmitVerdict::StaleFragment);
    }
    SECTION("the window's edges")
    {
        const auto complete = [&](Datagrams& media, u64 sequence)
        {
            u32 got = 0U;
            for (u32 index = 0U; index < 3U; ++index)
            {
                const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, sequence, index);
                (void)media.read(ConstSpan<u8>(bytes, one), 0U, [&](const AdmittedFrame&) { ++got; });
            }
            return got;
        };
        const auto first_piece = [&](Datagrams& media, u64 sequence)
        {
            const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, sequence, 0U);
            return media.read(ConstSpan<u8>(bytes, one)).verdict;
        };
        {
            // An older message finished below the newest is remembered.
            Session session;
            Datagrams media(session, kMedia);
            CHECK(complete(media, 100U) == 1U);
            CHECK(complete(media, 99U) == 1U);
            CHECK(first_piece(media, 99U) == AdmitVerdict::StaleFragment);
            CHECK(first_piece(media, 98U) == AdmitVerdict::Buffered);
            // Past the window nobody can tell, whatever the window's bits say.
            CHECK(first_piece(media, 35U) == AdmitVerdict::StaleFragment);
        }
        {
            Session session;
            Datagrams media(session, kMedia);
            CHECK(complete(media, 100U) == 1U);
            CHECK(first_piece(media, 35U) == AdmitVerdict::StaleFragment);
            CHECK(first_piece(media, 36U) == AdmitVerdict::Buffered);
        }
        {
            // A jump of exactly the window still remembers the old newest, at the window's last place.
            Session session;
            Datagrams media(session, kMedia);
            CHECK(complete(media, 100U) == 1U);
            CHECK(complete(media, 164U) == 1U);
            CHECK(first_piece(media, 100U) == AdmitVerdict::StaleFragment);
            CHECK(first_piece(media, 101U) == AdmitVerdict::Buffered);
        }
        {
            // A message at the window's last place when it finishes is remembered too.
            Session session;
            Datagrams media(session, kMedia);
            CHECK(first_piece(media, 100U) == AdmitVerdict::Buffered);
            CHECK(complete(media, 164U) == 1U);
            for (u32 index = 1U; index < 3U; ++index)
            {
                const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 100U, index);
                (void)media.read(ConstSpan<u8>(bytes, one));
            }
            CHECK(first_piece(media, 100U) == AdmitVerdict::StaleFragment);
        }
        {
            // A timeout at the far end of the clock never expires.
            Session session;
            Datagrams media(session, kMedia, kNeverNs);
            const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 1U, 0U);
            CHECK(media.read(ConstSpan<u8>(bytes, one), 5U).verdict == AdmitVerdict::Buffered);
            media.assembler.sweep(kNeverNs - 1U);
            CHECK(session.buffers.live == 1U);
        }
    }
    SECTION("the reassembly quota and the allocator bound what partial messages hold")
    {
        QuotaLimits limits;
        limits.reassembly_bytes = 2000U;
        Session session(limits);
        Datagrams media(session, kMedia);
        // Priority 5 has three eighths of the 2,000: 750 bytes.
        const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 700U, 400U, 1U, 0U);
        CHECK(media.read(ConstSpan<u8>(bytes, one)).verdict == AdmitVerdict::Buffered);
        const usize two = put_fragment(bytes, kMedia, kVoice, 1U, 700U, 400U, 2U, 0U);
        const FrameStep full = media.read(ConstSpan<u8>(bytes, two));
        CHECK(full.verdict == AdmitVerdict::ReassemblyFull);
        CHECK(full.action == AdmitAction::Skip);

        Session starving;
        starving.buffers.refuse_all = true;
        Datagrams refused(starving, kMedia);
        const FrameStep none = refused.read(ConstSpan<u8>(bytes, two));
        CHECK(none.verdict == AdmitVerdict::NoMemory);
        CHECK(none.action == AdmitAction::Skip);
        CHECK(starving.quota.reassembly.held() == 0U);
        CHECK(starving.quota.pending.held() == 0U);
    }
    SECTION("an assembler gone gives everything back")
    {
        Session session;
        {
            Datagrams media(session, kMedia);
            for (u64 sequence = 1U; sequence < 4U; ++sequence)
            {
                const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, sequence, 0U);
                CHECK(media.read(ConstSpan<u8>(bytes, one)).verdict == AdmitVerdict::Buffered);
            }
            CHECK(session.quota.reassembly.held() == 3000U);
        }
        CHECK(session.buffers.live == 0U);
        CHECK(session.quota.reassembly.held() == 0U);
        CHECK(session.quota.pending.held() == 0U);
    }
    SECTION("no slots at all refuses a fragment and nothing else")
    {
        Session session;
        DatagramAssembler bare(session.admission, kMedia, Span<FragmentSlot>{}, kSecond);
        const usize one = put_fragment(bytes, kMedia, kVoice, 1U, 1000U, 400U, 1U, 0U);
        DatagramCursor cursor;
        REQUIRE(session.admission.open_datagram(ConstSpan<u8>(bytes, one), 0U, cursor) == AdmitVerdict::Admitted);
        CHECK(bare.next(cursor, Delivery::Established, 0U).verdict == AdmitVerdict::ReassemblyFull);
    }
}

TEST_CASE("Datagrams: a hostile path delivers each message at most once, intact", "[net][datagram]")
{
    // Loss, duplication and reordering of every fragment, on several seeds. A message is delivered whole and right or
    // not at all; nothing is delivered twice; with no loss every message in the window arrives; all that was held comes
    // back.
    struct Packet
    {
        u32 size = 0U;
        u8 bytes[1500];
    };
    static Packet packets[2048];

    constexpr u32 messages_sent = 60U;
    for (u32 loss_percent : {0U, 5U, 30U})
    {
        for (u64 seed = 1U; seed <= 8U; ++seed)
        {
            QuotaLimits limits;
            limits.datagrams_per_s = 10000000U;
            Session session(limits);
            session.capability.grant.budget.operations_per_second = 1000000U;
            Mix mix{(seed * 1000U) + loss_percent};
            FragmentSlot slots[32];
            DatagramAssembler media(session.admission, kMedia, slots, 3600U * kSecond);

            u32 count = 0U;
            for (u32 message = 0U; message < messages_sent; ++message)
            {
                const u32 total = 1000U + static_cast<u32>(mix.next() % 9000U);
                const u32 chunk = 400U + static_cast<u32>(mix.next() % 500U);
                const u32 pieces = fragment_count(total, chunk);
                for (u32 index = 0U; index < pieces; ++index)
                {
                    const u32 copies = (mix.next() % 100U < 10U) ? 2U : 1U;
                    for (u32 copy = 0U; copy < copies; ++copy)
                    {
                        REQUIRE(count < 2048U);
                        packets[count].size = static_cast<u32>(put_fragment(Span<u8>(packets[count].bytes, 1500U),
                                                                            kMedia, kVoice, message, total, chunk,
                                                                            message, index, static_cast<u8>(message)));
                        ++count;
                    }
                }
            }
            // A reordering window of 24 datagrams: message order is kept loosely, as a real path keeps it.
            for (u32 i = 0U; i + 1U < count; ++i)
            {
                const u32 span = ((count - i) < 24U) ? (count - i) : 24U;
                const u32 other = i + static_cast<u32>(mix.next() % span);
                if (other != i)
                {
                    static Packet swap;
                    std::memcpy(&swap, &packets[i], sizeof(Packet));
                    std::memcpy(&packets[i], &packets[other], sizeof(Packet));
                    std::memcpy(&packets[other], &swap, sizeof(Packet));
                }
            }

            u8 seen[messages_sent] = {};
            u32 delivered = 0U;
            u64 now = 0U;
            for (u32 i = 0U; i < count; ++i)
            {
                now += 1000U;
                if ((mix.next() % 100U) < loss_percent)
                {
                    continue;
                }
                DatagramCursor cursor;
                REQUIRE(session.admission.open_datagram(ConstSpan<u8>(packets[i].bytes, packets[i].size), now,
                                                        cursor) == AdmitVerdict::Admitted);
                const FrameStep step = media.next(cursor, Delivery::Established, now);
                if (step.action == AdmitAction::Deliver)
                {
                    const u64 message = step.frame.object;
                    REQUIRE(message < messages_sent);
                    REQUIRE(seen[message] == 0U);
                    seen[message] = 1U;
                    REQUIRE(body_matches(step.frame.body, static_cast<u8>(message)));
                    ++delivered;
                    session.admission.finish(step.frame);
                }
            }
            if (loss_percent == 0U)
            {
                CHECK(delivered == messages_sent);
            }
            media.sweep(kNeverNs);
            CHECK(session.buffers.live == 0U);
            CHECK(session.quota.reassembly.held() == 0U);
            CHECK(session.quota.pending.held() == 0U);
        }
    }
}

TEST_CASE("Admission: two sessions of one principal share its quota", "[net][admission]")
{
    QuotaLimits limits;
    limits.messages_per_s = 2U;
    limits.tolerance_ns = 0U;
    limits.streams = 2U;
    PrincipalQuota quota{limits};
    Buffers buffers;
    AuthorizedSchema authorized = authorized_for(kGame);
    Capability capability = capability_for(authorized);

    ChannelState first_channels[16];
    ChannelState second_channels[16];
    Admission first(quota, buffers, first_channels);
    Admission second(quota, buffers, second_channels);
    const SessionSchema schemas[] = {{&kGame, &authorized}};
    for (Admission* admission : {&first, &second})
    {
        REQUIRE(admission->bind(schemas));
        REQUIRE(admission->announce(ConstSpan<u8>(kGame.id, sizeof(kGame.id)), 3U, 1U));
        admission->set_capability(&capability);
    }

    u8 bytes[64];
    const usize size = put_frame(bytes, 0U, kBlob, 1U, 8U);
    StreamAssembler one(first);
    StreamAssembler two(second);
    open(one, kBulk);
    open(two, kBulk);
    CHECK(quota.streams.held() == 2U);
    FrameStep step = one.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
    REQUIRE(step.verdict == AdmitVerdict::Admitted);
    first.finish(step.frame);
    step = two.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::PrincipalMessages);
    CHECK(step.retry_at_ns == kSecond / 2U);
    // Each session's channel gates are its own: the first's spending left the second's clear.
    step = two.feed(ConstSpan<u8>(bytes, size), Delivery::Established, kSecond / 2U);
    REQUIRE(step.verdict == AdmitVerdict::Admitted);
    second.finish(step.frame);

    // The capability's objects budget is held through the principal's gauge.
    CHECK(quota.objects.try_add(100U, capability.grant.budget.objects));
    CHECK_FALSE(quota.objects.try_add(1U, capability.grant.budget.objects));
    quota.objects.release(100U);
}

TEST_CASE("Frames: the generated schema's writer agrees with admission's reader", "[net][frames]")
{
    test_schema::Scalars scalars;
    scalars.flag = true;
    u8 out[256];
    usize written = 0U;
    REQUIRE(write_frame(test_schema::kSchema, HeaderForm::Sized, scalars, test_schema::kMinor, 77U, out, written) ==
            WireError::Ok);
    CHECK(written > 3U);
    CHECK(out[0] == 1U); // the message
    // The length counts the object and the hash too.
    CHECK(out[1] == crd::net::wire::encoded_bytes(scalars, test_schema::kMinor) + 1U + crd::net::kFrameHashBytes);
    CHECK(out[2] == 77U); // its object: it has an operation
    CHECK(written == 2U + out[1]);

    // Too little room, at each boundary, writes nothing.
    for (usize size = 0U; size < written; ++size)
    {
        usize none = 1U;
        WireError error = write_frame(test_schema::kSchema, HeaderForm::Sized, scalars, test_schema::kMinor, 77U,
                                      Span<u8>(out, size), none);
        CHECK(error == WireError::NoRoom);
        CHECK(none == 0U);
    }

    // A message of no schema is no frame.
    control::StateHash foreign;
    usize none = 1U;
    const WireSchema empty{};
    CHECK(write_frame(empty, HeaderForm::Sized, foreign, 0U, 0U, out, none) == WireError::OutOfRange);
    CHECK(none == 0U);

    // ToEnd has no length.
    REQUIRE(write_frame(test_schema::kSchema, HeaderForm::ToEnd, scalars, test_schema::kMinor, 77U, out, written) ==
            WireError::Ok);
    CHECK(out[1] == 77U);

    // find_message over a table with gaps.
    CHECK(crd::net::wire::find_message(kGame, 0U) == nullptr);
    CHECK(crd::net::wire::find_message(kGame, 1U) == &kMessages[0]);
    CHECK(crd::net::wire::find_message(kGame, 8U) == &kMessages[7]);
    CHECK(crd::net::wire::find_message(kGame, 10U) == nullptr);
    CHECK(crd::net::wire::find_message(kGame, 300U) == &kMessages[9]);
    CHECK(crd::net::wire::find_message(kGame, 301U) == nullptr);
}
