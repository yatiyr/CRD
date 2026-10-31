// The process environment reader (crd/core/environment.hpp).

#include <crd/core/assert.hpp>
#include <crd/core/environment.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/utf16.hpp>

#include <charconv>
#include <cstring>
#include <system_error>

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
#endif

namespace crd
{
#if CRD_OS_WINDOWS
namespace
{
constexpr usize kWideNameUnits  = 256U; // a name and its NUL
constexpr DWORD kWideValueUnits = 256U; // a value read on the stack and its NUL
} // namespace
#endif

EnvironmentRead read_environment(const char* name, char* buffer, crd::usize capacity) noexcept
{
    if (capacity > 0U)
    {
        buffer[0] = '\0';
    }
#if CRD_OS_WINDOWS
    // Windows keeps the environment in UTF-16 and GetEnvironmentVariableA hands a value over in the ANSI code page, where
    // a character the code page lacks arrives as '?'. The value is read wide and written as UTF-8 instead.
    wchar_t     wide_name[kWideNameUnits] = {}; // NOLINT(modernize-avoid-c-arrays): no heap
    const usize name_units                = utf8_to_utf16(reinterpret_cast<const u8*>(name), std::strlen(name),
                                                          wide_name, kWideNameUnits - 1U);
    if (name_units >= kWideNameUnits)
    {
        return EnvironmentRead::Unset; // no variable the engine reads has such a name
    }
    // A value of at most 255 units is read on the stack; a longer one into pages of its exact size. UTF-8 takes at least
    // a byte per UTF-16 unit, so a value of `capacity` units or more is too long before it is read.
    wchar_t  local[kWideValueUnits] = {}; // NOLINT(modernize-avoid-c-arrays): no heap
    wchar_t* wide                   = local;
    DWORD    room                   = kWideValueUnits;
    void*    pages                  = nullptr;
    EnvironmentRead result          = EnvironmentRead::Unset;
    for (;;)
    {
        // The length without the NUL when it fits, the size it needs with the NUL when not, 0 when missing or empty.
        const DWORD units = ::GetEnvironmentVariableW(wide_name, wide, room);
        if (units == 0U)
        {
            break;
        }
        if (units < room)
        {
            const usize bytes = utf16_to_utf8(wide, units, reinterpret_cast<u8*>(buffer), capacity);
            if (bytes < capacity)
            {
                buffer[bytes] = '\0';
                result        = EnvironmentRead::Set;
            }
            else
            {
                if (capacity > 0U)
                {
                    buffer[0] = '\0';
                }
                result = EnvironmentRead::TooLong;
            }
            break;
        }
        if (static_cast<usize>(units) - 1U >= capacity)
        {
            result = EnvironmentRead::TooLong;
            break;
        }
        // Another thread may lengthen the value between the two reads: the loop reads again at the new size.
        if (pages != nullptr)
        {
            (void)::VirtualFree(pages, 0U, MEM_RELEASE);
        }
        pages = ::VirtualAlloc(nullptr, static_cast<SIZE_T>(units) * sizeof(wchar_t), MEM_COMMIT | MEM_RESERVE,
                               PAGE_READWRITE);
        if (pages == nullptr)
        {
            CRD_FATAL("read_environment: out of memory for an environment value");
        }
        wide = static_cast<wchar_t*>(pages);
        room = units;
    }
    if (pages != nullptr)
    {
        (void)::VirtualFree(pages, 0U, MEM_RELEASE);
    }
    return result;
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

bool read_environment_integer(const char* name, crd::i64 low, crd::i64 high, crd::i64& value) noexcept
{
    char buffer[24] = {}; // NOLINT(modernize-avoid-c-arrays): a 64-bit integer, its sign and its NUL
    if (read_environment(name, buffer, sizeof(buffer)) != EnvironmentRead::Set)
    {
        return false;
    }
    const char* const end = buffer + std::strlen(buffer);
    crd::i64 n = 0;
    const std::from_chars_result parsed = std::from_chars(buffer, end, n);
    if (parsed.ec != std::errc{} || parsed.ptr != end || n < low || n > high)
    {
        return false;
    }
    value = n;
    return true;
}

} // namespace crd
