#include <crd/log/log.hpp>
#include <crd/vm/log_channel.hpp>
#include <crd/vm/virtual_memory.hpp>

#include <cstdint>

#if CRD_OS_WINDOWS
#include <windows.h>
#else // POSIX (Linux / macOS / other)
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace crd::vm
{
namespace
{
// Round helpers (alignment is always a power of two).
[[nodiscard]] inline crd::usize round_up(crd::usize v, crd::usize align) noexcept
{
    return (v + (align - 1)) & ~(align - 1);
}

// Compute the page-aligned span covering [ptr, ptr+bytes): start rounded DOWN,
// end rounded UP. Returns the aligned base + length. align must be a power of 2.
struct AlignedSpan
{
    void*      base;
    crd::usize len;
};
[[nodiscard]] inline AlignedSpan page_span(void* ptr, crd::usize bytes, crd::usize page) noexcept
{
    const auto           raw  = reinterpret_cast<std::uintptr_t>(ptr);
    const std::uintptr_t lead = raw & (static_cast<std::uintptr_t>(page) - 1); // bytes into the first page
    // Pointer arithmetic to find the page base (not an int->ptr cast, which the
    // optimizer pessimizes / clang-tidy flags).
    void*            base = static_cast<unsigned char*>(ptr) - lead;
    const crd::usize len  = round_up(static_cast<crd::usize>(lead) + bytes, page);
    return AlignedSpan{base, len};
}

#if CRD_OS_WINDOWS
struct SysInfo
{
    crd::usize page;
    crd::usize granularity;
    SysInfo() noexcept
    {
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        page        = static_cast<crd::usize>(si.dwPageSize);
        granularity = static_cast<crd::usize>(si.dwAllocationGranularity);
    }
};
[[nodiscard]] const SysInfo& sys_info() noexcept
{
    static const SysInfo kInfo; // thread-safe one-time init
    return kInfo;
}
[[nodiscard]] DWORD to_win_protect(Access a) noexcept
{
    switch (a)
    {
    case Access::None:      return PAGE_NOACCESS;
    case Access::ReadOnly:  return PAGE_READONLY;
    case Access::ReadWrite: return PAGE_READWRITE;
    }
    return PAGE_NOACCESS;
}
#else
[[nodiscard]] crd::usize posix_page_size() noexcept
{
    static const crd::usize kPage = []() noexcept -> crd::usize {
        const long p = ::sysconf(_SC_PAGESIZE);
        return p > 0 ? static_cast<crd::usize>(p) : crd::usize{4096};
    }();
    return kPage;
}
[[nodiscard]] int to_posix_protect(Access a) noexcept
{
    switch (a)
    {
    case Access::None:      return PROT_NONE;
    case Access::ReadOnly:  return PROT_READ;
    case Access::ReadWrite: return PROT_READ | PROT_WRITE;
    }
    return PROT_NONE;
}
#endif
} // namespace

crd::usize page_size() noexcept
{
#if CRD_OS_WINDOWS
    return sys_info().page;
#else
    return posix_page_size();
#endif
}

crd::usize allocation_granularity() noexcept
{
#if CRD_OS_WINDOWS
    return sys_info().granularity;
#else
    return posix_page_size(); // mmap reserves at page granularity
#endif
}

crd::usize large_page_size() noexcept
{
#if CRD_OS_WINDOWS
    return static_cast<crd::usize>(::GetLargePageMinimum()); // 0 if large pages unsupported
#else
    // Portable large-page (hugetlb) reservation needs explicit MAP_HUGETLB / a
    // hugetlbfs mount; not exposed by this minimal API yet. Report "unavailable".
    return 0;
#endif
}

VmRegion reserve_at(void* hint, crd::usize bytes) noexcept
{
    if (bytes == 0)
    {
        return {};
    }
    const crd::usize size = round_up(bytes, page_size());
#if CRD_OS_WINDOWS
    void* base = ::VirtualAlloc(hint, size, MEM_RESERVE, PAGE_NOACCESS);
    if (base == nullptr)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::reserve({} bytes) failed: VirtualAlloc err={}", size,
                      static_cast<unsigned long>(::GetLastError()));
        return {};
    }
