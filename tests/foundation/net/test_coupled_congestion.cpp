// crd-net -- coupled congestion control (NET-PATH; docs/design/network-foundation.md#multipath, RFC 6356 sections 3
// and 4, RFC 10069 section 5.3): the policy rules and the coupling's membership through builds, swaps and
// destruction; one member alone growing as NewReno to the byte; the integer divisor against equations 1 and 2 in
// floating point, with unequal windows and RTTs, flights and unsampled members, RTT ratios past the bound and windows
// at kCongestionMaxWindow; and over simulated bottlenecks the RFC's goals: two coupled paths through one bottleneck
// beside a NewReno flow take about one flow's share (uncoupled, about two), disjoint links are both filled, a lossier
// path carries less than it does uncoupled with the total at least the better path's alone, and so with unequal RTTs.

#include "bottleneck_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using crd::u64;
using crd::usize;
using namespace crd::net;
using crd::net::testing::Flow;
using crd::net::testing::kBottleneckMss;
using crd::net::testing::kBottleneckSecond;
using crd::net::testing::Link;
using crd::net::testing::run_flows;

constexpr u64 kMs = 1000000U;
constexpr u64 kRate = 1250000U; // 10 Mbit/s

CongestionPolicy reno(bool coupled = true)
{
    CongestionPolicy policy;
    policy.algorithm = CongestionAlgorithm::NewReno;
    policy.coupled = coupled;
    return policy;
}

// A controller built in its own slot, destroyed with it.
struct Built
{
    Built(const CongestionPolicy& policy, CongestionCoupling* coupling, u64 window = 0U)
    {
        CongestionState seed;
        seed.window = window;
        seed.slow_start_threshold = window;
        ctl = make_congestion(policy, kBottleneckMss, (window != 0U) ? &seed : nullptr, slot, delivery, 1U, coupling);
    }
    ~Built()
    {
        if (ctl != nullptr)
        {
            ctl->~CongestionController();
        }
    }
    Built(const Built&) = delete;
    Built& operator=(const Built&) = delete;

    CongestionSlot slot;
    DeliveryRateSampler delivery;
    CongestionController* ctl = nullptr;
};

// RFC 6356 equations 1 and 2 in floating point: the divisor of acked * MSS for member `self`.
double reference(const double* windows, const double* rtts, usize count, usize self)
{
    double total = 0.0;
    double peak = 0.0;
    double sum = 0.0;
    for (usize i = 0U; i < count; ++i)
    {
        total += windows[i];
        peak = std::fmax(peak, windows[i] / (rtts[i] * rtts[i]));
        sum += windows[i] / rtts[i];
    }
    const double alpha = total * peak / (sum * sum);
    return std::fmax(total / alpha, windows[self]);
}

// Bytes each flow had acknowledged from `start_ns` to `end_ns`, the flows run together.
struct Shares
{
    u64 bytes[4] = {};
};

Shares measure(std::initializer_list<Flow*> flows, u64 start_ns, u64 end_ns)
{
    run_flows(flows, start_ns, [] {});
    Shares from;
    usize index = 0U;
    for (const Flow* flow : flows)
    {
        from.bytes[index++] = flow->acked_bytes;
    }
    run_flows(flows, end_ns, [] {});
    Shares out;
    index = 0U;
    for (const Flow* flow : flows)
    {
        out.bytes[index] = flow->acked_bytes - from.bytes[index];
        ++index;
    }
    return out;
}

double ratio(u64 a, u64 b)
{
    return static_cast<double>(a) / static_cast<double>(b);
}
} // namespace

