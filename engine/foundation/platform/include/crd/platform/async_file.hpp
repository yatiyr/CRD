#pragma once

#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

// Forward-declare Counter to avoid pulling all of jobs.hpp into this header.
namespace crd::jobs
{
namespace detail { struct Counter; }
using Counter = detail::Counter;
} // namespace crd::jobs

namespace crd::platform
{

// How one read_async completion ended. The job writes it before the returned counter reaches 0, so it is final once
// wait() on that counter returns. Only Complete means `dst` holds the requested bytes.
enum class AsyncReadStatus : crd::u8
{
    Pending   = 0U, // submitted; not yet complete
    Complete  = 1U, // every requested byte was read
    Short     = 2U, // the read stopped early: the bytes it delivered are in place, the rest of dst is zeroed
    Failed    = 3U, // the file could not be opened or sought, or the read was refused; dst is zeroed
    Cancelled = 4U, // the completion was cancelled (an injected fault); dst is untouched
};

// "pending", "complete", "short", "failed", "cancelled".
[[nodiscard]] crd::containers::StringView async_read_status_name(AsyncReadStatus s) noexcept;

// Job-pool-based async file reader.
//
// Each read_async() call submits a small job to crd-jobs that performs a
// synchronous read at the requested offset and decrements the returned Counter
// on completion. Callers suspend a fiber (or spin on thread 0) via
// crd::jobs::wait(counter) until the read is done.
//
// Constraints:
//   - `dst` in read_async() must remain valid until the returned counter reaches 0.
//   - The AsyncFile must remain valid (not moved or destroyed) until all in-flight
//     read jobs have completed (i.e., wait() has been called on their counters).
//   - Callers must call crd::jobs::wait() on every counter returned; failing to do
//     so leaks a counter slot from the job system pool.
//   - `status` must remain valid until the counter reaches 0, and the caller must read it: a short or failed read
//     is reported there (DIAG.9b), never as silently zeroed bytes. One read is at most 4 GiB - 1 bytes.
//   - Each completion consults the I/O fault seam (crd/core/fault_seam.hpp) as an AsyncRead, so a diagnostics host
//     can fail, shorten, delay or cancel it.
//
// v1d: thread-pool-based implementation (jobs doing synchronous reads).
//      True kernel async I/O (IOCP / io_uring) is deferred.
class AsyncFile
{
public:
    AsyncFile()  noexcept = default;
    ~AsyncFile() noexcept = default;

    AsyncFile(const AsyncFile&)            = delete;
    AsyncFile& operator=(const AsyncFile&) = delete;

    AsyncFile(AsyncFile&& o) noexcept;
    AsyncFile& operator=(AsyncFile&& o) noexcept;

    // Open a file for reading. Returns an invalid AsyncFile (is_open() == false) on error.
    // Uses default_allocator() for the internal path copy.
    [[nodiscard]] static AsyncFile open(crd::containers::StringView path) noexcept;

    [[nodiscard]] bool     is_open() const noexcept;
    [[nodiscard]] crd::u64 size()    const noexcept { return m_size; }

    // Submit an async read of `dst.size()` bytes at `offset`; `status` is Pending until the read completes.
    // Returns a Counter* the caller must wait() on; returns nullptr (status Failed) on error: a closed file, an empty
    // or oversized dst, or a range past the end of the file.
    [[nodiscard]] crd::jobs::Counter* read_async(crd::u64 offset, crd::containers::Span<crd::u8> dst,
                                                 AsyncReadStatus& status) noexcept;

private:
    crd::containers::String m_path;   // uses default_allocator()
    crd::u64                m_size = 0;
};

} // namespace crd::platform
