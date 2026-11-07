// crd-hesap-link-netem-tests: hesap-net's link models against the running kernel's netem, statistically and at arm's
// length. No netem source is read, cut or compiled: `tc` sets a law on the loopback of a private network namespace
// (CTest runs this under `unshare -Urn`), numbered datagrams cross it, and what arrives is compared with what the model
// answers for the same law. The kernel draws its own randomness, so every bound here is on a sampling distribution, set
// where a false alarm is about one in a million.
//
// netem is GPL-2.0, which is why this is a comparison and nothing more: see "Copyleft" in docs/design/pinned-inputs.md.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/net/link_models.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace hn = crd::hesap::net;
using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;

namespace
{

// Far enough past the sends of one run that the lost and the late are told apart.
constexpr int kDrainMs = 300;
constexpr u32 kRunBins = 9U; // Runs of 1 to 8 and the rest.

void shell(const char* command)
{
    INFO(command);
    REQUIRE(std::system(command) == 0);
}

void set_netem(const char* law)
{
    static const int ready = []
    {
        return std::system("ip link set lo up");
    }();
    REQUIRE(ready == 0);
    char command[256];
    std::snprintf(command, sizeof(command), "tc qdisc replace dev lo root netem limit 1000000 %s", law);
    shell(command);
}

class Loopback
{
public:
    Loopback()
    {
        m_receive = socket(AF_INET, SOCK_DGRAM, 0);
        m_send = socket(AF_INET, SOCK_DGRAM, 0);
        REQUIRE((m_receive >= 0 && m_send >= 0));
        const int on = 1;
        const int large = 64 * 1024 * 1024;
        REQUIRE(setsockopt(m_receive, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) == 0);
        REQUIRE(setsockopt(m_receive, SOL_SOCKET, SO_RCVBUF, &large, sizeof(large)) == 0);
        m_address.sin_family = AF_INET;
        m_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(m_receive, reinterpret_cast<const sockaddr*>(&m_address), sizeof(m_address)) == 0);
        socklen_t length = sizeof(m_address);
        REQUIRE(getsockname(m_receive, reinterpret_cast<sockaddr*>(&m_address), &length) == 0);
    }

    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;

    ~Loopback()
    {
        close(m_receive);
        close(m_send);
    }

    void send(u32 number)
    {
        const ssize_t sent = sendto(m_send, &number, sizeof(number), 0, reinterpret_cast<const sockaddr*>(&m_address),
                                    sizeof(m_address));
        REQUIRE(sent == static_cast<ssize_t>(sizeof(number)));
    }

