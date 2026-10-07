#pragma once

// crd-ceir-cook (private) -- the bounded program-file read the diagnostic commands share: the file's size is checked
// against the host's limit before a byte is read, and the bytes read are counted for the host's evidence.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/diag_commands.hpp>

#include <atomic>

namespace crd::ceir::cook::detail
{
// Read the file at `path` into `out`. Returns Ok, Oversized (the file is larger than `max_bytes`; nothing is read) or
// Failed (cannot open, size or fully read it), with `reason` set for every status but Ok; `what` names the file in
// the reason. `bytes_read` gains every byte read.
[[nodiscard]] perf::DiagStatus read_bounded_file(containers::StringView path, crd::u64 max_bytes,
                                                 containers::Array<crd::u8>& out, containers::String& reason,
                                                 std::atomic<crd::u64>& bytes_read,
                                                 containers::StringView what = containers::StringView{"the program"});

// Whether a file exists (can be opened for reading) at `path`.
[[nodiscard]] bool file_exists(containers::StringView path);

// Create the file at `path` and write `bytes` to it. The create is exclusive: an existing file is refused and left
// untouched. Returns Ok, or Failed with `reason` set (the file exists, cannot be created or was not fully written; a
// partial file is removed).
[[nodiscard]] perf::DiagStatus write_new_file(containers::StringView path, containers::ConstSpan<crd::u8> bytes,
                                              containers::String& reason);

void append_decimal(containers::String& out, crd::u64 v);
} // namespace crd::ceir::cook::detail
