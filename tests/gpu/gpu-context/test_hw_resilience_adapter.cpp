// HW-RESILIENCE.2: the adapter's driver version text and the process's opened-adapter log (crd/gpu/adapter.hpp),
// device-free.

#include <crd/gpu/adapter.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>

namespace g = crd::gpu;

namespace
{
g::GpuAdapter adapter(const char* backend, crd::u32 vendor, crd::u32 device, const char* version)
{
    g::GpuAdapter a{};
    (void)std::snprintf(a.backend, sizeof(a.backend), "%s", backend);
    (void)std::snprintf(a.name, sizeof(a.name), "GPU %04x", device);
    (void)std::snprintf(a.driver_version, sizeof(a.driver_version), "%s", version);
    a.vendor = vendor;
    a.device = device;
    a.known = true;
    return a;
}

// The listener's record (a plain function receives each adapter).
struct Heard
{
    g::GpuAdapter adapters[g::OpenedAdapters::kMaxOpenedAdapters + 1U]{};
    crd::u32 count = 0U;
};

Heard& heard()
{
    static Heard record;
    return record;
}

void hear(const g::GpuAdapter& a)
{
    Heard& h = heard();
    h.adapters[h.count] = a;
    ++h.count;
}
} // namespace

TEST_CASE("adapter: driver versions read as their vendors write them", "[gpu][diag][hw-resilience]")
{
    char out[g::kAdapterDriverVersionBytes];
    g::format_windows_driver_version((32ULL << 48U) | (0ULL << 32U) | (15ULL << 16U) | 8157ULL, out);
    CHECK(std::strcmp(out, "32.0.15.8157") == 0);
    g::format_windows_driver_version(0xFFFFFFFFFFFFFFFFULL, out);
    CHECK(std::strcmp(out, "65535.65535.65535.65535") == 0);

    // NVIDIA packs 10.8.8.6 bits on every OS.
    const crd::u32 nvidia = (581U << 22U) | (57U << 14U) | (3U << 6U) | 9U;
    g::format_vulkan_driver_version(0x10DEU, nvidia, false, out);
    CHECK(std::strcmp(out, "581.57.3.9") == 0);
    g::format_vulkan_driver_version(0x10DEU, nvidia, true, out);
    CHECK(std::strcmp(out, "581.57.3.9") == 0);
    // Intel packs 18.14 bits on Windows and VK_MAKE_API_VERSION on Linux (Mesa).
    const crd::u32 intel_windows = (101U << 14U) | 6130U;
    g::format_vulkan_driver_version(0x8086U, intel_windows, true, out);
    CHECK(std::strcmp(out, "101.6130") == 0);
    const crd::u32 made = (24U << 22U) | (2U << 12U) | 8U; // VK_MAKE_API_VERSION(0, 24, 2, 8)
    g::format_vulkan_driver_version(0x8086U, made, false, out);
    CHECK(std::strcmp(out, "24.2.8") == 0);
    g::format_vulkan_driver_version(0x1002U, made, true, out); // AMD
    CHECK(std::strcmp(out, "24.2.8") == 0);
    g::format_vulkan_driver_version(0x10005U, (7U << 29U) | made, false, out); // Mesa; the variant bits are not major
    CHECK(std::strcmp(out, "24.2.8") == 0);
}

TEST_CASE("adapter: the opened-adapter log keeps each adapter once, bounded, and replays it to a late listener",
          "[gpu][diag][hw-resilience]")
{
    g::OpenedAdapters log;
    CHECK(log.note(g::GpuAdapter{})); // unknown: ignored
    CHECK(log.count() == 0U);

    const g::GpuAdapter vk = adapter("vulkan", 0x10DEU, 0x2705U, "581.57");
    const g::GpuAdapter dx = adapter("dx12", 0x10DEU, 0x2705U, "32.0.15.8157");
    REQUIRE(log.note(vk));
    REQUIRE(log.note(dx)); // the same GPU through another API is its own entry
    REQUIRE(log.note(vk)); // the same adapter again is not
    g::GpuAdapter newer = vk;
    (void)std::snprintf(newer.driver_version, sizeof(newer.driver_version), "%s", "581.80");
    REQUIRE(log.note(newer)); // a driver update is
    REQUIRE(log.count() == 3U);
    CHECK(std::strcmp(log.at(1U).backend, "dx12") == 0);
    CHECK_FALSE(log.at(3U).known);

    heard().count = 0U;
    log.listen(&hear); // the late listener hears every adapter noted before it, in order
    REQUIRE(heard().count == 3U);
    CHECK(std::strcmp(heard().adapters[0].driver_version, "581.57") == 0);
    CHECK(std::strcmp(heard().adapters[2].driver_version, "581.80") == 0);
    for (crd::u32 i = 3U; i < g::OpenedAdapters::kMaxOpenedAdapters; ++i)
    {
        REQUIRE(log.note(adapter("cuda", 0x10DEU, i, "13.0")));
    }
    CHECK(heard().count == g::OpenedAdapters::kMaxOpenedAdapters);    // then each new one
    CHECK_FALSE(log.note(adapter("cuda", 0x10DEU, 0xFFFFU, "13.0"))); // full
    CHECK(log.note(vk));                                              // known ones still count as there
    CHECK(log.count() == g::OpenedAdapters::kMaxOpenedAdapters);
    CHECK(heard().count == g::OpenedAdapters::kMaxOpenedAdapters);

    log.listen(nullptr); // stopped: a new adapter is noted, not sent
    g::OpenedAdapters other;
    other.listen(&hear);
    other.listen(nullptr);
    REQUIRE(other.note(vk));
    CHECK(heard().count == g::OpenedAdapters::kMaxOpenedAdapters);
}
