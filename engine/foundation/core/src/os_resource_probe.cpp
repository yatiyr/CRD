#include <crd/core/os_resource_probe.hpp>

#include <crd/core/platform.hpp>

#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#elif CRD_OS_LINUX
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#endif

namespace crd
{

const char* os_presence_name(OsPresence p) noexcept
{
    switch (p) // no default: every presence is named
    {
    case OsPresence::Present:     return "present";
    case OsPresence::Absent:      return "absent";
    case OsPresence::Partial:     return "partial";
    case OsPresence::Unavailable: return "unavailable";
    }
    return "unknown";
}

namespace
{
OsProbeResult unavailable(const char* reason) noexcept
{
    return OsProbeResult{OsPresence::Unavailable, reason};
}

// The verdict for `covered` reserved or mapped bytes of a `bytes`-long range.
OsProbeResult coverage(crd::usize covered, crd::usize bytes) noexcept
{
    if (covered == 0U)
    {
        return OsProbeResult{OsPresence::Absent, ""};
    }
    if (covered >= bytes)
    {
        return OsProbeResult{OsPresence::Present, ""};
    }
    return OsProbeResult{OsPresence::Partial, ""};
}

// Bytes of [lo, hi) inside [a, b).
crd::usize overlap(crd::usize lo, crd::usize hi, crd::usize a, crd::usize b) noexcept
{
    const crd::usize from = lo > a ? lo : a;
    const crd::usize to   = hi < b ? hi : b;
    return to > from ? to - from : 0U;
}

#if CRD_OS_WINDOWS

// ProcessHandleSnapshotInformation (Windows 8 and later): the calling process's own handle table. The layout is the
// one ntdll has returned since its introduction; it is not in the SDK headers.
constexpr ULONG kProcessHandleSnapshotInformation = 51U;
constexpr LONG  kStatusInfoLengthMismatch         = static_cast<LONG>(0xC0000004UL);

struct HandleEntry
{
    HANDLE      handle_value;
    ULONG_PTR   handle_count;
    ULONG_PTR   pointer_count;
    ACCESS_MASK granted_access;
    ULONG       object_type_index;
    ULONG       handle_attributes;
    ULONG       reserved;
};

struct HandleSnapshotHeader
{
    ULONG_PTR number_of_handles;
    ULONG_PTR reserved;
};

constexpr DWORD kPathChars = 2048U;

// Pages from the OS that hold the handle table; never the heap.
class Pages
{
public:
    Pages() noexcept = default;
    ~Pages()
    {
        reset();
    }
    Pages(const Pages&)            = delete;
    Pages& operator=(const Pages&) = delete;

