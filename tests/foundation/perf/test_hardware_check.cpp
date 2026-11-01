// HW-RESILIENCE.3 -- the hardware check (crd/perf/hardware_check.hpp): every arm runs on every CPU the process may run
// on and a sound machine shows no event; each arm finds a value the fault seam corrupts on one CPU, and the report
// names that CPU, the arm and the value; the compute arm's levels agree bit for bit; hardware.check serves it under the
// Execute grant only, within the host's bounds. Contract: docs/design/runtime-diagnostics.md (HW-RESILIENCE.3).

#include "kernel_levels.hpp"

#include <crd/containers/string.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/hardware_check.hpp>
#include <crd/perf/hardware_check_kernels.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string_view>

namespace
{
namespace cp = crd::perf;

constexpr crd::u64 kArmNs = 5'000'000U; // 5 ms an arm

[[nodiscard]] cp::HardwareCheckConfig short_run() noexcept
{
    cp::HardwareCheckConfig config;
    config.nanoseconds_per_arm = kArmNs;
    config.memory_mib = 1U;
    return config;
}

struct Corrupt
{
    crd::u32 cpu = 0U;
    std::atomic<crd::u32> consulted{0U};
};

bool corrupt_one_cpu(void* user, crd::u32 cpu, const char* /*arm*/) noexcept
{
    auto* c = static_cast<Corrupt*>(user);
    c->consulted.fetch_add(1U, std::memory_order_relaxed);
    return cpu == c->cpu;
}

[[nodiscard]] bool has(const crd::containers::String& text, const char* needle)
{
    return std::string_view{text.c_str()}.find(needle) != std::string_view::npos;
}

// The index of the first CPU a check could pin (logical_cpu_count() when none could be).
[[nodiscard]] crd::u32 first_pinned_cpu()
{
    const cp::HardwareCheckReport probe = cp::run_hardware_check(short_run(), crd::memory::default_allocator());
    for (crd::u32 c = 0U; c < probe.cpus.size(); ++c)
    {
        if (probe.cpus[c].pinned)
        {
            return c;
        }
    }
    return crd::logical_cpu_count();
}

} // namespace

TEST_CASE("hardware check: every arm runs on every pinned CPU and a sound machine shows no event", "[perf][kernels]")
{
    const cp::HardwareCheckReport report = cp::run_hardware_check(short_run(), crd::memory::default_allocator());
    INFO(report.reason.c_str());
    REQUIRE(report.ran);
    CHECK_FALSE(report.cancelled);
    REQUIRE(report.cpus.size() == crd::logical_cpu_count());
    CHECK(report.cpus_pinned >= 1U); // the CPU this test runs on is in the process's set

    // reload, compute at every level the selection includes, memory, shared.
    REQUIRE(report.arm_count >= 4U);
    CHECK(std::string_view{cp::hardware_arm_name(report.arms[0])} == "reload");
    CHECK(std::string_view{cp::hardware_arm_name(report.arms[1])} == "compute-portable");
    CHECK(std::string_view{cp::hardware_arm_name(report.arms[report.arm_count - 2U])} == "memory");
    CHECK(std::string_view{cp::hardware_arm_name(report.arms[report.arm_count - 1U])} == "shared");
    const crd::CpuLevel selected = crd::cpu_selection().selected;
    for (crd::u32 a = 1U; a + 2U < report.arm_count; ++a)
    {
        CHECK(report.arms[a].kind == cp::HardwareArmKind::Compute);
        CHECK(crd::cpu_level_includes(selected, report.arms[a].level));
    }

    crd::u32 pinned = 0U;
    for (const cp::HardwareCpuResult& c : report.cpus)
    {
        if (!c.pinned)
        {
            for (crd::u32 a = 0U; a < report.arm_count; ++a)
            {
                CHECK(c.arms[a].blocks == 0U); // a refused CPU runs nothing
            }
            continue;
        }
        ++pinned;
        for (crd::u32 a = 0U; a < report.arm_count; ++a)
        {
            const cp::HardwareArmResult& r = c.arms[a];
            INFO("cpu " << c.cpu.group << ":" << c.cpu.number << " arm " << cp::hardware_arm_name(report.arms[a]));
            CHECK(r.blocks >= 1U);
            CHECK(r.nanoseconds >= kArmNs);
            CHECK(r.events == 0U);
            CHECK_FALSE(r.injected);
        }
    }
    CHECK(pinned == report.cpus_pinned);
    CHECK(report.events == 0U);
}

