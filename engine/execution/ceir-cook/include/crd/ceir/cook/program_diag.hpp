#pragma once

// crd-ceir-cook -- the program-provenance diagnostic command.
//
// `program.provenance` answers, for one authored program file under the diagnostic host's file root, every op with the
// authored position and nodes it came from: its stable id and name, the closest-known file:line:col, the first CHIR
// node it was lowered from (id and position), its native binding, and the rendered provenance. It is registered into a
// crd-perf DiagCommandService, so a native caller, the CLI verb and the MCP tool get the same bounded, paginated answer
// under the same authority checks (Read).
//
// The command lives in this bridge rather than in crd-ceir, whose link edges stay the host-only substrate (it may not
// link crd-perf). A program is read in the three forms this module already loads: a cooked CRDR program blob, a raw
// CEIR binary and CEIR text. Text is parsed under the request's relative path, so the host's root never appears in an
// answer; the binary forms carry their own authored file names. Each request loads into a fresh Context with the
// host's dialects registered first, so an op the host does not register reports its native binding as unregistered
// (unknown), never as non-intrinsic.
//
// The file's size is checked against the host's limit before a byte is read. A file that fails to load is refused
// Failed with the position (text) or the load error (binary forms) in the reason. Contract:
// docs/design/runtime-diagnostics.md.

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace crd::perf
{
class DiagCommandService;
} // namespace crd::perf

namespace crd::ceir::cook
{
inline constexpr containers::StringView kProgramProvenanceCommand{"program.provenance"};

// The host's configuration of the command, and evidence of the work it did. The host owns it and keeps it alive at
// least as long as every service it is registered with.
struct ProgramProvenanceCommand
{
    Registrar registrar = nullptr; // registers the host's dialects into each fresh Context (null: none)
    void*     user      = nullptr;
    crd::u64  max_program_bytes = 16ULL * 1024ULL * 1024ULL; // a larger file is refused Oversized, unread

    std::atomic<crd::u64> runs{0U};       // handler runs (requests that passed every service check)
    std::atomic<crd::u64> bytes_read{0U}; // program bytes read from files
};

// Register `program.provenance` (Read, takes a path) with `service`. Returns false when the service refuses the
// registration (a duplicate name or a full table).
[[nodiscard]] bool register_program_provenance(perf::DiagCommandService& service, ProgramProvenanceCommand& command);
} // namespace crd::ceir::cook
