// HW-RESILIENCE.3: the process's GPU device checks (crd/gpu/device_check.hpp), device-free: each adapter is checked
// once (the cached outcome after the first), the device-check fault seam fails a named adapter in place of the check,
// a backend's selection reaches the listener only when it changes, and the demotion line says what failed and what
// runs instead.

#include <crd/core/fault_seam.hpp>
#include <crd/gpu/device_check.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace g = crd::gpu;

namespace
{
g::GpuAdapter adapter(const char* backend, const char* name)
{
    g::GpuAdapter a{};
    (void)std::snprintf(a.backend, sizeof(a.backend), "%s", backend);
    (void)std::snprintf(a.name, sizeof(a.name), "%s", name);
    a.vendor = 0x10DEU;
    a.device = 0x2705U;
    a.known = true;
    return a;
}

struct Runs
{
    int calls = 0;
    g::DeviceCheckOutcome answer;
};

g::DeviceCheckOutcome run_check(void* user)
{
    auto* const r = static_cast<Runs*>(user);
    ++r->calls;
    return r->answer;
}

bool fail_named(void* user, const char* backend, const char* name) noexcept
{
    return std::strcmp(backend, "vulkan") == 0 && std::strcmp(name, static_cast<const char*>(user)) == 0;
}

struct Heard
{
    int count = 0;
    g::DeviceSelection last;
};

Heard& heard()
{
    static Heard record;
    return record;
}

void hear(const g::DeviceSelection& s)
{
    ++heard().count;
    heard().last = s;
}

// A scratch stream the report writes to (MSVC deprecates std::tmpfile for tmpfile_s).
std::FILE* scratch_file()
{
#if defined(_MSC_VER)
    std::FILE* f = nullptr;
    return tmpfile_s(&f) == 0 ? f : nullptr;
#else
    return std::tmpfile();
#endif
}

// Every case starts from an unchecked process and leaves no listener or hook behind.
struct Fresh
{
    Fresh() { g::device_checks().forget(); }
    ~Fresh()
    {
        g::device_checks().listen(nullptr);
        g::device_checks().forget();
        crd::uninstall_device_check_fault_hook();
    }
    Fresh(const Fresh&) = delete;
    Fresh& operator=(const Fresh&) = delete;
    Fresh(Fresh&&) = delete;
    Fresh& operator=(Fresh&&) = delete;
};
} // namespace

TEST_CASE("hw-resilience 3: an adapter's device check runs once per process", "[gpu-context][hw-resilience]")
{
    const Fresh fresh;
    Runs runs;
    const g::GpuAdapter a = adapter("vulkan", "Adapter A");
    const g::DeviceCheck first = g::check_device_once(a, 0U, &run_check, &runs);
    CHECK(first.failed == nullptr);
    CHECK_FALSE(first.injected);
    CHECK(first.ordinal == 0U);
    const g::DeviceCheck again = g::check_device_once(a, 0U, &run_check, &runs);
    CHECK(again.failed == nullptr);
    CHECK(runs.calls == 1); // the logged outcome, not a second run
    CHECK(g::device_checks().check_count() == 1U);

    // The same adapter at another ordinal (a second identical GPU) is its own check.
    runs.answer.failed = "arithmetic";
    const g::DeviceCheck twin = g::check_device_once(a, 1U, &run_check, &runs);
    CHECK(runs.calls == 2);
    REQUIRE(twin.failed != nullptr);
    CHECK(std::string_view{twin.failed} == "arithmetic");
    g::DeviceCheck found;
    REQUIRE(g::device_checks().find(a, 1U, found));
    CHECK(std::string_view{found.failed} == "arithmetic");
    CHECK_FALSE(g::device_checks().find(adapter("vulkan", "Adapter B"), 0U, found));

    g::device_checks().forget();
    CHECK(g::device_checks().check_count() == 0U);
    (void)g::check_device_once(a, 0U, &run_check, &runs);
    CHECK(runs.calls == 3); // forgotten: checked again

    // A check that cannot run on this host (no shader compiler) is no failure: the adapter is used, unchecked.
    runs.answer = g::DeviceCheckOutcome{nullptr, "no GLSL to SPIR-V compile"};
    const g::DeviceCheck unchecked = g::check_device_once(adapter("vulkan", "Adapter C"), 2U, &run_check, &runs);
    CHECK(unchecked.failed == nullptr);
    REQUIRE(unchecked.unchecked != nullptr);
    CHECK(std::string_view{unchecked.unchecked} == "no GLSL to SPIR-V compile");
    // A failure wins over a reason: the device failed, whatever else the run said.
    runs.answer = g::DeviceCheckOutcome{"arithmetic", "ignored"};
    const g::DeviceCheck both = g::check_device_once(adapter("vulkan", "Adapter D"), 3U, &run_check, &runs);
    CHECK(std::string_view{both.failed} == "arithmetic");
    CHECK(both.unchecked == nullptr);
}

TEST_CASE("hw-resilience 3: the device-check fault seam fails a named adapter in place of its check",
          "[gpu-context][hw-resilience]")
{
    const Fresh fresh;
    char victim[] = "Adapter A"; // NOLINT(modernize-avoid-c-arrays)
    if (!crd::fault_seams_compiled())
    {
        CHECK_FALSE(crd::install_device_check_fault_hook(&fail_named, victim));
        CHECK_FALSE(crd::consult_device_check_fault("vulkan", victim));
        return;
    }
    REQUIRE(crd::install_device_check_fault_hook(&fail_named, victim));
    CHECK_FALSE(crd::install_device_check_fault_hook(&fail_named, victim)); // one hook
    Runs runs;
    const g::DeviceCheck failed = g::check_device_once(adapter("vulkan", "Adapter A"), 0U, &run_check, &runs);
    REQUIRE(failed.failed != nullptr);
    CHECK(std::string_view{failed.failed} == "injected");
    CHECK(failed.injected);
    CHECK(runs.calls == 0); // the seam answered before the check ran
    const g::DeviceCheck other = g::check_device_once(adapter("vulkan", "Adapter B"), 1U, &run_check, &runs);
    CHECK(other.failed == nullptr);
    CHECK(runs.calls == 1);
    const g::DeviceCheck dx12 = g::check_device_once(adapter("dx12", "Adapter A"), 0U, &run_check, &runs);
    CHECK(dx12.failed == nullptr); // the hook names a backend too
}

