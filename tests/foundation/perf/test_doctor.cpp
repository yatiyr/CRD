// DIAG.0 -- the diagnostics doctor's report and its dependency probes, including
// the missing-symbolizer control (the fifth negative); DIAG.11c -- its modes against the lane. Contract:
// docs/design/runtime-diagnostics.md; the doctor lives in crd-perf.

#include <crd/perf/doctor.hpp>

#include <crd/core/platform.hpp>
#include <crd/diag/specimen_runner.hpp>
#include <crd/perf/diagnostic_host.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string_view>

namespace
{
namespace cp = crd::perf;
namespace cont = crd::containers;

void set_env(const char* name, const char* value)
{
#if defined(_WIN32)
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}
void clear_env(const char* name)
{
#if defined(_WIN32)
    (void)_putenv_s(name, "");
#else
    (void)unsetenv(name);
#endif
}
} // namespace

TEST_CASE("doctor: reports the schema, host tuple and the diagnostic modes", "[diag][doctor][diag-workflow]")
{
    const cp::DoctorReport r = cp::run_doctor();
    CHECK(cont::StringView{r.schema} == cont::StringView{"cerid-diagnostics/1"});
    // The tuple is the build identity's: platform/arch/compiler version/configuration.
    const std::string_view tuple{r.host_tuple};
    CHECK(tuple.starts_with(std::string_view{crd::platform_name()}));
    CHECK(tuple.find(std::string_view{crd::arch_name()}) != std::string_view::npos);
    CHECK(tuple.ends_with("/debug") != tuple.ends_with("/release"));

    // crash-capture follows the platform's backend; a route this report does not own is never claimed.
    const auto* crash = r.find_mode(cont::StringView{"crash-capture"});
    REQUIRE(crash != nullptr);
    CHECK(crash->compiled == cp::diagnostic_host_capabilities().crash_backend);
    const auto* emergency = r.find_mode(cont::StringView{"emergency-recording"});
    REQUIRE(emergency != nullptr);
    CHECK(emergency->usable);
    const auto* gpu = r.find_mode(cont::StringView{"gpu-fault"});
    REQUIRE(gpu != nullptr);
    CHECK_FALSE(gpu->compiled);
}

TEST_CASE("doctor: the instrument modes are the lane's, and an absent one names its build", "[diag][doctor]")
{
    // DIAG.11c absent-mode evidence: the binary's own sanitizer against the configuration's promise.
    const crd::diag::LanePromise lane = crd::diag::lane_promise();
    const cp::DoctorReport       r    = cp::run_doctor();
    const struct
    {
        const char* name;
        bool        promised;
        const char* option;
    } instruments[] = {{"memory-check", lane.asan, "CRD_ENABLE_ASAN"}, {"race-check", lane.tsan, "CRD_ENABLE_TSAN"}};
    for (const auto& instrument : instruments)
    {
        INFO(instrument.name);
        const auto* m = r.find_mode(cont::StringView{instrument.name});
        REQUIRE(m != nullptr);
        CHECK(m->compiled == instrument.promised);
        CHECK(m->usable == instrument.promised);
        CHECK((std::string_view{m->disposition}.find(instrument.option) != std::string_view::npos) ==
              !instrument.promised);
    }
    const auto* san = r.find_dependency(cont::StringView{"sanitizer_runtime"});
    REQUIRE(san != nullptr);
    CHECK(san->present == (lane.asan || lane.tsan));
}

TEST_CASE("doctor: an upper module's modes follow the host's registered commands", "[diag][doctor]")
{
    const cp::DoctorReport bare = cp::run_doctor();
    CHECK_FALSE(bare.find_mode(cont::StringView{"controlled-replay"})->usable);
    CHECK_FALSE(bare.find_mode(cont::StringView{"authored-provenance"})->usable);

    // Both replay commands serve controlled replay; one alone does not.
    const cont::StringView half[] = {cont::StringView{"replay.run"}, cont::StringView{"program.provenance"}};
    cp::DoctorLive         live;
    live.commands               = cont::ConstSpan<cont::StringView>{half, 2U};
    const cp::DoctorReport some = cp::run_doctor(cont::StringView{}, live);
    CHECK_FALSE(some.find_mode(cont::StringView{"controlled-replay"})->compiled);
    CHECK(some.find_mode(cont::StringView{"authored-provenance"})->usable);

    const cont::StringView all[] = {cont::StringView{"replay.record"}, cont::StringView{"replay.run"}};
    live.commands                = cont::ConstSpan<cont::StringView>{all, 2U};
    const cp::DoctorReport both  = cp::run_doctor(cont::StringView{}, live);
    CHECK(both.find_mode(cont::StringView{"controlled-replay"})->usable);
    CHECK_FALSE(both.find_mode(cont::StringView{"authored-provenance"})->compiled);
}

TEST_CASE("doctor: a missing symbolizer is detected", "[diag][doctor]")
{
    set_env("ASAN_SYMBOLIZER_PATH", "/nonexistent/crd-no-such-symbolizer");
    const cp::DoctorReport r = cp::run_doctor();
    const auto* sym = r.find_dependency(cont::StringView{"symbolizer"});
    REQUIRE(sym != nullptr);
    CHECK_FALSE(sym->present); // the bogus path does not resolve
    clear_env("ASAN_SYMBOLIZER_PATH");
}

TEST_CASE("doctor: an unset symbolizer path is reported available (toolchain default)", "[diag][doctor]")
{
    clear_env("ASAN_SYMBOLIZER_PATH");
    const cp::DoctorReport r = cp::run_doctor();
    const auto* sym = r.find_dependency(cont::StringView{"symbolizer"});
    REQUIRE(sym != nullptr);
    CHECK(sym->present);
}

TEST_CASE("doctor: output-path writability is probed", "[diag][doctor]")
{
    // A file used as an output directory is unusable (the denied-output control)...
    const cp::DoctorReport denied = cp::run_doctor(cont::StringView{CRD_DIAG_CLEAN_SPECIMEN});
    const auto* out_denied = denied.find_dependency(cont::StringView{"output_path"});
    REQUIRE(out_denied != nullptr);
    CHECK_FALSE(out_denied->present);

    // ...while the current working directory is writable.
    const cp::DoctorReport ok = cp::run_doctor(cont::StringView{"."});
    const auto* out_ok = ok.find_dependency(cont::StringView{"output_path"});
    REQUIRE(out_ok != nullptr);
    CHECK(out_ok->present);
}
