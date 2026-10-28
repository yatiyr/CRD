// crd-perf -- the diagnostics doctor. See crd/perf/doctor.hpp.

#include <crd/perf/doctor.hpp>

#include <crd/perf/build_identity.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <crd/core/fault_seam.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string_view>

namespace crd::perf
{
namespace
{
namespace fs = std::filesystem;

// "platform/arch/compiler version/configuration", from the identity a bundle records for this build.
cont::String host_tuple_string(const BuildIdentity& build)
{
    cont::String t;
    for (const cont::String* field : {&build.platform, &build.arch, &build.compiler_version, &build.config})
    {
        if (!t.empty())
        {
            t.push_back('/');
        }
        t.append(cont::StringView{field->data(), field->size()});
    }
    return t;
}

// Read an environment variable without tripping the MSVC deprecation of getenv
// (mirrors crd-platform's context.cpp). Empty when unset.
cont::String read_env(const char* name)
{
    cont::String out;
#if defined(_WIN32)
    char*        value = nullptr;
    std::size_t  len = 0;
    if (_dupenv_s(&value, &len, name) == 0 && value != nullptr)
    {
        out = cont::String{value};
        std::free(value);
    }
#else
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe,cert-env33-c) -- diagnostic probe, read once
    if (v != nullptr)
    {
        out = cont::String{v};
    }
#endif
    return out;
}

// Can a probe file be created in `dir`? False for a missing directory or a file
// used as a directory (the denied-output control), true only for a writable dir.
bool output_writable(cont::StringView dir)
{
    std::error_code ec;
    const fs::path  base(std::string_view{dir});
    if (!fs::is_directory(base, ec))
    {
        return false;
    }
    const fs::path probe = base / ".crd-doctor-write-probe";
    std::FILE*     f = nullptr;
#if defined(_WIN32)
    if (::fopen_s(&f, probe.string().c_str(), "wb") != 0)
    {
        f = nullptr;
    }
#else
    f = std::fopen(probe.string().c_str(), "wb");
#endif
    if (f == nullptr)
    {
        return false;
    }
    std::fclose(f);
    fs::remove(probe, ec);
    return true;
}

DiagnosticMode mode(cont::StringView name, bool compiled, bool enabled, bool usable, cont::StringView disposition)
{
    return DiagnosticMode{name, compiled, enabled, usable, disposition};
}

// A mode of this build that is present or absent as a whole, with what it is, or what builds it.
DiagnosticMode built_mode(cont::StringView name, bool built, cont::StringView present, cont::StringView absent)
{
    return mode(name, built, built, built, built ? present : absent);
}

// True when `commands` names every one of `names`.
bool serves(cont::ConstSpan<cont::StringView> commands, std::initializer_list<cont::StringView> names) noexcept
{
    for (const cont::StringView name : names)
    {
        bool found = false;
        for (const cont::StringView command : commands)
        {
            found = found || std::string_view{command} == std::string_view{name};
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}
} // namespace

const DiagnosticMode* DoctorReport::find_mode(cont::StringView name) const noexcept
{
    const std::string_view want{name};
    for (crd::usize i = 0; i < modes.size(); ++i)
    {
        if (std::string_view{modes[i].name} == want)
        {
            return &modes[i];
        }
    }
    return nullptr;
}

const DiagnosticDependency* DoctorReport::find_dependency(cont::StringView name) const noexcept
{
    const std::string_view want{name};
    for (crd::usize i = 0; i < dependencies.size(); ++i)
    {
        if (std::string_view{dependencies[i].name} == want)
        {
            return &dependencies[i];
        }
    }
    return nullptr;
}

DoctorReport run_doctor(cont::StringView output_dir, const DoctorLive& live)
{
    DoctorReport report;
    report.schema = cont::StringView{"cerid-diagnostics/1"};
    const BuildIdentity build = current_build_identity();
    const bool asan = (build.flags & kBuildFlagAsan) != 0U;
    const bool tsan = (build.flags & kBuildFlagTsan) != 0U;
    report.host_tuple = host_tuple_string(build);
    report.cpu = crd::cpu_selection();
    report.cpu_crt = crd::cpu_runtime_dispatch();

    // ---- live-probed dependencies -------------------------------------------
    {
        DiagnosticDependency san;
        san.name = cont::StringView{"sanitizer_runtime"};
        san.present = asan || tsan;
        san.detail = cont::String{"none"};
        if (san.present)
        {
            san.detail = cont::String{asan ? "asan" : "tsan"}; // exclusive instruments
        }
        report.dependencies.push_back(static_cast<DiagnosticDependency&&>(san));
    }
    {
        DiagnosticDependency sym;
        sym.name = cont::StringView{"symbolizer"};
        const cont::String path = read_env("ASAN_SYMBOLIZER_PATH");
        if (path.empty())
        {
            // Unset: rely on the toolchain default; treated as available but noted.
            sym.present = true;
            sym.detail = cont::String{"default (ASAN_SYMBOLIZER_PATH unset)"};
        }
        else
        {
            std::error_code ec;
            sym.present = std::filesystem::exists(std::filesystem::path(std::string_view{path}), ec);
            sym.detail = path;
        }
        report.dependencies.push_back(static_cast<DiagnosticDependency&&>(sym));
    }
    {
        DiagnosticDependency out;
        out.name = cont::StringView{"output_path"};
        if (output_dir.empty())
        {
            out.present = false;
            out.detail = cont::String{"not checked (no output path given)"};
        }
        else
        {
            out.present = output_writable(output_dir);
            out.detail = cont::String{output_dir};
        }
        report.dependencies.push_back(static_cast<DiagnosticDependency&&>(out));
    }

    // ---- modes of this build -----------------------------------------------
    const cont::StringView profiling_absent{"needs a build with CRD_ENABLE_PROFILING"};
    report.modes.push_back(mode(cont::StringView{"disabled"}, true, !kEnabled, true,
                                cont::StringView{"no-op path when the perf gate is off"}));
    report.modes.push_back(built_mode(cont::StringView{"basic-recording"}, kEnabled,
                                      cont::StringView{"profiler regions and the basic flight detail"},
                                      profiling_absent));
    report.modes.push_back(built_mode(cont::StringView{"full-profile"}, kEnabled,
                                      cont::StringView{"the full flight detail, chosen by the host's flight policy"},
                                      profiling_absent));
    report.modes.push_back(built_mode(cont::StringView{"crash-capture"}, diagnostic_host_capabilities().crash_backend,
                                      cont::StringView{"crd::crash: a Windows minidump, a signal-safe Linux record"},
                                      cont::StringView{"no crash backend on this platform (MAC.DIAG, WEB.DIAG)"}));
    report.modes.push_back(built_mode(cont::StringView{"memory-check"}, asan,
                                      cont::StringView{"this build carries AddressSanitizer"},
                                      cont::StringView{"needs its own build: CRD_ENABLE_ASAN"}));
    report.modes.push_back(built_mode(cont::StringView{"race-check"}, tsan,
                                      cont::StringView{"this build carries ThreadSanitizer"},
                                      cont::StringView{"needs its own GCC or Clang build: CRD_ENABLE_TSAN"}));
    report.modes.push_back(mode(cont::StringView{"gpu-fault"}, false, false, false,
                                cont::StringView{"not reported here: the GPU device reports validation and faults"}));

    // ---- live modes (DIAG.9b): read from the host's instances ----------------
    {
        const FlightRecorder* flight  = live.flight_recorder;
        const bool            enabled = flight != nullptr && flight->is_initialized();
        const bool            usable  = enabled && !flight->fatal_entered();
        cont::StringView      disposition{"triggered bounded capture; DIAG.9b"};
        if (flight == nullptr)
        {
            disposition = cont::StringView{"no flight recorder installed by this host; DIAG.9b"};
        }
        else if (!enabled)
        {
            disposition = cont::StringView{"flight recorder installed but not initialised; DIAG.9b"};
        }
        else if (!usable)
        {
            disposition = cont::StringView{"a capture ended on a fatal path; the recorder records no more; DIAG.9b"};
        }
        report.modes.push_back(mode(cont::StringView{"flight-recorder"}, true, enabled, usable, disposition));
    }
    {
        const FaultInjector* injector = live.fault_injector;
        const bool           compiled = crd::fault_seams_compiled();
        const bool           enabled  = compiled && injector != nullptr && injector->armed();
        const bool           usable   = compiled && injector != nullptr;
        cont::StringView     disposition{"test-only seeded injection, needs the inject grant; DIAG.9b"};
        if (!compiled)
        {
            disposition = cont::StringView{"compiled out of this shipping build; DIAG.9b"};
        }
        else if (injector == nullptr)
        {
            disposition = cont::StringView{"no fault injector installed by this host; DIAG.9b"};
        }
        report.modes.push_back(mode(cont::StringView{"fault-injection"}, compiled, enabled, usable, disposition));
    }

    // ---- upper-module modes: the commands the host registered with its service ----
    report.modes.push_back(
        built_mode(cont::StringView{"controlled-replay"},
                   serves(live.commands, {cont::StringView{"replay.record"}, cont::StringView{"replay.run"}}),
                   cont::StringView{"served by replay.record and replay.run"},
                   cont::StringView{"replay.record and replay.run not registered by this host"}));
    report.modes.push_back(built_mode(cont::StringView{"authored-provenance"},
                                      serves(live.commands, {cont::StringView{"program.provenance"}}),
                                      cont::StringView{"served by program.provenance"},
                                      cont::StringView{"program.provenance not registered by this host"}));

    // No build replays external I/O (DIAG.9b): a run record holds no external I/O completions or opaque call results,
    // and replay.run refuses a program that needs them, decided from its ops (crd-ceir-cook check_external_replay).
    report.modes.push_back(mode(cont::StringView{"external-io-replay"}, false, false, false,
                                cont::StringView{"refused in every build; records hold no external results; DIAG.9b"}));

    return report;
}

} // namespace crd::perf
