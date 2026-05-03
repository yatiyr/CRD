#pragma once

namespace crd::cooker
{

// Run the cook sub-command. Scans `root` recursively, cooks each asset into
// the incremental cache, assembles a PACK at `out_path`, and writes
// cook.log.toml adjacent to the PACK file.
// Returns 0 on success, non-zero on fatal error.
[[nodiscard]] int cmd_cook(const char* root, const char* out_path);

} // namespace crd::cooker
