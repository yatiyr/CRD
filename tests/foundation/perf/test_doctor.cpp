// DIAG.0 -- the diagnostics doctor's report and its dependency probes, including
// the missing-symbolizer control (the fifth negative); DIAG.11c -- its modes against the lane; HW-RESILIENCE.3 -- the
// kernel families' self-checks. Contract: docs/design/runtime-diagnostics.md; the doctor lives in crd-perf.

#include <crd/perf/doctor.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/containers/checksum_dispatch.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/kernel_dispatch.hpp>
#include <crd/core/platform.hpp>
#include <crd/diag/specimen_runner.hpp>
#include <crd/perf/diagnostic_host.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
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

bool fail_crc_above_portable(void* /*user*/, const char* family, crd::CpuLevel level,
                             crd::CpuFeatureSet /*features*/) noexcept
{
    return std::string_view{family} == "containers.crc" && level != crd::CpuLevel::Portable;
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

    // The CPU selection is the process's own (ADR-0134 §8), with the C runtime's dispatch where it has one.
    const crd::CpuSelection& cpu = crd::cpu_selection();
    CHECK(r.cpu.detected == cpu.detected);
    CHECK(r.cpu.selected == cpu.selected);
    CHECK(r.cpu.override_status == cpu.override_status);
    CHECK(r.cpu.kernel_features == cpu.kernel_features);
    CHECK(r.cpu_crt.crt_level == crd::cpu_runtime_dispatch().crt_level);

    // crash-capture follows the platform's backend; a route this report does not own is never claimed.
    const auto* crash = r.find_mode(cont::StringView{"crash-capture"});
    REQUIRE(crash != nullptr);
    CHECK(crash->compiled == cp::diagnostic_host_capabilities().crash_backend);
    // The emergency channel is crash-capture's record and the flight recorder's fatal path; no separate mode.
    CHECK(r.find_mode(cont::StringView{"emergency-recording"}) == nullptr);
    const auto* gpu = r.find_mode(cont::StringView{"gpu-fault"});
    REQUIRE(gpu != nullptr);
    CHECK_FALSE(gpu->compiled);
}

TEST_CASE("doctor: every kernel family the image links has run its self-check on the table it selected",
          "[diag][doctor]")
{
    const cp::DoctorReport r = cp::run_doctor();
    CHECK(r.kernel_demotions == 0U); // a sound CPU reproduces every known answer at every level
    crd::u32 families = 0U;
    bool crc = false;
    for (const crd::KernelFamily* f = r.kernel_families; f != nullptr; f = f->next)
    {
        ++families;
        INFO(f->name);
        CHECK(f->result.status == crd::KernelCheckStatus::Passed);
        CHECK(f->result.level == f->result.selected_level);
        CHECK(f->result.failed_routine == nullptr);
        crc = crc || std::string_view{f->name} == "containers.crc";
    }
    CHECK(crc); // the flight bundle's checksums link it into every diagnostics host
    CHECK(families >= 2U);
    CHECK(r.kernel_families == crd::kernel_families());
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

TEST_CASE("doctor child: the crc family resolves with every table above portable failed", "[.kernel-check-child]")
{
    namespace cc = crd::containers;
    const crd::CpuSelection& s = crd::cpu_selection();
    const crd::KernelLevel<cc::CrcKernels>& top = crd::select_kernel_level<cc::CrcKernels>(
        cc::crc_kernel_levels(), s.selected, s.kernel_features, crd::kernel_level_cap());
    const bool installed = crd::install_kernel_check_fault_hook(&fail_crc_above_portable, nullptr);
    CHECK(installed == crd::fault_seams_compiled());
    // The report resolves every family; the crc family's first resolution meets the hook.
    const auto start = std::chrono::steady_clock::now();
    const crd::u32 demoted = cp::report_kernel_self_checks(stdout);
    const auto took = std::chrono::steady_clock::now() - start;
    crd::uninstall_kernel_check_fault_hook();
    const bool fell = installed && &top != cc::crc_kernel_levels().data();
    CHECK(demoted == (fell ? 1U : 0U));
    CHECK(&cc::crc_kernels() == (installed ? cc::crc_kernel_levels().data() : &top));
    const crd::u8 bytes[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(cc::crc32(cc::ConstSpan<crd::u8>(bytes, sizeof bytes)) == 0xCBF43926U); // and it keeps running, right
    const cp::DoctorReport r = cp::run_doctor();
    CHECK(r.kernel_demotions == demoted);
    for (const crd::KernelFamily* f = r.kernel_families; f != nullptr; f = f->next)
    {
        if (std::string_view{f->name} == "containers.crc")
        {
            CHECK(f->result.status == (fell ? crd::KernelCheckStatus::Demoted : crd::KernelCheckStatus::Passed));
            CHECK(f->result.selected_level == top.level);
            CHECK(f->result.level == (installed ? crd::CpuLevel::Portable : top.level));
            CHECK(f->result.injected == fell);
        }
    }
    std::printf("kernel check child: fell=%d, every family resolved and checked in %lld us\n", fell ? 1 : 0,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(took).count()));
}

TEST_CASE("doctor: a process whose crc tables fail their checks keeps running on the portable one and says why",
          "[diag][doctor]")
{
    const cont::String self = crd::diag::self_executable();
    REQUIRE_FALSE(self.empty());
    cont::Array<cont::String> args;
    args.push_back(cont::String{"[.kernel-check-child]"});
    crd::diag::Expectation e;
    e.timeout_ms = 60000U;
    const crd::diag::Outcome o = crd::diag::run_specimen(self, args, e);
    const std::string_view out{o.output.data(), o.output.size()};
    INFO(out);
    CHECK_FALSE(o.crashed);
    CHECK_FALSE(o.timed_out);
    CHECK(o.exit_code == 0);
    CHECK(out.find("All tests passed") != std::string_view::npos);
    const bool fell = out.find("kernel check child: fell=1,") != std::string_view::npos;
    CHECK(fell != (out.find("kernel check child: fell=0,") != std::string_view::npos));
    const std::size_t timing = out.find("kernel check child: fell=");
    REQUIRE(timing != std::string_view::npos);
    WARN(out.substr(timing, out.find('\n', timing) - timing));
    // The line an application prints at startup names the family, the failed table and routine, and the table it runs.
    CHECK((out.find("[crd] kernel family containers.crc failed its known answers on ") != std::string_view::npos) ==
          fell);
    CHECK((out.find(" in injected (fault seam); it runs portable") != std::string_view::npos) == fell);
    // The child selects as this process does (it inherits CRD_CPU_LEVEL): it falls when a table above portable is
    // selected and the seam is compiled.
    namespace cc = crd::containers;
    const crd::CpuSelection& s = crd::cpu_selection();
    const crd::KernelLevel<cc::CrcKernels>& top = crd::select_kernel_level<cc::CrcKernels>(
        cc::crc_kernel_levels(), s.selected, s.kernel_features, crd::kernel_level_cap());
    CHECK(fell == (crd::fault_seams_compiled() && &top != cc::crc_kernel_levels().data()));
}