    bool allocate(crd::usize bytes) noexcept
    {
        reset();
        m_base = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        m_size = m_base != nullptr ? bytes : 0U;
        return m_base != nullptr;
    }
    void reset() noexcept
    {
        if (m_base != nullptr)
        {
            (void)::VirtualFree(m_base, 0, MEM_RELEASE);
        }
        m_base = nullptr;
        m_size = 0U;
    }
    [[nodiscard]] void*      base() const noexcept { return m_base; }
    [[nodiscard]] crd::usize size() const noexcept { return m_size; }

private:
    void*      m_base = nullptr;
    crd::usize m_size = 0U;
};

// The normalized NT path of the file `h` refers to; false when it has none or it does not fit.
bool final_path(HANDLE h, wchar_t* out, DWORD capacity, DWORD& length) noexcept
{
    length = ::GetFinalPathNameByHandleW(h, out, capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    return length > 0U && length < capacity;
}

// Read this process's handle table into `pages`; false when the OS would not give it.
bool snapshot_handles(Pages& pages) noexcept
{
    crd::usize size = 64U * 1024U;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (!pages.allocate(size))
        {
            return false;
        }
        ULONG      needed = 0U;
        const LONG status = ::NtQueryInformationProcess(::GetCurrentProcess(),
                                                        static_cast<PROCESSINFOCLASS>(kProcessHandleSnapshotInformation),
                                                        pages.base(), static_cast<ULONG>(pages.size()), &needed);
        if (status >= 0)
        {
            return true;
        }
        if (status != kStatusInfoLengthMismatch)
        {
            return false;
        }
        const crd::usize grown = static_cast<crd::usize>(needed) + 64U * 1024U;
        size                   = grown > size * 2U ? grown : size * 2U;
    }
    return false;
}

#elif CRD_OS_LINUX

// Parse a lowercase hexadecimal number at `text[i]`, advancing `i`; false when there is none.
bool parse_hex(const char* text, crd::usize length, crd::usize& i, crd::usize& value) noexcept
{
    const crd::usize start = i;
    value                  = 0U;
    while (i < length)
    {
        const char c     = text[i];
        crd::usize digit = 0U;
        if (c >= '0' && c <= '9')
        {
            digit = static_cast<crd::usize>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = static_cast<crd::usize>(c - 'a') + 10U;
        }
        else
        {
            break;
        }
        value = value * 16U + digit;
        ++i;
    }
    return i > start;
}

#endif
} // namespace

OsProbeResult os_file_open_in_process(const char* path) noexcept
{
    if (path == nullptr || path[0] == '\0')
    {
        return unavailable("no path");
    }
#if CRD_OS_WINDOWS
    wchar_t   wide[kPathChars];
    const int converted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, kPathChars);
    if (converted <= 0)
    {
        return unavailable("the path is not valid UTF-8 or is too long");
    }
    // Attribute-only access takes part in no sharing check, so the probe opens a file whatever its holders allow.
    const HANDLE probe = ::CreateFileW(wide, FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (probe == INVALID_HANDLE_VALUE)
    {
        return unavailable("cannot open the path to identify it");
    }
    wchar_t target[kPathChars];
    DWORD   target_length = 0U;
    Pages   pages;
    if (!final_path(probe, target, kPathChars, target_length) || !snapshot_handles(pages))
    {
        (void)::CloseHandle(probe);
        return unavailable("cannot read the process handle table");
    }
    const auto*      header  = static_cast<const HandleSnapshotHeader*>(pages.base());
    const auto*      entries = reinterpret_cast<const HandleEntry*>(header + 1);
    const crd::usize room    = (pages.size() - sizeof(HandleSnapshotHeader)) / sizeof(HandleEntry);
    const crd::usize count   = header->number_of_handles < room ? header->number_of_handles : room;
    // Only handles of the probe's own object type (File) are examined, and only disk files are asked their name.
    ULONG file_type = 0U;
    bool  typed     = false;
    for (crd::usize i = 0U; i < count; ++i)
    {
        if (entries[i].handle_value == probe)
        {
            file_type = entries[i].object_type_index;
            typed     = true;
            break;
        }
    }
    bool held = false;
    for (crd::usize i = 0U; typed && !held && i < count; ++i)
    {
        const HandleEntry& e = entries[i];
        if (e.handle_value == probe || e.object_type_index != file_type)
        {
            continue;
        }
        if (::GetFileType(e.handle_value) != FILE_TYPE_DISK)
        {
            continue;
        }
        wchar_t name[kPathChars];
        DWORD   name_length = 0U;
        if (!final_path(e.handle_value, name, kPathChars, name_length) || name_length != target_length)
        {
            continue;
        }
        held = ::CompareStringOrdinal(name, static_cast<int>(name_length), target, static_cast<int>(target_length),
                                      TRUE) == CSTR_EQUAL;
    }
    (void)::CloseHandle(probe);
    if (!typed)
    {
        return unavailable("the probe's own handle is missing from the handle table");
    }
    return OsProbeResult{held ? OsPresence::Present : OsPresence::Absent, ""};
#elif CRD_OS_LINUX
    struct stat target{};
    if (::stat(path, &target) != 0)
    {
        return unavailable("cannot stat the path to identify it");
    }
    DIR* const dir = ::opendir("/proc/self/fd");
    if (dir == nullptr)
    {
        return unavailable("cannot read /proc/self/fd");
    }
    const int own  = ::dirfd(dir);
    bool      held = false;
    for (const struct dirent* e = ::readdir(dir); e != nullptr && !held; e = ::readdir(dir))
    {
        int         fd    = 0;
        bool        digit = e->d_name[0] != '\0';
        const char* c     = e->d_name;
        for (; *c != '\0'; ++c)
        {
            if (*c < '0' || *c > '9')
            {
                digit = false;
                break;
            }
            fd = fd * 10 + (*c - '0');
        }
        if (!digit || fd == own)
        {
            continue;
        }
        struct stat st{};
        held = ::fstat(fd, &st) == 0 && st.st_dev == target.st_dev && st.st_ino == target.st_ino;
    }
    (void)::closedir(dir);
    return OsProbeResult{held ? OsPresence::Present : OsPresence::Absent, ""};
#else
    return unavailable("no file-handle probe on this OS");
#endif
}

OsProbeResult os_range_mapped(const void* base, crd::usize bytes) noexcept
{
    if (base == nullptr || bytes == 0U)
    {
        return unavailable("an empty range");
    }
    const auto lo = reinterpret_cast<crd::usize>(base);
    if (bytes > ~crd::usize{0} - lo)
    {
        return unavailable("the range wraps the address space");
    }
    const crd::usize hi = lo + bytes;
#if CRD_OS_WINDOWS
    crd::usize     covered = 0U;
    const crd::u8* cursor  = static_cast<const crd::u8*>(base);
    while (reinterpret_cast<crd::usize>(cursor) < hi)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(cursor, &info, sizeof(info)) == 0U)
        {
            break; // past the user address space: nothing more is mapped
        }
        const auto* const next   = static_cast<const crd::u8*>(info.BaseAddress) + info.RegionSize;
        const auto        region = reinterpret_cast<crd::usize>(info.BaseAddress);
        const auto        end    = reinterpret_cast<crd::usize>(next);
        if (info.State != MEM_FREE)
        {
            covered += overlap(lo, hi, region, end);
        }
        if (end <= reinterpret_cast<crd::usize>(cursor))
        {
            break;
        }
        cursor = next;
    }
    return coverage(covered, bytes);
