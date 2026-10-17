#pragma once

// One expected CRD_FATAL per run, judged by CTest from the output (crd_diag_fatal_tests in cmake/CrdDiag.cmake).
// A contract-fatal specimen arms the mode it runs with the message the fatal must carry, then breaks the contract. The
// assert platform handler prints `CRD_DIAG_FATAL_OK <mode>` and exits 42 when that message fires; any other assert
// prints `CRD_DIAG_FATAL_OTHER <message>` and exits 97. A mode whose fatal never fires returns from the violation and
// exits 0 without the marker, so the CTest's pass expression (the marker) fails it: a missing check cannot pass.
// A fatal cannot run in-process (CRD_FATAL breaks into the debugger or traps), so each mode is its own child process.

#include <crd/core/assert.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace crd_diag_fatal
{
inline const char* g_mode = nullptr;     // the mode this run is checking (argv[1])
inline const char* g_expected = nullptr; // the CRD_FATAL message it must reach

inline int handler(const char* formatted_message)
{
    if (g_expected != nullptr && formatted_message != nullptr && std::strstr(formatted_message, g_expected) != nullptr)
    {
        std::printf("CRD_DIAG_FATAL_OK %s\n", g_mode != nullptr ? g_mode : "?");
        std::fflush(stdout);
        std::_Exit(42);
    }
    std::printf("CRD_DIAG_FATAL_OTHER %s\n", formatted_message != nullptr ? formatted_message : "(null)");
    std::fflush(stdout);
    std::_Exit(97);
}

// Installs the handler and records what this mode must reach.
inline void arm(const char* mode, const char* expected_message) noexcept
{
    g_mode = mode;
    g_expected = expected_message;
    crd::set_assert_platform_handler(&handler);
}
} // namespace crd_diag_fatal