TEST_CASE("Coupled congestion: NewReno of version 7 alone couples, and controllers join and leave their coupling",
          "[net][net-path][congestion][coupled]")
{
    CHECK(congestion_policy_valid(reno()));
    for (const CongestionAlgorithm algorithm :
         {CongestionAlgorithm::Cubic, CongestionAlgorithm::Bbr, CongestionAlgorithm::Prague,
          CongestionAlgorithm::Scream, CongestionAlgorithm::Gcc})
    {
        CongestionPolicy policy = reno();
        policy.algorithm = algorithm;
        CHECK_FALSE(congestion_policy_valid(policy));
        policy.coupled = false;
        CHECK(congestion_policy_valid(policy));
        policy.coupled = true;
        policy.version = 6U; // before coupling: the field is not read
        CHECK(congestion_policy_valid(policy));
    }

    CongestionCoupling coupling;
    CHECK(coupling.members() == 0U);
    {
        Built alone(reno(), nullptr);
        REQUIRE(alone.ctl != nullptr);
        CongestionPolicy old = reno();
        old.version = 6U;
        Built before(old, &coupling);
        REQUIRE(before.ctl != nullptr);
        Built uncoupled(reno(false), &coupling);
        REQUIRE(uncoupled.ctl != nullptr);
        CHECK(coupling.members() == 0U);
    }
    Built first(reno(), &coupling);
    {
        Built second(reno(), &coupling);
        Built third(reno(), &coupling);
        Built fourth(reno(), &coupling);
        REQUIRE(fourth.ctl != nullptr);
        CHECK(coupling.members() == 4U);
        Built fifth(reno(), &coupling); // every member taken
        CHECK(fifth.ctl == nullptr);
        CHECK(coupling.members() == 4U);
    }
    CHECK(coupling.members() == 1U);
    CHECK(coupling.join(*first.ctl) == 1U); // the lowest free index
    coupling.leave(1U);
    coupling.leave(kCongestionNoMember);
    CHECK(coupling.members() == 1U);

    // A live swap leaves and joins again; a swap to CUBIC leaves for good.
    Flow flow(reno(), kRate, 20U * kMs, 50000U, 7U, &coupling);
    CHECK(coupling.members() == 2U);
    flow.swap(reno());
    CHECK(coupling.members() == 2U);
    CongestionPolicy cubic;
    flow.swap(cubic);
    CHECK(coupling.members() == 1U);
}

TEST_CASE("Coupled congestion: one member alone grows as NewReno to the byte", "[net][net-path][congestion][coupled]")
{
    CongestionCoupling coupling;
    Flow coupled(reno(), kRate, 20U * kMs, 40000U, 7U, &coupling);
    Flow alone(reno(false), kRate, 20U * kMs, 40000U, 7U);
    coupled.set_loss(3U);
    alone.set_loss(3U);
    usize avoided = 0U;
    for (u64 t = 1U; t <= 300U; ++t)
    {
        coupled.run(t * 100U * kMs);
        alone.run(t * 100U * kMs);
        const CongestionState a = coupled.ctl->state();
        const CongestionState b = alone.ctl->state();
        REQUIRE(a.window == b.window);
        REQUIRE(a.slow_start_threshold == b.slow_start_threshold);
        REQUIRE(a.reductions == b.reductions);
        REQUIRE(coupled.acked_bytes == alone.acked_bytes);
        avoided += (a.phase == CongestionPhase::Avoidance) ? 1U : 0U;
    }
    CHECK(avoided > 100U); // most of it in congestion avoidance, where coupling acts
    CHECK(coupled.ctl->state().reductions > 10U);
}

