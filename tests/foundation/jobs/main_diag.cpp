#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <crd/core/crash.hpp>

#include <filesystem>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>

// Vectored exception handler fires before SetUnhandledExceptionFilter,
// including for some fast-fail paths.  Prints ExceptionCode+Address to
// stderr so CI logs capture the crash type without needing a debugger.
namespace
{
LONG CALLBACK fiber_diag_veh(EXCEPTION_POINTERS* ep) noexcept
{
    auto* rec = ep->ExceptionRecord;
    std::fprintf(stderr,
                 "[fiber-diag] ExceptionCode=0x%08lX ExceptionAddress=%p\n",
                 rec->ExceptionCode, rec->ExceptionAddress);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace
#endif

int main(int argc, char* argv[])
{
    // Minidump + stderr exception info for all crash types.
    // Install under the system temp dir, not the repo root: install() creates the directory, and a repo-root
    // crashes/ would trip the repository structure check.
    const std::string crash_dir = (std::filesystem::temp_directory_path() / "crd-test-crashes").string();
    (void)crd::crash::install(crash_dir.c_str());

#if defined(_WIN32)
    // VEH fires before the unhandled-exception filter — catches CET violations
    // (0xC0000409) that bypass SetUnhandledExceptionFilter.
    AddVectoredExceptionHandler(1, &fiber_diag_veh);
#endif

    return Catch::Session().run(argc, argv);
}
