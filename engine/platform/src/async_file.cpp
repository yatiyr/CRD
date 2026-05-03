#include <crd/platform/async_file.hpp>

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstring>
#include <type_traits>

namespace crd::platform
{

namespace
{

// Trivially-copyable, trivially-destructible closure for the async read job.
// Captures a pointer into the owning AsyncFile's m_path (valid while the AsyncFile
// lives — documented constraint in async_file.hpp) plus the read parameters.
//
// sizeof == 40 bytes (five 8-byte fields on 64-bit) ≤ 41-byte SBO limit.
struct ReadJob
{
    const char* path_data; // ptr into AsyncFile::m_path
    crd::usize  path_size;
    crd::u64    offset;
    crd::u8*    dst_ptr;
    crd::usize  dst_size;

    void operator()() noexcept
    {
        crd::memory::IAllocator* const         alloc = crd::memory::default_allocator();
        const crd::containers::StringView      sv{path_data, path_size};
        const fs::Path                         p(sv);
        crd::containers::Array<crd::u8>        tmp(alloc);
        const bool ok = fs::read_file_range(p, offset, dst_size, tmp);
        if (ok && tmp.size() == dst_size)
        {
            std::memcpy(dst_ptr, tmp.data(), dst_size);
        }
        else
        {
            std::memset(dst_ptr, 0, dst_size);
        }
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

crd::jobs::Counter* AsyncFile::read_async(crd::u64                       offset,
                                           crd::containers::Span<crd::u8> dst) noexcept
{
    if (!is_open() || dst.empty())
    {
        return nullptr;
    }
    if (offset + static_cast<crd::u64>(dst.size()) > m_size)
    {
        return nullptr;
    }

    const ReadJob job_data{
        m_path.data(),
        m_path.size(),
        offset,
        dst.data(),
        dst.size()
    };

    return crd::jobs::run(crd::jobs::make_job(job_data));
}

} // namespace crd::platform