TEST_CASE("hardware check: each arm finds the value the fault seam corrupts and names its CPU", "[perf]")
{
    Corrupt corrupt;
    if (!crd::fault_seams_compiled())
    {
        CHECK_FALSE(crd::install_hardware_check_fault_hook(&corrupt_one_cpu, &corrupt));
        return;
    }
    corrupt.cpu = first_pinned_cpu();
    REQUIRE(corrupt.cpu < crd::logical_cpu_count());

    REQUIRE(crd::install_hardware_check_fault_hook(&corrupt_one_cpu, &corrupt));
    CHECK_FALSE(crd::install_hardware_check_fault_hook(&corrupt_one_cpu, &corrupt)); // one hook
    const cp::HardwareCheckReport report = cp::run_hardware_check(short_run(), crd::memory::default_allocator());
    crd::uninstall_hardware_check_fault_hook();
    REQUIRE(report.ran);
    CHECK(corrupt.consulted.load() == report.cpus_pinned * report.arm_count); // once per CPU and arm
    CHECK(report.events == report.arm_count);
    for (crd::u32 c = 0U; c < report.cpus.size(); ++c)
    {
        for (crd::u32 a = 0U; a < report.arm_count; ++a)
        {
            const cp::HardwareArmResult& r = report.cpus[c].arms[a];
            INFO("cpu " << c << " arm " << cp::hardware_arm_name(report.arms[a]));
            if (c != corrupt.cpu)
            {
                CHECK(r.events == 0U);
                CHECK_FALSE(r.injected);
                continue;
            }
            // The corrupted value is the arm's first: its place 0, read with bit 0 flipped, found once.
            CHECK(r.injected);
            CHECK(r.events == 1U);
            CHECK(r.first_where == 0U);
            CHECK((r.first_got ^ r.first_want) == 1U);
            if (report.arms[a].kind != cp::HardwareArmKind::Shared) // a shared read may wait for a whole write
            {
                CHECK(r.first_block == 0U);
            }
        }
    }
}

TEST_CASE("hardware check: every compute level the CPU runs gives the portable bits", "[perf][kernels]")
{
    cp::HardwareComputeState seed{};
    for (crd::u32 l = 0U; l < cp::kHardwareComputeLanes; ++l)
    {
        seed.x[l] = 0xD1B54A32D192ED03ULL * (l + 3U);
        seed.y[l] = 0.03125 + static_cast<double>(l) / 19.0;
        seed.z[l] = 2.0 - static_cast<double>(l) / 16.0;
        seed.c[l] = ~static_cast<crd::u64>(l);
    }
    cp::HardwareComputeState portable = seed;
    cp::kHardwareCheckKernelsPortable.compute(portable, 1000U);
    crd::u32 ran = 0U;
    for (const crd::KernelLevel<cp::HardwareCheckKernels>& l : cp::hardware_check_levels())
    {
        if (!crd::test::kernel_level_runs_here(l))
        {
            continue;
        }
        ++ran;
        INFO(crd::cpu_level_name(l.level));
        cp::HardwareComputeState got = seed;
        l.table->compute(got, 1000U);
        CHECK(std::memcmp(&got, &portable, sizeof(got)) == 0);
        cp::HardwareComputeState none = seed;
        l.table->compute(none, 0U); // no step leaves the state as it was
        CHECK(std::memcmp(&none, &seed, sizeof(none)) == 0);
    }
    CHECK(ran >= 1U);
}

TEST_CASE("hardware check: a configuration out of bounds and a cancelled run are refused or stopped", "[perf]")
{
    cp::HardwareCheckConfig config = short_run();
    config.nanoseconds_per_arm = 0U;
    CHECK_FALSE(cp::run_hardware_check(config, crd::memory::default_allocator()).ran);
    config = short_run();
    config.nanoseconds_per_arm = cp::kHardwareCheckMaxNanosecondsPerArm + 1U;
    CHECK_FALSE(cp::run_hardware_check(config, crd::memory::default_allocator()).ran);
    config = short_run();
    config.memory_mib = 0U;
    CHECK_FALSE(cp::run_hardware_check(config, crd::memory::default_allocator()).ran);
    config.memory_mib = cp::kHardwareCheckMaxMemoryMib + 1U;
    const cp::HardwareCheckReport refused = cp::run_hardware_check(config, crd::memory::default_allocator());
    CHECK_FALSE(refused.ran);
    CHECK_FALSE(refused.reason.empty());

    // A run asked for an hour an arm, cancelled before it starts: every arm runs one block and stops.
    const std::atomic<bool> cancel{true};
    config = short_run();
    config.nanoseconds_per_arm = cp::kHardwareCheckMaxNanosecondsPerArm;
    config.cancel = &cancel;
    const cp::HardwareCheckReport stopped = cp::run_hardware_check(config, crd::memory::default_allocator());
    REQUIRE(stopped.ran);
    CHECK(stopped.cancelled);
    for (const cp::HardwareCpuResult& c : stopped.cpus)
    {
        for (crd::u32 a = 0U; a < stopped.arm_count; ++a)
        {
            CHECK(c.arms[a].blocks == (c.pinned ? 1U : 0U));
        }
    }
}