TEST_CASE("Coupled congestion: the divisor is RFC 6356's cwnd_total / alpha in exact integers",
          "[net][net-path][congestion][coupled]")
{
    u64 seed = 0x9E3779B97F4A7C15U;
    const auto next = [&seed](u64 bound)
    {
        seed = seed * 6364136223846793005U + 1442695040888963407U;
        return (seed >> 17U) % bound;
    };
    SECTION("windows and RTTs drawn at random, with each member asking")
    {
        for (usize trial = 0U; trial < 2000U; ++trial)
        {
            const usize count = 1U + next(4U);
            CongestionCoupling coupling;
            Built members[4] = {{reno(), &coupling, 2400U + next(1U << 26U)},
                                {reno(), &coupling, 2400U + next(1U << 26U)},
                                {reno(), &coupling, 2400U + next(1U << 26U)},
                                {reno(), &coupling, 2400U + next(1U << 26U)}};
            double windows[4] = {};
            double rtts[4] = {};
            u64 rtt_ns[4] = {};
            for (usize i = 0U; i < 4U; ++i)
            {
                rtt_ns[i] = kMs + next(999U * kMs);
                windows[i] = static_cast<double>(members[i].ctl->window());
                rtts[i] = static_cast<double>(rtt_ns[i]);
                // Members past `count` have nothing in flight and count nothing.
                coupling.report(i, (i < count) ? members[i].ctl->window() : 0U, rtt_ns[i]);
            }
            for (usize self = 0U; self < count; ++self)
            {
                const double expected = reference(windows, rtts, count, self);
                const double got =
                    static_cast<double>(coupling.divisor(self, members[self].ctl->window(), rtt_ns[self]));
                CHECK(std::fabs(got - expected) <= expected * 1e-3 + 2.0);
                CHECK(got >= windows[self]);
            }
        }
    }
    SECTION("a member counts what it has in flight under its window, and only once it has an RTT sample")
    {
        CongestionCoupling coupling;
        Built a(reno(), &coupling, 100000U);
        Built b(reno(), &coupling, 200000U);
        Built c(reno(), &coupling, 300000U);
        coupling.report(0U, 100000U, 20U * kMs);
        coupling.report(1U, 50000U, 40U * kMs); // application limited: counts 50000
        coupling.report(2U, 900000U, 0U);       // no RTT sample yet: counts nothing
        const double windows[2] = {100000.0, 50000.0};
        const double rtts[2] = {20.0 * kMs, 40.0 * kMs};
        const double expected = reference(windows, rtts, 2U, 0U);
        CHECK(std::fabs(static_cast<double>(coupling.divisor(0U, 100000U, 20U * kMs)) - expected) <= 2.0);
        // The asking member counts its own window and RTT whatever it reported.
        coupling.report(0U, 0U, 0U);
        CHECK(std::fabs(static_cast<double>(coupling.divisor(0U, 100000U, 20U * kMs)) - expected) <= 2.0);
        // Alone, it is its own window: NewReno.
        coupling.report(1U, 0U, 40U * kMs);
        CHECK(coupling.divisor(0U, 100000U, 20U * kMs) == 100000U);
        CHECK(coupling.divisor(0U, 100000U, 0U) == 100000U);
    }
    SECTION("an RTT far below the largest counts at its bound, and windows at their most stay exact")
    {
        CongestionCoupling coupling;
        Built a(reno(), &coupling, kCongestionMaxWindow);
        Built b(reno(), &coupling, kCongestionMaxWindow);
        Built c(reno(), &coupling, kCongestionMaxWindow);
        Built d(reno(), &coupling, kCongestionMaxWindow);
        REQUIRE(a.ctl->window() == kCongestionMaxWindow);
        const u64 largest = 10U * kBottleneckSecond;
        const u64 rtt_ns[4] = {largest, largest / 3U, 1000U, 1U}; // the last two below largest >> 14
        const double bound = static_cast<double>(largest >> kCouplingRttRatioBits);
        double windows[4] = {};
        double rtts[4] = {};
        for (usize i = 0U; i < 4U; ++i)
        {
            coupling.report(i, kCongestionMaxWindow, rtt_ns[i]);
            windows[i] = static_cast<double>(kCongestionMaxWindow);
            rtts[i] = std::fmax(static_cast<double>(rtt_ns[i]), bound);
        }
        for (usize self = 0U; self < 4U; ++self)
        {
            const double expected = reference(windows, rtts, 4U, self);
            const double got = static_cast<double>(coupling.divisor(self, kCongestionMaxWindow, rtt_ns[self]));
            CHECK(std::fabs(got - expected) <= expected * 1e-9 + 2.0);
        }
        // Equal paths: alpha is 1 / n, so each grows by 1 / n^2 of a lone path's increase.
        CongestionCoupling equal;
        Built e(reno(), &equal, 1U << 20U);
        Built f(reno(), &equal, 1U << 20U);
        equal.report(0U, 1U << 20U, 30U * kMs);
        equal.report(1U, 1U << 20U, 30U * kMs);
        CHECK(equal.divisor(0U, 1U << 20U, 30U * kMs) == 4U << 20U);
    }
}

