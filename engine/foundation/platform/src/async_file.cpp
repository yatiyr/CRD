#include <crd/platform/async_file.hpp>

#include "filesystem_detail.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstring>
#include <limits>
#include <type_traits>

namespace crd::platform
{

namespace
{

// Trivially-copyable, trivially-destructible closure for the async read job.
// Captures a pointer into the owning AsyncFile's m_path (valid while the AsyncFile
// lives — documented constraint in async_file.hpp) plus the read parameters and the
// caller's status slot. The two sizes are 32-bit so the closure stays within the SBO:
// sizeof == 40 bytes (8 + 4 + 4 + 8 + 8 + 8) ≤ 41-byte SBO limit.
struct ReadJob
{
    const char*      path_data; // ptr into AsyncFile::m_path
    crd::u32         path_size;
    crd::u32         dst_size;
    crd::u64         offset;
    crd::u8*         dst_ptr;
    AsyncReadStatus* status;

    void operator()() const noexcept
    {
        // DIAG.9b fault seam: the completion may be failed, cut short, delayed or cancelled.
        const IoFault fault = consult_io_fault(IoFaultOp::AsyncRead, path_data, path_size, offset, dst_size);
        crd::u64      want  = dst_size;
        switch (fault.action) // no default: every action is handled
        {
        case IoFaultAction::Proceed:
            break;
        case IoFaultAction::Fail:
            std::memset(dst_ptr, 0, dst_size);
            *status = AsyncReadStatus::Failed;
            return;
        case IoFaultAction::Cancel:
            *status = AsyncReadStatus::Cancelled;
            return;
        case IoFaultAction::Delay:
            fs::detail::sleep_for_injected_delay(fault.value);
            break;
        case IoFaultAction::Short:
            want = fault.value < static_cast<crd::u64>(dst_size) ? fault.value : static_cast<crd::u64>(dst_size) - 1U;
            break;
        }

        const fs::Path              p(crd::containers::StringView{path_data, path_size});
        const fs::detail::RangeRead r = fs::detail::read_range_into(p, offset, dst_ptr, want);
        if (!r.opened)
        {
            std::memset(dst_ptr, 0, dst_size);
            *status = AsyncReadStatus::Failed;
            return;
        }
        if (r.delivered < static_cast<crd::u64>(dst_size))
        {
            std::memset(dst_ptr + r.delivered, 0, static_cast<crd::usize>(dst_size - r.delivered));
            *status = AsyncReadStatus::Short;
            return;
        }
        *status = AsyncReadStatus::Complete;
    }
};

static_assert(sizeof(ReadJob)  <= 41U, "ReadJob must fit in 41-byte SBO");
static_assert(alignof(ReadJob) <= 8U,  "ReadJob alignment must be ≤ 8");
static_assert(std::is_trivially_copyable_v<ReadJob>);
static_assert(std::is_trivially_destructible_v<ReadJob>);

} // anonymous namespace

// ── AsyncFile ─────────────────────────────────────────────────────────────────

AsyncFile::AsyncFile(AsyncFile&& o) noexcept
    : m_path(std::move(o.m_path))
    , m_size(o.m_size)
{
    o.m_size = 0;
}

AsyncFile& AsyncFile::operator=(AsyncFile&& o) noexcept
{
    if (this != &o)
    {
        m_path   = std::move(o.m_path);
        m_size   = o.m_size;
        o.m_size = 0;
    }
    return *this;
}

AsyncFile AsyncFile::open(crd::containers::StringView path) noexcept
{
    const fs::Path p(path);
    if (!fs::is_file(p))
    {
        return AsyncFile{};
    }
    AsyncFile f;
    f.m_path = crd::containers::String(path.data(), path.size(), crd::memory::default_allocator());
    f.m_size = fs::file_size(p);
    return f;
}

bool AsyncFile::is_open() const noexcept
{
    return !m_path.empty();
}

crd::containers::StringView async_read_status_name(AsyncReadStatus s) noexcept
{
    switch (s) // no default: every status is named
    {
    case AsyncReadStatus::Pending:   return crd::containers::StringView{"pending"};
    case AsyncReadStatus::Complete:  return crd::containers::StringView{"complete"};
    case AsyncReadStatus::Short:     return crd::containers::StringView{"short"};
    case AsyncReadStatus::Failed:    return crd::containers::StringView{"failed"};
    case AsyncReadStatus::Cancelled: return crd::containers::StringView{"cancelled"};
    }
    return crd::containers::StringView{"unknown"};
}

crd::jobs::Counter* AsyncFile::read_async(crd::u64 offset, crd::containers::Span<crd::u8> dst,
                                          AsyncReadStatus& status) noexcept
{
    constexpr crd::usize max_bytes = std::numeric_limits<crd::u32>::max();
    status                         = AsyncReadStatus::Failed;
    if (!is_open() || dst.empty() || dst.size() > max_bytes || m_path.size() > max_bytes)
    {
        return nullptr;
    }
    if (offset + static_cast<crd::u64>(dst.size()) > m_size)
    {
        return nullptr;
    }

    status = AsyncReadStatus::Pending;
    const ReadJob job_data{
        m_path.data(),
        static_cast<crd::u32>(m_path.size()),
        static_cast<crd::u32>(dst.size()),
        offset,
        dst.data(),
        &status
    };

    return crd::jobs::run(crd::jobs::make_job(job_data));
}

} // namespace crd::platform