#elif CRD_OS_LINUX
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return unavailable("cannot read /proc/self/maps");
    }
    // Each line starts "start-end " in hex; the rest of the line is skipped, across reads when it is long.
    char       buffer[4096];
    char       head[64];
    crd::usize head_length = 0U;
    bool       in_head     = true;
    crd::usize covered     = 0U;
    bool       failed      = false;
    while (!failed)
    {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got < 0)
        {
            failed = true;
            break;
        }
        if (got == 0)
        {
            break;
        }
        for (ssize_t k = 0; k < got; ++k)
        {
            const char c = buffer[k];
            if (c == '\n')
            {
                in_head     = true;
                head_length = 0U;
                continue;
            }
            if (!in_head)
            {
                continue;
            }
            if (c != ' ')
            {
                if (head_length + 1U < sizeof(head))
                {
                    head[head_length] = c;
                    ++head_length;
                }
                continue;
            }
            in_head          = false;
            crd::usize i     = 0U;
            crd::usize start = 0U;
            crd::usize end   = 0U;
            if (!parse_hex(head, head_length, i, start) || i >= head_length || head[i] != '-')
            {
                failed = true;
                break;
            }
            ++i;
            if (!parse_hex(head, head_length, i, end) || i != head_length)
            {
                failed = true;
                break;
            }
            covered += overlap(lo, hi, start, end);
        }
    }
    (void)::close(fd);
    if (failed)
    {
        return unavailable("cannot parse /proc/self/maps");
    }
    return coverage(covered, bytes);
#else
    return unavailable("no address-space probe on this OS");
#endif
}