TEST_CASE("Coupled congestion: two paths through one bottleneck beside a NewReno flow take about one flow's share",
          "[net][net-path][congestion][coupled]")
{
    const auto shares = [](bool coupled)
    {
        Link link{kRate, 50000U};
        CongestionCoupling coupling;
        Flow a(reno(coupled), link, 20U * kMs, 7U, &coupling);
        Flow b(reno(coupled), link, 20U * kMs, 8U, &coupling);
        Flow c(reno(false), link, 20U * kMs, 9U);
        const Shares out = measure({&a, &b, &c}, 20U * kBottleneckSecond, 200U * kBottleneckSecond);
        const double link_bytes = 180.0 * static_cast<double>(kRate);
        CHECK(static_cast<double>(out.bytes[0] + out.bytes[1] + out.bytes[2]) > 0.95 * link_bytes);
        return ratio(out.bytes[0] + out.bytes[1], out.bytes[2]);
    };
    const double coupled = shares(true);
    const double uncoupled = shares(false);
    INFO("multipath against single-path: coupled " << coupled << ", uncoupled " << uncoupled);
    CHECK(coupled > 0.6);
    CHECK(coupled < 1.4);
    CHECK(uncoupled > 1.6); // teeth: without coupling the two paths take two flows' share
}

TEST_CASE("Coupled congestion: disjoint links are both used, and a lossier or longer path carries its part",
          "[net][net-path][congestion][coupled]")
{
    constexpr u64 warm_ns = 20U * kBottleneckSecond;
    constexpr u64 end_ns = 200U * kBottleneckSecond;
    const double link_bytes = 180.0 * static_cast<double>(kRate);
    SECTION("two clean links are each kept full")
    {
        CongestionCoupling coupling;
        Flow a(reno(), kRate, 20U * kMs, 50000U, 7U, &coupling);
        Flow b(reno(), kRate, 20U * kMs, 50000U, 8U, &coupling);
        const Shares out = measure({&a, &b}, warm_ns, end_ns);
        INFO("utilisation " << ratio(out.bytes[0], kRate * 180U) << " and " << ratio(out.bytes[1], kRate * 180U));
        CHECK(static_cast<double>(out.bytes[0]) > 0.9 * link_bytes);
        CHECK(static_cast<double>(out.bytes[1]) > 0.9 * link_bytes);
    }
    SECTION("a lossier path carries less than uncoupled, and the total is at least the better path's alone")
    {
        const auto run = [&](bool coupled)
        {
            CongestionCoupling coupling;
            Flow clean(reno(coupled), kRate, 20U * kMs, 50000U, 7U, &coupling);
            Flow lossy(reno(coupled), kRate, 20U * kMs, 50000U, 8U, &coupling);
            clean.set_loss(1U);
            lossy.set_loss(10U);
            return measure({&clean, &lossy}, warm_ns, end_ns);
        };
        Flow best(reno(false), kRate, 20U * kMs, 50000U, 7U);
        best.set_loss(1U);
        const u64 single = measure({&best}, warm_ns, end_ns).bytes[0];
        const Shares coupled = run(true);
        const Shares uncoupled = run(false);
        const double coupled_share = ratio(coupled.bytes[1], coupled.bytes[0] + coupled.bytes[1]);
        const double uncoupled_share = ratio(uncoupled.bytes[1], uncoupled.bytes[0] + uncoupled.bytes[1]);
        INFO("lossy path's share: coupled " << coupled_share << ", uncoupled " << uncoupled_share << "; total "
                                            << ratio(coupled.bytes[0] + coupled.bytes[1], single) << " of one path");
        CHECK(coupled_share < uncoupled_share);
        CHECK(coupled.bytes[0] + coupled.bytes[1] >= single);
    }
    SECTION("with RTTs of 20 and 160 ms the total is at least the shorter path's alone")
    {
        CongestionCoupling coupling;
        Flow short_path(reno(), kRate, 10U * kMs, 25000U, 7U, &coupling);
        Flow long_path(reno(), kRate, 80U * kMs, 200000U, 8U, &coupling);
        Flow best(reno(false), kRate, 10U * kMs, 25000U, 7U);
        const u64 single = measure({&best}, warm_ns, end_ns).bytes[0];
        const Shares out = measure({&short_path, &long_path}, warm_ns, end_ns);
        INFO("shorter " << ratio(out.bytes[0], kRate * 180U) << ", longer " << ratio(out.bytes[1], kRate * 180U));
        CHECK(out.bytes[0] + out.bytes[1] >= single);
        CHECK(static_cast<double>(out.bytes[1]) > 0.5 * link_bytes);
    }
}
