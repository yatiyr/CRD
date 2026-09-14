#include <crd/log/log.hpp>
#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/log_channel.hpp>

#if CRD_OS_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <crd/containers/array.hpp>

namespace crd::platform
{
namespace
{
#if CRD_OS_WINDOWS
[[nodiscard]] crd::containers::Array<wchar_t> utf8_to_wide(containers::StringView sv) noexcept
{
    // crd::Array uses CRD_FATAL on OOM (never throws), so nothing escapes this noexcept.
    // A single L'\0' is the valid-empty default and this function's failure signal.
    crd::containers::Array<wchar_t> out;
    out.push_back(L'\0');
    if (sv.empty())
    {
        return out;
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (needed <= 0)
    {
        return out;
    }
    out.resize(static_cast<crd::usize>(needed) + 1); // content + trailing null (value-initialised)
    const int written = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), needed);
    if (written != needed)
    {
        out.clear();
        out.push_back(L'\0');
    }
    return out;
}
#endif
} // namespace

DynamicLibrary DynamicLibrary::open(const fs::Path& path, bool log_on_failure) noexcept
{
#if CRD_OS_WINDOWS
    HMODULE handle = LoadLibraryW(utf8_to_wide(path.generic()).data());
    if (handle == nullptr)
    {
        if (log_on_failure)
        {
            CRD_LOG_ERROR(g_log_platform, "LoadLibraryW failed for '{}'", path.generic().data());
        }
        return DynamicLibrary{};
    }
    return DynamicLibrary{static_cast<void*>(handle)};
#else
    void* handle = dlopen(path.generic().data(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        if (log_on_failure)
        {
            CRD_LOG_ERROR(g_log_platform, "dlopen failed for '{}': {}", path.generic().data(), dlerror());
        }
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
