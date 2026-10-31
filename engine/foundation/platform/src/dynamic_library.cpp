#include "wide_text.hpp"

#include <crd/core/lifetime_ledger.hpp>
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

DynamicLibrary DynamicLibrary::open(const fs::Path& path, bool log_on_failure) noexcept
{
#if CRD_OS_WINDOWS
    HMODULE handle = LoadLibraryW(wide_text::utf8_to_wide(path.generic()).data());
    if (handle == nullptr)
    {
        if (log_on_failure)
        {
            CRD_LOG_ERROR(g_log_platform, "LoadLibraryW failed for '{}'", path.generic().data());
        }
        return DynamicLibrary{};
    }
    lifetime_ledger().acquired(LifetimeClass::LibraryHandle);
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
    lifetime_ledger().acquired(LifetimeClass::LibraryHandle);
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
    const bool freed = FreeLibrary(static_cast<HMODULE>(m_handle)) != 0;
#else
    const bool freed = dlclose(m_handle) == 0;
#endif
    if (freed) // a reference the OS kept stays live in the ledger
    {
        lifetime_ledger().released(LifetimeClass::LibraryHandle);
    }
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
