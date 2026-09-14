// crd-perf -- the diagnostics doctor (DIAG.0). See crd/perf/doctor.hpp.

#include <crd/perf/doctor.hpp>

#include <crd/perf/config.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace crd::perf
{
namespace
{
namespace fs = std::filesystem;

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kBuiltWithAsan = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kBuiltWithAsan = true;
#else
constexpr bool kBuiltWithAsan = false;
#endif
#else
constexpr bool kBuiltWithAsan = false;
#endif

cont::String host_tuple_string()
{
    cont::String t;
#if defined(_WIN32)
    t.append("windows");
#elif defined(__linux__)
    t.append("linux");
#elif defined(__APPLE__)
    t.append("macos");
#else
    t.append("unknown-os");
#endif
    t.append("/");
#if defined(__x86_64__) || defined(_M_X64)
    t.append("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    t.append("arm64");
#else
    t.append("unknown-isa");
#endif
    t.append("/");
#if defined(__clang__)
    t.append("clang");
#elif defined(_MSC_VER)
    t.append("msvc");
#elif defined(__GNUC__)
    t.append("gcc");
#else
    t.append("unknown-cc");
#endif
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

DoctorReport run_doctor(cont::StringView output_dir)
{
    DoctorReport report;
    report.schema = cont::StringView{"cerid-diagnostics/1"};
    report.host_tuple = host_tuple_string();

    // ---- live-probed dependencies -------------------------------------------
    {
        DiagnosticDependency san;
        san.name = cont::StringView{"sanitizer_runtime"};
        san.present = kBuiltWithAsan;
        san.detail = cont::String{kBuiltWithAsan ? "asan" : "none"};
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

    // ---- modes (census-sourced; live dependencies fold into `usable`) --------
    const bool san_ready = kBuiltWithAsan;
    report.modes.push_back(mode(cont::StringView{"disabled"}, true, !kEnabled, true,
                                cont::StringView{"no-op path when the perf gate is off"}));
    report.modes.push_back(mode(cont::StringView{"basic-recording"}, kEnabled, kEnabled, kEnabled,
                                cont::StringView{"unqualified overhead/budget; DIAG.2"}));
    report.modes.push_back(mode(cont::StringView{"emergency-recording"}, false, false, false,
                                cont::StringView{"unsupported; DIAG.5"}));
    report.modes.push_back(mode(cont::StringView{"crash-capture"}, true, true, true,
                                cont::StringView{"unqualified fatal path; DIAG.5a/5b"}));
    report.modes.push_back(mode(cont::StringView{"sanitizer-coverage"}, kBuiltWithAsan, kBuiltWithAsan, san_ready,
                                cont::StringView{"unqualified; DIAG.1b/3f"}));
    report.modes.push_back(mode(cont::StringView{"gpu-fault"}, false, false, false,
                                cont::StringView{"unsupported; DIAG.7"}));
    report.modes.push_back(mode(cont::StringView{"authored-provenance"}, true, true, true,
                                cont::StringView{"unqualified full chain; DIAG.8"}));

    return report;
}

} // namespace crd::perf
