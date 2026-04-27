#include <crd/log/log.hpp>
#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/log_channel.hpp>

#if CRD_OS_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace crd::platform
{
namespace
{
#if CRD_OS_WINDOWS
[[nodiscard]] std::wstring utf8_to_wide(containers::StringView sv) noexcept
{
    if (sv.empty())
    {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (needed <= 0)
    {
        return {};
    }
    std::wstring out(static_cast<crd::usize>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), needed);
    if (written != needed)
    {
        return {};
    }
    return out;
}
#endif
} // namespace

DynamicLibrary DynamicLibrary::open(const fs::Path& path) noexcept
{
#if CRD_OS_WINDOWS
    HMODULE handle = LoadLibraryW(utf8_to_wide(path.generic()).c_str());
    if (handle == nullptr)
    {
        CRD_LOG_ERROR(g_log_platform, "LoadLibraryW failed for '{}'", path.generic().data());
        return DynamicLibrary{};
    }
    return DynamicLibrary{static_cast<void*>(handle)};
#else
    void* handle = dlopen(path.generic().data(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        CRD_LOG_ERROR(g_log_platform, "dlopen failed for '{}': {}", path.generic().data(), dlerror());
        return DynamicLibrary{};
    }
    return DynamicLibrary{handle};
#endif
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : m_handle(other.m_handle)
{
    other.m_handle = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
    if (this != &other)
    {
        this->~DynamicLibrary();
        m_handle = other.m_handle;
        other.m_handle = nullptr;
    }
    return *this;
}

DynamicLibrary::~DynamicLibrary() noexcept
{
    if (m_handle == nullptr)
    {
        return;
    }
#if CRD_OS_WINDOWS
    FreeLibrary(static_cast<HMODULE>(m_handle));
#else
    dlclose(m_handle);
#endif
    m_handle = nullptr;
}

void* DynamicLibrary::resolve(const char* symbol_name) const noexcept
{
    if (m_handle == nullptr || symbol_name == nullptr)
    {
        return nullptr;
    }
#if CRD_OS_WINDOWS
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), symbol_name));
#else
    return dlsym(m_handle, symbol_name);
#endif
}
} // namespace crd::platform
