#pragma once

// The process environment, read into caller storage: no heap, no lock, safe in a CRT initializer before main. Code
// reads environment variables through it (MSVC deprecates std::getenv, and Windows cannot tell an empty variable
// from a missing one, so an empty value reads as unset on every platform).

#include <crd/core/types.hpp>

namespace crd
{

enum class EnvironmentRead : crd::u8
{
    Unset,   // missing or empty; the buffer holds ""
    Set,     // the value and its NUL fill the buffer's front
    TooLong, // the value needs more than capacity - 1 bytes; the buffer holds ""
};

// Copies variable `name` into buffer[0, capacity). capacity 0 reads every set variable as TooLong.
[[nodiscard]] EnvironmentRead read_environment(const char* name, char* buffer, crd::usize capacity) noexcept;

// True when variable `name` holds a value (of any length).
[[nodiscard]] inline bool environment_set(const char* name) noexcept
{
    return read_environment(name, nullptr, 0U) != EnvironmentRead::Unset;
}

// One variable's value in N bytes of its own storage.
template <crd::usize N> class EnvironmentValue
{
public:
    explicit EnvironmentValue(const char* name) noexcept : m_read(read_environment(name, m_value, N)) {}

    [[nodiscard]] EnvironmentRead read() const noexcept { return m_read; }
    // The value, or nullptr unless it is set and fits.
    [[nodiscard]] const char* c_str() const noexcept { return m_read == EnvironmentRead::Set ? m_value : nullptr; }

private:
    char m_value[N] = {}; // NOLINT(modernize-avoid-c-arrays): fixed storage, no heap
    EnvironmentRead m_read;
};

} // namespace crd
