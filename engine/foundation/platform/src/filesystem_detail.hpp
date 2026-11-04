#pragma once

// crd-platform internal: the OS spelling of a path, shared by the file and durable-save units. The raw range read
// crd-platform-async's AsyncFile uses is in crd/platform/detail/range_read.hpp. Not part of the public surface.

#include <crd/platform/detail/range_read.hpp>
#include <crd/platform/filesystem.hpp>

#include <filesystem>

namespace crd::platform::fs::detail
{

// The OS spelling of `path` (UTF-16 on Windows). May throw (allocation), as std::filesystem::path does.
[[nodiscard]] std::filesystem::path native_path(const Path& path);

} // namespace crd::platform::fs::detail