TEST_CASE("hardware check: hardware.check runs under Execute only, within the host's bounds", "[perf][diag]")
{
    const cp::DiagRequest base = []
    {
        cp::DiagRequest r;
        r.command = "hardware.check";
        r.page_items = cp::kDiagMaxPageItems;
        r.page_bytes = cp::kDiagMaxPageBytes;
        return r;
    }();
    const cp::DiagArg args[] = {{"seconds", "0.005"}, {"memory_mib", "1"}}; // NOLINT(modernize-avoid-c-arrays)

    // Read alone, or Read and Record, never starts the load.
    cp::DiagCommandService readonly(cp::authority_bit(cp::DiagAuthority::Read) |
                                        cp::authority_bit(cp::DiagAuthority::Record),
                                    cp::DiagServiceConfig{});
    cp::DiagRequest r = base;
    r.args = {args, 2U};
    CHECK(readonly.execute(r).status == cp::DiagStatus::Unauthorized);

    const cp::DiagAuthoritySet execute =
        cp::authority_bit(cp::DiagAuthority::Read) | cp::authority_bit(cp::DiagAuthority::Execute);
    cp::DiagServiceConfig bounded;
    bounded.max_hardware_check_ns = 10'000'000U;
    bounded.max_hardware_check_mib = 2U;
    cp::DiagCommandService service(execute, bounded);

    // Arguments the check refuses before any work: unknown, malformed, zero and above the host's bounds.
    const cp::DiagArg bad[][1] = {
        // NOLINT(modernize-avoid-c-arrays)
        {{"cpus", "1"}},
        {{"seconds", ""}},
        {{"seconds", "1e3"}},
        {{"seconds", "0"}},
        {{"seconds", "0.0000000001"}},
        {{"seconds", "0.011"}},
        {{"memory_mib", "0"}},
        {{"memory_mib", "3"}},
        {{"memory_mib", "-1"}},
    };
    for (const auto& one : bad)
    {
        r = base;
        r.args = {one, 1U};
        const cp::DiagResult refused = service.execute(r);
        INFO(refused.json.c_str());
        CHECK(refused.status == cp::DiagStatus::BadArgument);
    }

    // Within them: the verdict and one item per pinned CPU and arm.
    r = base;
    r.args = {args, 2U};
    const cp::DiagResult ran = service.execute(r);
    INFO(ran.json.c_str());
    REQUIRE(ran.status == cp::DiagStatus::Ok);
    CHECK(has(ran.json, "\"verdict\":\"no-event-seen\""));
    CHECK(has(ran.json, "\"nanoseconds_per_arm\":5000000"));
    CHECK(has(ran.json, "\"kind\":\"cpu-arm\""));
    CHECK(has(ran.json, "\"arm\":\"compute-portable\""));

    // The host's bound lowers the default (2 s) without an argument.
    r = base;
    const cp::DiagResult defaulted = service.execute(r);
    REQUIRE(defaulted.status == cp::DiagStatus::Ok);
    CHECK(has(defaulted.json, "\"nanoseconds_per_arm\":10000000"));
    CHECK(has(defaulted.json, "\"memory_mib\":2"));

    // A corrupted value on one CPU: events-seen, and that CPU's item names the arm and the values.
    if (crd::fault_seams_compiled())
    {
        Corrupt corrupt;
        corrupt.cpu = first_pinned_cpu();
        REQUIRE(crd::install_hardware_check_fault_hook(&corrupt_one_cpu, &corrupt));
        r = base;
        r.args = {args, 2U};
        const cp::DiagResult events = service.execute(r);
        crd::uninstall_hardware_check_fault_hook();
        INFO(events.json.c_str());
        REQUIRE(events.status == cp::DiagStatus::Ok);
        CHECK(has(events.json, "\"verdict\":\"events-seen\""));
        CHECK(has(events.json, "\"arm\":\"memory\",\"blocks\""));
        CHECK(has(events.json, "\"events\":1,\"first_block\":0,\"first_where\":0"));
        CHECK(has(events.json, "\"injected\":true"));
    }
}
