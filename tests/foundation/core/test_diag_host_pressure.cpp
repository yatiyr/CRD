// DIAG.10c host memory pressure: the operating system's view of memory as budget readings. Declared contract
// (docs/design/runtime-diagnostics.md, DIAG.10c): physical memory is bounded by the usable RAM with in use = that minus
// what the OS reports available; the commit charge is bounded only where the OS enforces its limit (Windows always,
// Linux in strict overcommit mode); the resident set is this process's own, with its peak, and has no bound. A
// reading the OS could not give is unread with a reason, never zero use.
//
// The oracles share no code with the probe: the usable RAM is asked of a second OS interface (Windows
// GetPerformanceInfo, Linux sysinfo), the overcommit mode is read from /proc by the test itself, and the resident set
// is held to memory the test maps and touches through the OS directly.

#include <crd/core/budget_pressure.hpp>
#include <crd/core/host_pressure.hpp>
#include <crd/core/platform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif CRD_OS_LINUX
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{
constexpr crd::usize kTouchBytes = crd::usize{64} << 20U;

// Memory mapped and written through the OS, released at scope end.
class TouchedPages
{
public:
    TouchedPages() noexcept
    {
#if CRD_OS_WINDOWS
        m_base = VirtualAlloc(nullptr, kTouchBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#elif CRD_OS_LINUX
        void* const p = mmap(nullptr, kTouchBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        m_base        = p == MAP_FAILED ? nullptr : p;
#endif
        if (m_base == nullptr)
        {
            return;
        }
        auto* const bytes = static_cast<volatile unsigned char*>(m_base);
        for (crd::usize i = 0U; i < kTouchBytes; i += 4096U)
        {
            bytes[i] = 1U;
        }
    }
    ~TouchedPages()
    {
        if (m_base == nullptr)
        {
            return;
        }
#if CRD_OS_WINDOWS
        (void)VirtualFree(m_base, 0U, MEM_RELEASE);
#elif CRD_OS_LINUX
        (void)munmap(m_base, kTouchBytes);
#endif
    }
    TouchedPages(const TouchedPages&)            = delete;
    TouchedPages& operator=(const TouchedPages&) = delete;
    TouchedPages(TouchedPages&&)                 = delete;
    TouchedPages& operator=(TouchedPages&&)      = delete;

    [[nodiscard]] bool mapped() const noexcept { return m_base != nullptr; }

private:
    void* m_base = nullptr;
};

#if CRD_OS_LINUX
// The kernel's overcommit mode, read by the test itself: '2' is strict (the commit limit is enforced).
char overcommit_mode()
{
    std::FILE* f = std::fopen("/proc/sys/vm/overcommit_memory", "r");
    REQUIRE(f != nullptr);
    const int c = std::fgetc(f);
    (void)std::fclose(f);
    return static_cast<char>(c);
}
#endif
} // namespace

TEST_CASE("diag10c: host memory is read as budget readings that agree with a second OS interface",
          "[core][pressure][host][diag10c]")
{
    const crd::HostMemoryReading host = crd::read_host_memory();
    INFO(host.reason);
#if CRD_OS_WINDOWS || CRD_OS_LINUX
    REQUIRE(host.physical.read);
    CHECK(host.reason[0] == '\0');
    CHECK(host.physical.bounded);
    CHECK_FALSE(host.physical.high_water_tracked);
    CHECK(host.physical.capacity > 0U);
    CHECK(host.physical.in_use <= host.physical.capacity);
    // The usable RAM, asked of another interface: exactly the same number.
    CHECK(host.physical.capacity == crd::host_physical_total_cross_check());

    REQUIRE(host.commit.read);
#if CRD_OS_WINDOWS
    CHECK(host.commit_enforced);
    CHECK(host.commit.bounded);
    CHECK(host.commit.capacity > 0U);
    CHECK(host.commit.in_use <= host.commit.capacity);
    REQUIRE(host.commit.high_water_tracked);
    CHECK(host.commit.high_water >= host.commit.in_use);
#else
    const bool strict = overcommit_mode() == '2';
    CHECK(host.commit_enforced == strict);
    CHECK(host.commit.bounded == strict);
    CHECK_FALSE(host.commit.high_water_tracked);
    if (!strict)
    {
        // Heuristic overcommit: the charge may pass the limit, so it is reported and never judged.
        CHECK(crd::pressure_level(host.commit, host.commit.in_use) == crd::PressureLevel::Unbounded);
    }
#endif

    REQUIRE(host.resident.read);
    CHECK_FALSE(host.resident.bounded);
    CHECK(host.resident.high_water_tracked);
    CHECK(host.resident.in_use > 0U);
    CHECK(host.resident.high_water >= host.resident.in_use);
    // The process's resident pages are in use: they are not counted as available.
    CHECK(host.physical.in_use >= host.resident.in_use);
#else
    CHECK_FALSE(host.physical.read);
    CHECK(host.reason[0] != '\0');
#endif
}

TEST_CASE("diag10c: the resident reading follows memory this process touches", "[core][pressure][host][diag10c]")
{
#if CRD_OS_WINDOWS || CRD_OS_LINUX
    const crd::HostMemoryReading before = crd::read_host_memory();
    REQUIRE(before.resident.read);
    crd::u64 peak = 0U;
    {
        const TouchedPages pages;
        REQUIRE(pages.mapped());
        const crd::HostMemoryReading during = crd::read_host_memory();
        REQUIRE(during.resident.read);
        INFO("before " << before.resident.in_use << " during " << during.resident.in_use);
        // Every touched page is resident at the moment it is written. The OS may trim a working set at any time
        // (Windows under memory pressure), so the reading must show at least half of what the test just touched.
        CHECK(during.resident.in_use >= before.resident.in_use + kTouchBytes / 2U);
        CHECK(during.resident.high_water >= during.resident.in_use);
        peak = during.resident.high_water;
    }
    // The pages are released; the peak is kept.
    const crd::HostMemoryReading after = crd::read_host_memory();
    REQUIRE(after.resident.read);
    CHECK(after.resident.high_water >= peak);
    CHECK(after.resident.high_water >= after.resident.in_use);
#else
    SUCCEED("no host memory probe on this OS");
#endif
}

TEST_CASE("diag10c: the resident peak never falls between readings while memory comes and goes",
          "[core][pressure][host][diag10c]")
{
#if CRD_OS_WINDOWS || CRD_OS_LINUX
    crd::u64 previous = 0U;
    for (int cycle = 0; cycle < 8; ++cycle)
    {
        {
            const TouchedPages pages;
            REQUIRE(pages.mapped());
            const crd::HostMemoryReading during = crd::read_host_memory();
            REQUIRE(during.resident.read);
            INFO("cycle " << cycle << " during: peak " << during.resident.high_water << " in use "
                          << during.resident.in_use << ", earlier peak " << previous);
            CHECK(during.resident.high_water >= previous);
            CHECK(during.resident.high_water >= during.resident.in_use);
            previous = during.resident.high_water;
        }
        const crd::HostMemoryReading after = crd::read_host_memory();
        REQUIRE(after.resident.read);
        INFO("cycle " << cycle << " after: peak " << after.resident.high_water << ", earlier peak " << previous);
        CHECK(after.resident.high_water >= previous);
        previous = after.resident.high_water;
    }
#else
    SUCCEED("no host memory probe on this OS");
#endif
}
