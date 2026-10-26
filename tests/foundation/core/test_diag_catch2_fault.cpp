// A fault inside a Catch2 test case reaches the next handler as itself
// (cmake/patches/catch2-posix-signal-refault.cmake).
//
// Catch2's POSIX fatal-condition handler reported the case, restored the previous handlers and re-raised the signal, so
// a sanitizer received a sent signal: its report named the sender's {pid, uid} (0x03e8 with the process id) as the
// faulting address and the real one was lost (docs/sessions/2026-10-26-catch2-fault-address.md). Each parent case here
// re-enters this executable on a hidden child case through the DIAG.0 specimen harness and reads its whole output:
// Catch2 still names the case where it sees the fault, a sanitizer names the faulting address and function, a build
// without one dies by the fault itself, and a signal sent to the case still ends it. Windows runs the same wild read
// through Catch2's SEH handler, which already lets the exception continue to the next handler.

#include <crd/diag/specimen_runner.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace
{
namespace cont = crd::containers;
namespace cd   = crd::diag;

// A canonical user address no 64-bit Windows or Linux process maps, nor any sanitizer's shadow or metadata range.
constexpr std::uintptr_t kWildAddress = 0x5a5a00001000U;
constexpr const char*    kWildText    = "unknown address 0x5a5a00001000";

// The load is not instrumented, so a sanitizer's own shadow check cannot fault first.
#if defined(_MSC_VER) && !defined(__clang__)
__declspec(noinline) __declspec(no_sanitize_address)
#else
__attribute__((noinline, no_sanitize("address", "thread")))
#endif
int read_wild_address()
{
    return *reinterpret_cast<const volatile int*>(kWildAddress); // NOLINT(performance-no-int-to-ptr): the fault
}

void headless_child()
{
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}

cont::String self_path()
{
    char path[4096] = {};
#if defined(_WIN32)
    const DWORD n = ::GetModuleFileNameA(nullptr, path, sizeof(path));
    REQUIRE((n > 0U && n < sizeof(path)));
#else
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof(path) - 1U);
    REQUIRE((n > 0 && static_cast<std::size_t>(n) < sizeof(path) - 1U));
#endif
    return cont::String{path};
}

cd::Outcome run_child(const char* tag)
{
    cont::Array<cont::String> args;
    args.push_back(cont::String{tag});
    cd::Expectation e;
    e.want       = cd::Expectation::Want::Crash;
    e.timeout_ms = 30000U;
    return cd::run_specimen(self_path(), args, e);
}

bool sanitized()
{
    const cd::LanePromise promise = cd::lane_promise();
    return promise.asan || promise.tsan;
}

// Catch2 names the failing case, except where it never sees the fault: ThreadSanitizer trees compile no Catch2 POSIX
// handler (its report allocates inside the signal handler, which TSan with halt_on_error ends the process on before
// the fault is named), and on Windows AddressSanitizer's vectored handler runs first and ends the process.
bool catch2_reports()
{
    const cd::LanePromise promise = cd::lane_promise();
#if defined(_WIN32)
    return !promise.asan;
#else
    return !promise.tsan;
#endif
}

bool has(const cd::Outcome& o, std::string_view text)
{
    return std::string_view{o.output.data(), o.output.size()}.find(text) != std::string_view::npos;
}
} // namespace

TEST_CASE("catch2 fault child: a wild read", "[.catch2-fault-child-read]")
{
    headless_child();
    CHECK(read_wild_address() == 0);
}

TEST_CASE("catch2 fault: a wild read in a test case reaches the next handler with its own address",
          "[diag][catch2-fault]")
{
    const cd::Outcome o = run_child("[.catch2-fault-child-read]");
    INFO("exit " << o.exit_code << ", " << o.reason.c_str());
    REQUIRE_FALSE(o.timed_out);
    if (catch2_reports())
    {
        CHECK(has(o, "catch2 fault child: a wild read"));
        CHECK(has(o, "SIGSEGV - Segmentation violation signal"));
    }
    if (sanitized())
    {
        CHECK(o.exit_code != 0);
        CHECK(has(o, kWildText)); // not the sender's {pid, uid} of a re-raised signal
        CHECK(has(o, "read_wild_address"));
    }
    else
    {
        CHECK(o.crashed);
#if defined(_WIN32)
        CHECK(static_cast<std::uint32_t>(o.exit_code) == 0xC0000005U); // EXCEPTION_ACCESS_VIOLATION
#else
        CHECK(o.exit_code == 128 + SIGSEGV);
#endif
    }
}

#if !defined(_WIN32)
TEST_CASE("catch2 fault child: a sent SIGSEGV", "[.catch2-fault-child-sent]")
{
    (void)std::raise(SIGSEGV);
    std::printf("CHILD CONTINUED AFTER A SENT SIGSEGV\n");
    std::fflush(stdout);
}

TEST_CASE("catch2 fault: a signal sent to a test case still ends it by that signal", "[diag][catch2-fault]")
{
    const cd::Outcome o = run_child("[.catch2-fault-child-sent]");
    INFO("exit " << o.exit_code << ", " << o.reason.c_str());
    REQUIRE_FALSE(o.timed_out);
    if (catch2_reports())
    {
        CHECK(has(o, "SIGSEGV - Segmentation violation signal"));
    }
    CHECK_FALSE(has(o, "CHILD CONTINUED"));
    if (sanitized())
    {
        CHECK(o.exit_code != 0);
        CHECK(has(o, "SEGV on unknown address"));
    }
    else
    {
        CHECK(o.crashed);
        CHECK(o.exit_code == 128 + SIGSEGV);
    }
}
#endif