namespace
{
#if CRD_OS_LINUX
// What /proc/self/task/<tid>/stat says about a task: whether it exists, its start time and whether it has ended.
struct TaskStat
{
    bool     readable = false; // the file was opened and parsed
    bool     missing  = false; // no such task in this process
    bool     ended    = false; // a zombie or dead task
    crd::u64 start    = 0U;
};

TaskStat read_task_stat(crd::u64 tid) noexcept
{
    TaskStat out;
    char     path[64];
    (void)std::snprintf(path, sizeof(path), "/proc/self/task/%llu/stat", static_cast<unsigned long long>(tid));
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        out.missing = errno == ENOENT || errno == ESRCH;
        return out;
    }
    char       text[1024];
    crd::usize length = 0U;
    while (length + 1U < sizeof(text))
    {
        const ssize_t got = ::read(fd, text + length, sizeof(text) - 1U - length);
        if (got <= 0)
        {
            // A task reaped after open() reads as ESRCH: it is gone, not unreadable.
            out.missing = got < 0 && errno == ESRCH;
            break;
        }
        length += static_cast<crd::usize>(got);
    }
    (void)::close(fd);
    // "tid (comm) state ppid ...": the name may hold spaces and parentheses, so the fields start after the last ')'.
    crd::usize close_at = length;
    for (crd::usize i = 0U; i < length; ++i)
    {
        if (text[i] == ')')
        {
            close_at = i;
        }
    }
    if (close_at == length || close_at + 2U >= length)
    {
        return out;
    }
    const char state = text[close_at + 2U];
    // The state is field 3, the kernel flags word field 9 and starttime field 22, each after one space.
    crd::usize field    = 3U;
    crd::usize i        = close_at + 2U;
    crd::usize flags_at = length;
    while (i < length && field < 22U)
    {
        if (text[i] == ' ')
        {
            ++field;
            if (field == 9U)
            {
                flags_at = i + 1U;
            }
        }
        ++i;
    }
    if (field != 22U || i >= length || text[i] < '0' || text[i] > '9')
    {
        return out;
    }
    const auto number_at = [&](crd::usize at) noexcept
    {
        crd::u64 value = 0U;
        while (at < length && text[at] >= '0' && text[at] <= '9')
        {
            value = (value * 10U) + static_cast<crd::u64>(text[at] - '0');
            ++at;
        }
        return value;
    };
    // PF_EXITING (include/linux/sched.h) is set when the thread enters do_exit, before the kernel clears its
    // CLONE_CHILD_CLEARTID word and so before pthread_join returns; the task stays listed and running ('R') until
    // its exit is complete. A task with the flag set has ended its code even while it still reads as running.
    constexpr crd::u64 pf_exiting = 0x00000004U;
    const bool         exiting    = flags_at < length && (number_at(flags_at) & pf_exiting) != 0U;
    out.ended                     = state == 'Z' || state == 'X' || state == 'x' || exiting;
    out.start                     = number_at(i);
    out.readable                  = true;
    return out;
}
#endif
} // namespace

OsThreadIdentity os_current_thread_identity() noexcept
{
    OsThreadIdentity out;
#if CRD_OS_WINDOWS
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetThreadTimes(::GetCurrentThread(), &created, &exited, &kernel, &user) == 0)
    {
        return out;
    }
    out.id    = ::GetCurrentThreadId();
    out.start = (static_cast<crd::u64>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
    out.known = true;
#elif CRD_OS_LINUX
    const auto     tid  = static_cast<crd::u64>(::syscall(SYS_gettid));
    const TaskStat stat = read_task_stat(tid);
    if (!stat.readable)
    {
        return out;
    }
    out.id    = tid;
    out.start = stat.start;
    out.known = true;
#endif
    return out;
}

OsProbeResult os_thread_alive(const OsThreadIdentity& t) noexcept
{
    if (!t.known)
    {
        return unavailable("the thread's identity was not read");
    }
#if CRD_OS_WINDOWS
    if (t.id > 0xFFFFFFFFULL)
    {
        return unavailable("not a Windows thread id");
    }
    HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(t.id));
    if (h == nullptr)
    {
        if (::GetLastError() == ERROR_INVALID_PARAMETER)
        {
            return OsProbeResult{OsPresence::Absent, ""}; // no thread has this id now
        }
        return unavailable("OpenThread refused the thread id");
    }
    OsProbeResult result{OsPresence::Present, ""};
    FILETIME      created{};
    FILETIME      exited{};
    FILETIME      kernel{};
    FILETIME      user{};
    const bool    ours = ::GetProcessIdOfThread(h) == ::GetCurrentProcessId();
    if (ours && ::GetThreadTimes(h, &created, &exited, &kernel, &user) == 0)
    {
        result = unavailable("GetThreadTimes failed");
    }
    else if (!ours || ((static_cast<crd::u64>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) != t.start ||
             ::WaitForSingleObject(h, 0U) == WAIT_OBJECT_0)
    {
        // The id now names a thread of another process or a later thread of this one, or the thread has exited and
        // something still holds its handle.
        result = OsProbeResult{OsPresence::Absent, ""};
    }
    (void)::CloseHandle(h);
    return result;
#elif CRD_OS_LINUX
    const TaskStat stat = read_task_stat(t.id);
    if (stat.missing)
    {
        return OsProbeResult{OsPresence::Absent, ""};
    }
    if (!stat.readable)
    {
        return unavailable("cannot read /proc/self/task/<tid>/stat");
    }
    if (stat.start != t.start || stat.ended)
    {
        return OsProbeResult{OsPresence::Absent, ""};
    }
    return OsProbeResult{OsPresence::Present, ""};
#else
    return unavailable("no thread probe on this OS");
#endif
}

} // namespace crd
