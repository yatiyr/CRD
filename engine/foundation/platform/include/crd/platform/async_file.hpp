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

    // Submit an async read of `dst.size()` bytes at `offset`.
    // Returns a Counter* the caller must wait() on; returns nullptr on error.
    [[nodiscard]] crd::jobs::Counter* read_async(crd::u64                        offset,
                                                  crd::containers::Span<crd::u8>  dst) noexcept;

private:
    crd::containers::String m_path;   // uses default_allocator()
    crd::u64                m_size = 0;
};

} // namespace crd::platform
