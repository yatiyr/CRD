// The process environment reader (crd/core/environment.hpp).

#include <crd/core/environment.hpp>
#include <crd/core/platform.hpp>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdlib>
#include <cstring>
#endif

namespace crd
{

EnvironmentRead read_environment(const char* name, char* buffer, crd::usize capacity) noexcept
{
    if (capacity > 0U)
    {
        buffer[0] = '\0';
    }
#if CRD_OS_WINDOWS
    // The length without the NUL when it fits, the size it needs with the NUL when not, 0 when missing or empty.
    char probe = '\0';
    char* const out = capacity > 0U ? buffer : &probe;
    crd::usize room = capacity > 0U ? capacity : 1U;
    if (room > 0xFFFFFFFFU)
    {
        room = 0xFFFFFFFFU;
    }
    const DWORD n = ::GetEnvironmentVariableA(name, out, static_cast<DWORD>(room));
    if (n == 0U)
    {
        return EnvironmentRead::Unset;
    }
    if (n >= room)
    {
        out[0] = '\0';
        return EnvironmentRead::TooLong;
    }
    return EnvironmentRead::Set;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): engine code never writes the environment
    if (value == nullptr || value[0] == '\0')
    {
        return EnvironmentRead::Unset;
    }
    const crd::usize n = std::strlen(value);
    if (n >= capacity)
    {
        return EnvironmentRead::TooLong;
    }
    std::memcpy(buffer, value, n + 1U);
    return EnvironmentRead::Set;
#endif
}

} // namespace crd
