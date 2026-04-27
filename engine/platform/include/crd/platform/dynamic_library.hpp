#pragma once

#include <crd/platform/filesystem.hpp>

namespace crd::platform
{
class DynamicLibrary
{
public:
    DynamicLibrary() noexcept = default;
    [[nodiscard]] static DynamicLibrary open(const fs::Path& path) noexcept;

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    ~DynamicLibrary() noexcept;

    [[nodiscard]] bool is_valid() const noexcept { return m_handle != nullptr; }
    [[nodiscard]] void* resolve(const char* symbol_name) const noexcept;

    template <typename FnPtr> [[nodiscard]] FnPtr resolve_as(const char* symbol_name) const noexcept
    {
        return reinterpret_cast<FnPtr>(resolve(symbol_name));
    }

private:
    explicit DynamicLibrary(void* handle) noexcept : m_handle(handle) {}

    void* m_handle = nullptr;
};
} // namespace crd::platform