    // One waiting datagram's number and its arrival stamp, or false after `wait_ms` with none.
    [[nodiscard]] bool receive(u32& number, i64& arrived_ns, int wait_ms)
    {
        if (wait_ms != 0 && wait_ms != m_wait_ms)
        {
            const timeval timeout{wait_ms / 1000, (wait_ms % 1000) * 1000};
            REQUIRE(setsockopt(m_receive, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
            m_wait_ms = wait_ms;
        }
        alignas(cmsghdr) char control[128];
        iovec part{&number, sizeof(number)};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        if (recvmsg(m_receive, &message, wait_ms == 0 ? MSG_DONTWAIT : 0) < 0)
        {
            return false;
        }
        for (cmsghdr* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item))
        {
            if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_TIMESTAMPNS)
            {
                timespec stamp{};
                std::memcpy(&stamp, CMSG_DATA(item), sizeof(stamp));
                arrived_ns = (i64{stamp.tv_sec} * 1'000'000'000) + stamp.tv_nsec;
            }
        }
        return true;
    }

private:
    int m_receive = -1;
    int m_send = -1;
    int m_wait_ms = 0;
    sockaddr_in m_address{};
};

[[nodiscard]] i64 now_ns()
{
    timespec stamp{};
    REQUIRE(clock_gettime(CLOCK_REALTIME, &stamp) == 0);
    return (i64{stamp.tv_sec} * 1'000'000'000) + stamp.tv_nsec;
}

void wait_until(i64 when_ns)
{
    while (now_ns() < when_ns)
    {
    }
}

// Which of `packets` datagrams the current netem setting drops.
[[nodiscard]] crd::containers::Array<bool> kernel_fates(u32 packets)
{
    crd::containers::Array<bool> lost(packets);
    lost.resize(packets);
    std::fill(lost.begin(), lost.end(), true);
    Loopback link;
    const auto take = [&](int wait_ms)
    {
        u32 number = 0U;
        i64 arrived = 0;
        while (link.receive(number, arrived, wait_ms))
        {
            REQUIRE(number < packets);
            lost[number] = false;
        }
    };
    for (u32 i = 0U; i < packets; ++i)
    {
        link.send(i);
        take(0);
    }
    take(kDrainMs);
    return lost;
}

template <typename Setting> [[nodiscard]] crd::containers::Array<bool> model_fates(u32 packets, const Setting& setting)
{
    crd::containers::Array<bool> lost(packets);
    lost.resize(packets);
    hn::LossModel model;
    crd::hesap::stats::PhiloxRng rng(0xC0FFEEU, 1U);
    setting(model, rng);
    for (u32 i = 0U; i < packets; ++i)
    {
        lost[i] = model.next(rng);
    }
    return lost;
}

struct LossStats
{
    f64 rate = 0.0;
    u64 runs[kRunBins] = {};
    u64 gaps[kRunBins] = {};
    u64 run_count = 0U;
    u64 gap_count = 0U;
};

[[nodiscard]] LossStats loss_stats(const crd::containers::Array<bool>& lost)
{
    LossStats stats;
    u64 lost_count = 0U;
    usize start = 0U;
    while (start < lost.size())
    {
        usize end = start;
        while (end < lost.size() && lost[end] == lost[start])
        {
            ++end;
        }
        const u64 length = end - start;
        const u64 bin = std::min<u64>(length, kRunBins) - 1U;
        if (lost[start])
        {
            lost_count += length;
            ++stats.runs[bin];
            ++stats.run_count;
        }
        else
        {
            ++stats.gaps[bin];
            ++stats.gap_count;
        }
        start = end;
    }
    stats.rate = static_cast<f64>(lost_count) / static_cast<f64>(lost.size());
    return stats;
}

// Pearson's statistic of two samples of the same bins. A bin thin on both sides joins the next.
[[nodiscard]] f64 two_sample_chi_square(const u64* first, const u64* second, u32 bins, u64 first_total,
                                        u64 second_total, u32& degrees)
{
    const f64 k1 = std::sqrt(static_cast<f64>(second_total) / static_cast<f64>(first_total));
    const f64 k2 = 1.0 / k1;
    f64 sum = 0.0;
    u64 held_first = 0U;
    u64 held_second = 0U;
    degrees = 0U;
    for (u32 bin = 0U; bin < bins; ++bin)
    {
        held_first += first[bin];
        held_second += second[bin];
        if (held_first + held_second < 20U && bin + 1U != bins)
        {
            continue;
        }
        const f64 difference = (k1 * static_cast<f64>(held_first)) - (k2 * static_cast<f64>(held_second));
        sum += difference * difference / static_cast<f64>(held_first + held_second);
        ++degrees;
        held_first = 0U;
        held_second = 0U;
    }
    degrees = degrees == 0U ? 0U : degrees - 1U;
    return sum;
}

// The chi-square upper tail at about one in a million, by Wilson-Hilferty.
[[nodiscard]] f64 chi_square_bound(u32 degrees)
{
    const f64 v = static_cast<f64>(degrees == 0U ? 1U : degrees);
    const f64 term = 1.0 - (2.0 / (9.0 * v)) + (4.75 * std::sqrt(2.0 / (9.0 * v)));
    return v * term * term * term;
}

void compare_runs(const LossStats& kernel, const LossStats& model)
{
    u32 degrees = 0U;
    const f64 runs =
        two_sample_chi_square(kernel.runs, model.runs, kRunBins, kernel.run_count, model.run_count, degrees);
    INFO("loss runs: chi-square " << runs << " on " << degrees << " degrees");
    CHECK(runs < chi_square_bound(degrees));
    const f64 gaps =
        two_sample_chi_square(kernel.gaps, model.gaps, kRunBins, kernel.gap_count, model.gap_count, degrees);
    INFO("received runs: chi-square " << gaps << " on " << degrees << " degrees");
    CHECK(gaps < chi_square_bound(degrees));
}

constexpr u32 kPackets = 1'000'000U;

[[nodiscard]] f64 percent(f64 fraction)
{
    return fraction * 100.0;
}

// The sorted one-way delays of `packets` datagrams, paced so the kernel's timers are the only queue.
[[nodiscard]] crd::containers::Array<f64> kernel_delays(u32 packets)
{
    crd::containers::Array<i64> sent(packets);
    sent.resize(packets);
    crd::containers::Array<f64> delays(packets);
    Loopback link;
    const i64 pace_ns = 200'000;
    const auto take = [&](int wait_ms)
    {
        u32 index = 0U;
        i64 arrived = 0;
        while (link.receive(index, arrived, wait_ms))
        {
            REQUIRE(index < packets);
            delays.push_back(static_cast<f64>(arrived - sent[index]));
        }
    };
    i64 next = now_ns();
    for (u32 i = 0U; i < packets; ++i)
    {
        wait_until(next);
        sent[i] = now_ns();
        link.send(i);
        take(0);
        next += pace_ns;
    }
    take(kDrainMs);
    std::sort(delays.begin(), delays.end());
    return delays;
}

[[nodiscard]] crd::containers::Array<f64> model_delays(const hn::DelayOptions& options, u32 packets)
{
    crd::hesap::stats::PhiloxRng rng(0xBEEFU, 2U);
    hn::DelayProcess process(rng);
    REQUIRE(process.init(options));
    crd::containers::Array<f64> delays(packets);
    delays.resize(packets);
    for (u32 i = 0U; i < packets; ++i)
    {
        delays[i] = static_cast<f64>(process.next());
    }
    std::sort(delays.begin(), delays.end());
    return delays;
}

[[nodiscard]] f64 quantile(const crd::containers::Array<f64>& sorted, f64 rank)
{
    return sorted[static_cast<usize>(rank * static_cast<f64>(sorted.size() - 1U))];
}

constexpr f64 kKernelOverheadNs = 300'000.0; // What a loaded machine's timers and softirqs may add beside the law.

void compare_delays(const crd::containers::Array<f64>& kernel, const crd::containers::Array<f64>& model, f64 spread_ns)
{
    REQUIRE(kernel.size() > (model.size() * 99U) / 100U);
    for (const f64 rank : {0.1, 0.25, 0.5, 0.75, 0.9})
    {
        INFO("rank " << rank << ": kernel " << quantile(kernel, rank) << " ns, model " << quantile(model, rank)
                     << " ns");
        CHECK(std::abs(quantile(kernel, rank) - quantile(model, rank)) < (spread_ns * 0.08) + kKernelOverheadNs);
    }
}

} // namespace

TEST_CASE("netem comparison: independent loss", "[link-netem]")
{
    set_netem("loss 5%");
    const LossStats kernel = loss_stats(kernel_fates(kPackets));
    const LossStats model = loss_stats(
        model_fates(kPackets, [](hn::LossModel& loss, auto& rng) { loss.set_random(hn::link_chance(0.05), 0U, rng); }));
    INFO("kernel " << kernel.rate << " model " << model.rate);
    CHECK(std::abs(kernel.rate - model.rate) < 0.0015);
    compare_runs(kernel, model);
}

TEST_CASE("netem comparison: a simple Gilbert chain", "[link-netem]")
{
    constexpr f64 to_bad = 0.0103;
    constexpr f64 to_good = 0.25;
    char law[128];
    std::snprintf(law, sizeof(law), "loss gemodel %.6f%% %.6f%% 100%% 0%%", percent(to_bad), percent(to_good));
    set_netem(law);
    const LossStats kernel = loss_stats(kernel_fates(kPackets));
    const LossStats model = loss_stats(
        model_fates(kPackets, [](hn::LossModel& loss, auto&)
                    { loss.set_gilbert_elliott({hn::link_chance(to_bad), hn::link_chance(to_good), 0U, 0U}); }));
    INFO("kernel " << kernel.rate << " model " << model.rate);
    CHECK(std::abs(kernel.rate - model.rate) < 0.004);
    compare_runs(kernel, model);
}

TEST_CASE("netem comparison: the full Gilbert-Elliott chain", "[link-netem]")
{
    constexpr f64 to_bad = 0.02;
    constexpr f64 to_good = 0.2;
    constexpr f64 lost_in_good = 0.01;
    constexpr f64 lost_in_bad = 0.6;
    char law[128];
    std::snprintf(law, sizeof(law), "loss gemodel %.6f%% %.6f%% %.6f%% %.6f%%", percent(to_bad), percent(to_good),
                  percent(lost_in_bad), percent(lost_in_good));
    set_netem(law);
    const LossStats kernel = loss_stats(kernel_fates(kPackets));
    const LossStats model = loss_stats(
        model_fates(kPackets,
                    [](hn::LossModel& loss, auto&)
                    {
                        loss.set_gilbert_elliott({hn::link_chance(to_bad), hn::link_chance(to_good),
                                                  hn::link_chance(1.0 - lost_in_bad), hn::link_chance(lost_in_good)});
                    }));
    INFO("kernel " << kernel.rate << " model " << model.rate);
    CHECK(std::abs(kernel.rate - model.rate) < 0.004);
    compare_runs(kernel, model);
}

TEST_CASE("netem comparison: the four-state chain", "[link-netem]")
{
    constexpr f64 p13 = 0.02;
    constexpr f64 p14 = 0.005;
    constexpr f64 p23 = 0.2;
    constexpr f64 p31 = 0.4;
    constexpr f64 p32 = 0.3;
    char law[128];
    std::snprintf(law, sizeof(law), "loss state %.6f%% %.6f%% %.6f%% %.6f%% %.6f%%", percent(p13), percent(p31),
                  percent(p32), percent(p23), percent(p14));
    set_netem(law);
    const LossStats kernel = loss_stats(kernel_fates(kPackets));
    const LossStats model = loss_stats(
        model_fates(kPackets,
                    [](hn::LossModel& loss, auto&)
                    {
                        REQUIRE(loss.set_four_state({hn::link_chance(p13), hn::link_chance(p31), hn::link_chance(p32),
                                                     hn::link_chance(p23), hn::link_chance(p14)}));
                    }));
    INFO("kernel " << kernel.rate << " model " << model.rate);
    CHECK(std::abs(kernel.rate - model.rate) < 0.004);
    compare_runs(kernel, model);
}

TEST_CASE("netem comparison: uniform and normal delay", "[link-netem]")
{
    constexpr u32 packets = 10'000U;
    constexpr i64 mean_ns = 20'000'000;
    constexpr i64 half_width_ns = 6'000'000;
    SECTION("uniform")
    {
        set_netem("delay 20ms 6ms");
        hn::DelayOptions options;
        options.shape = hn::DelayShape::Uniform;
        options.mean_ns = mean_ns;
        options.deviation_ns = static_cast<i64>(static_cast<f64>(half_width_ns) / std::sqrt(3.0));
        compare_delays(kernel_delays(packets), model_delays(options, packets), static_cast<f64>(half_width_ns));
    }
    SECTION("normal, from the distribution table iproute2 installs")
    {
        set_netem("delay 20ms 4ms distribution normal");
        hn::DelayOptions options;
        options.shape = hn::DelayShape::Normal;
        options.mean_ns = mean_ns;
        options.deviation_ns = 4'000'000;
        compare_delays(kernel_delays(packets), model_delays(options, packets), 4'000'000.0);
    }
}
