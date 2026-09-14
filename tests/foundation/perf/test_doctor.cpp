// DIAG.0 -- the diagnostics doctor's report and its dependency probes, including
// the missing-symbolizer control (the fifth negative). Contract:
// docs/design/runtime-diagnostics.md; the doctor lives in crd-perf.

#include <crd/perf/doctor.hpp>

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

TEST_CASE("doctor: reports the schema, host tuple and the diagnostic modes", "[diag][doctor]")
{
    const cp::DoctorReport r = cp::run_doctor();
    CHECK(cont::StringView{r.schema} == cont::StringView{"cerid-diagnostics/1"});
    CHECK_FALSE(cont::StringView{r.host_tuple}.empty());
    CHECK(r.modes.size() >= 5U);

    // A mode that exists is reported compiled; a route with no implementation is
    // reported unsupported -- the doctor never pretends a route is a command.
    const auto* crash = r.find_mode(cont::StringView{"crash-capture"});
    REQUIRE(crash != nullptr);
    CHECK(crash->compiled);
    const auto* gpu = r.find_mode(cont::StringView{"gpu-fault"});
    REQUIRE(gpu != nullptr);
    CHECK_FALSE(gpu->compiled);
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