#else
    void* base = ::mmap(hint, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::reserve({} bytes) failed: mmap errno={}", size, errno);
        return {};
    }
#endif
    return VmRegion{base, size};
}

VmRegion reserve(crd::usize bytes) noexcept
{
    return reserve_at(nullptr, bytes);
}

void release(VmRegion& region) noexcept
{
    if (!region.valid())
    {
        region = {};
        return;
    }
#if CRD_OS_WINDOWS
    // MEM_RELEASE requires size == 0 and base == the original reservation base.
    if (::VirtualFree(region.base, 0, MEM_RELEASE) == 0)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::release failed: VirtualFree err={}",
                      static_cast<unsigned long>(::GetLastError()));
    }
#else
    if (::munmap(region.base, region.size) != 0)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::release failed: munmap errno={}", errno);
    }
#endif
    region = {};
}

bool commit(void* ptr, crd::usize bytes) noexcept
{
    if (ptr == nullptr || bytes == 0)
    {
        return false;
    }
    const AlignedSpan s = page_span(ptr, bytes, page_size());
#if CRD_OS_WINDOWS
    if (::VirtualAlloc(s.base, s.len, MEM_COMMIT, PAGE_READWRITE) == nullptr)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::commit({} bytes) failed: VirtualAlloc err={}", s.len,
                      static_cast<unsigned long>(::GetLastError()));
        return false;
    }
#else
    // Linux overcommits: physical pages fault in on first touch. mprotect to RW
    // is the "commit". (A prior decommit's MADV_DONTNEED guarantees zero-fill.)
    if (::mprotect(s.base, s.len, PROT_READ | PROT_WRITE) != 0)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::commit({} bytes) failed: mprotect errno={}", s.len, errno);
        return false;
    }
#endif
    return true;
}

bool decommit(void* ptr, crd::usize bytes) noexcept
{
    if (ptr == nullptr || bytes == 0)
    {
        return false;
    }
    const AlignedSpan s = page_span(ptr, bytes, page_size());
#if CRD_OS_WINDOWS
    if (::VirtualFree(s.base, s.len, MEM_DECOMMIT) == 0)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::decommit({} bytes) failed: VirtualFree err={}", s.len,
                      static_cast<unsigned long>(::GetLastError()));
        return false;
    }
#else
    // Drop physical pages (next access after re-commit reads zero) THEN make the
    // range inaccessible — mirrors Windows MEM_DECOMMIT (reserved but no-access).
    if (::madvise(s.base, s.len, MADV_DONTNEED) != 0)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::decommit({} bytes) failed: madvise errno={}", s.len, errno);
        return false;
    }
    if (::mprotect(s.base, s.len, PROT_NONE) != 0)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::decommit({} bytes) failed: mprotect errno={}", s.len, errno);
        return false;
    }
#endif
    return true;
}

bool protect(void* ptr, crd::usize bytes, Access access) noexcept
{
    if (ptr == nullptr || bytes == 0)
    {
        return false;
    }
    const AlignedSpan s = page_span(ptr, bytes, page_size());
#if CRD_OS_WINDOWS
    DWORD old = 0;
    if (::VirtualProtect(s.base, s.len, to_win_protect(access), &old) == 0)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::protect failed: VirtualProtect err={}",
                      static_cast<unsigned long>(::GetLastError()));
        return false;
    }
#else
    if (::mprotect(s.base, s.len, to_posix_protect(access)) != 0)
    {
        CRD_LOG_ERROR(g_log_vm, "vm::protect failed: mprotect errno={}", errno);
        return false;
    }
#endif
    return true;
}
} // namespace crd::vm
