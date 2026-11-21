#pragma once

// ceridc_shell.hpp — how ceridc's tests run the real binary: one UTF-8 command line through the platform's command
// processor, which opens the redirection files the tests name in the working directory.

#include <crd/containers/array.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/core/utf16.hpp>

#include <cstdlib>
#include <cstring>

namespace crd::ceridc::test
{

// The command processor's exit status for `command`, a line the tests build from the binary CMake names and fixed
// text. sh takes its bytes, cmd.exe its UTF-16 form (std::system would hand it over in the ANSI code page, so a
// Turkish name would reach ceridc as other characters).
[[nodiscard]] inline int run_shell(const char* command)
{
#if CRD_OS_WINDOWS
    const usize bytes = std::strlen(command);
    containers::Array<wchar_t> wide;
    wide.resize(bytes + 1U); // UTF-16 takes at most a unit per UTF-8 byte; the last stays the NUL
    (void)utf8_to_utf16(reinterpret_cast<const u8*>(command), bytes, wide.data(), bytes);
    return _wsystem(wide.data()); // NOLINT(bugprone-command-processor): the tests drive the binary with redirection
#else
    return std::system(command); // NOLINT(bugprone-command-processor): the tests drive the binary with redirection
#endif
}

} // namespace crd::ceridc::test