TEST_CASE("hw-resilience 3: a backend's selection reaches the listener when it changes, and its line says why",
          "[gpu-context][hw-resilience]")
{
    const Fresh fresh;
    heard() = Heard{};
    g::DeviceSelection passed;
    passed.status = g::DeviceCheckStatus::Passed;
    passed.selected = adapter("vulkan", "Adapter A");
    passed.runs = passed.selected;
    g::device_checks().note_selection(passed);
    g::device_checks().listen(&hear);
    CHECK(heard().count == 1); // the selection noted before listening
    g::device_checks().note_selection(passed);
    CHECK(heard().count == 1); // unchanged: not heard again

    g::DeviceSelection demoted;
    demoted.status = g::DeviceCheckStatus::Demoted;
    demoted.selected = adapter("vulkan", "Adapter A");
    demoted.runs = adapter("vulkan", "Adapter B");
    demoted.failed = "workgroup-sum";
    demoted.adapters_failed = 1U;
    g::device_checks().note_selection(demoted);
    CHECK(heard().count == 2);
    CHECK(heard().last.status == g::DeviceCheckStatus::Demoted);
    CHECK(g::device_checks().selection_count() == 1U); // one per backend: replaced

    g::DeviceSelection none;
    none.status = g::DeviceCheckStatus::NoDevicePassed;
    none.selected = adapter("dx12", "Adapter C");
    none.failed = "injected";
    none.injected = true;
    none.adapters_failed = 2U;
    g::device_checks().note_selection(none);
    CHECK(heard().count == 3);
    CHECK(g::device_checks().selection_count() == 2U);

    char line[512] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)g::format_device_demotion(demoted, line, sizeof(line));
    CHECK(std::string_view{line} == "[crd] GPU device path vulkan on Adapter A failed its known answers in "
                                    "workgroup-sum; it runs Adapter B (demoted, 1 adapters failed)");
    (void)g::format_device_demotion(none, line, sizeof(line));
    CHECK(std::string_view{line} == "[crd] GPU device path dx12 on Adapter C failed its known answers in injected "
                                    "(fault seam); it runs no device (no-device-passed, 2 adapters failed)");
    char tiny[8] = {};                                                            // NOLINT(modernize-avoid-c-arrays)
    CHECK(g::format_device_demotion(demoted, tiny, sizeof(tiny)) > sizeof(tiny)); // the length the line needs
    CHECK(std::strlen(tiny) == sizeof(tiny) - 1U);

    g::DeviceSelection unchecked;
    unchecked.status = g::DeviceCheckStatus::Unchecked;
    unchecked.selected = adapter("vulkan", "Adapter A");
    unchecked.runs = unchecked.selected;
    unchecked.unchecked = "no GLSL to SPIR-V compile";
    (void)g::format_device_demotion(unchecked, line, sizeof(line));
    CHECK(std::string_view{line} == "[crd] GPU device path vulkan on Adapter A ran no known-answer check (no GLSL to "
                                    "SPIR-V compile); it runs unchecked (unchecked, 0 adapters failed)");
    demoted.unchecked = "no GLSL to SPIR-V compile";
    (void)g::format_device_demotion(demoted, line, sizeof(line));
    CHECK(std::string_view{line} == "[crd] GPU device path vulkan on Adapter A failed its known answers in "
                                    "workgroup-sum; it runs Adapter B unchecked (no GLSL to SPIR-V compile) (demoted, "
                                    "1 adapters failed)");
}

TEST_CASE("hw-resilience 3: the demotion report writes one line per demoted selection", "[gpu-context][hw-resilience]")
{
    const Fresh fresh;
    std::FILE* const out = scratch_file();
    REQUIRE(out != nullptr);
    g::DeviceSelection passed;
    passed.status = g::DeviceCheckStatus::Passed;
    passed.selected = adapter("vulkan", "Adapter A");
    passed.runs = passed.selected;
    g::device_checks().note_selection(passed);
    g::DeviceSelection demoted;
    demoted.status = g::DeviceCheckStatus::Demoted;
    demoted.selected = adapter("dx12", "Adapter A");
    demoted.runs = adapter("dx12", "Adapter B");
    demoted.failed = "arithmetic";
    demoted.adapters_failed = 1U;
    g::device_checks().note_selection(demoted);
    g::report_device_demotions(out); // hears both noted selections; only the demotion is written
    g::report_device_demotions(nullptr);
    REQUIRE(std::fseek(out, 0, SEEK_SET) == 0);
    char text[1024] = {}; // NOLINT(modernize-avoid-c-arrays)
    const crd::usize n = std::fread(text, 1U, sizeof(text) - 1U, out);
    (void)std::fclose(out);
    const std::string_view written{text, n};
    CHECK(written.find("dx12 on Adapter A failed its known answers in arithmetic; it runs Adapter B") !=
          std::string_view::npos);
    CHECK(written.find("vulkan") == std::string_view::npos);
    CHECK(written.find('\n') == written.size() - 1U); // one line
}
