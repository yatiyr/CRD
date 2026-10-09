#pragma once

// crd-platform internal: the raw range read AsyncFile's completion job uses. Not part of the public surface.

#include <crd/core/types.hpp>
#include <crd/platform/filesystem.hpp>

namespace crd::platform::fs::detail
{

struct RangeRead
{
    bool     opened    = false; // the file opened and the seek to `offset` succeeded
    crd::u64 delivered = 0U;     // bytes written to dst (fewer than asked when the file ended or the read failed)
};

// Read up to `size` bytes at `offset` straight into `dst`, without consulting the I/O fault seam (the caller already
// did). Bytes past `delivered` are left as they were.
[[nodiscard]] RangeRead read_range_into(const Path& path, crd::u64 offset, crd::u8* dst, crd::u64 size) noexcept;

// Sleep for an injected completion delay.
void sleep_for_injected_delay(crd::u64 ns) noexcept;

} // namespace crd::platform::fs::detail
