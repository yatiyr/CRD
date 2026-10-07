#pragma once

// verbs.hpp — GEO-11 (D-007 row 76): the AGENT SURFACE — every engine operation as a headless verb emitting a
// MACHINE-READABLE JSON report, and `mcp_handle` exposing the SAME verbs over MCP (JSON-RPC 2.0, the stdio
// transport's per-line payload). ⛔ agent edits are TRANSACTIONAL: every verb validates COMPLETELY before its
// first side effect — an invalid request changes NOTHING on disk (and `dry_run` stops a valid one before the
// write). The CLI (main.cpp) and the MCP loop are thin shells over these functions — one implementation, two
// transports (the Blender-MCP lesson: the surface is the product, the socket is plumbing).
//
// Verbs: import · cook · query · instantiate · sequence · render · export_timeline · inspect (CLI-only) · diag.

#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::perf
{
class DiagCommandService;
struct DiagRequest;
} // namespace crd::perf

namespace crd::ceridc
{

// Parse any supported interchange file (.glb/.gltf/.stl/.obj/.ply/.3mf/.otio/.wav/.aiff/.flac/.mid) and
// report what is inside — the agent's eyes before a cook.
[[nodiscard]] crd::containers::String verb_import(const char* path, crd::memory::IAllocator* alloc);

// Run the GEO-6 incremental processor over a source root into a PACK.
[[nodiscard]] crd::containers::String verb_cook(const char* root, const char* out_pack,
                                                crd::memory::IAllocator* alloc);

// List a PACK's manifest — every UUID, type, and debug name (the agent's resource-graph query).
[[nodiscard]] crd::containers::String verb_query(const char* pack_path, crd::memory::IAllocator* alloc);

// Compose a scene: an entity instancing `asset_name` from the pack at `translate`, serialized as a SCEN
// artifact. TRANSACTIONAL: unknown asset / non-finite transform rejects with NO file written; dry_run
// validates and reports without writing.
[[nodiscard]] crd::containers::String verb_instantiate(const char* pack_path, const char* asset_name,
                                                       const crd::f32 translate[3], bool dry_run,
                                                       const char* out_scene, crd::memory::IAllocator* alloc);

// Author a 2-shot timeline (clip_a · centered dissolve · clip_b, 24 fps) and write BOTH the TIML artifact and
// its `.otio` interchange twin.
[[nodiscard]] crd::containers::String verb_sequence(const char* name, const char* clip_a, crd::i64 frames_a,
                                                    const char* clip_b, crd::i64 frames_b,
                                                    crd::i64 transition_frames, const char* out_timl,
                                                    const char* out_otio, crd::memory::IAllocator* alloc);

// Render a timeline (.otio) to an EXR sequence in `out_dir` (fNNNN.exr — the GEO-9 driver; clips resolve to
// deterministic solid takes until the renderer band binds real scene renders through the same seam).
[[nodiscard]] crd::containers::String verb_render(const char* otio_path, const char* out_dir,
                                                  crd::i64 max_frames, crd::memory::IAllocator* alloc);

// Convert a TIML artifact back to `.otio` (the interchange edge, resource → NLE).
[[nodiscard]] crd::containers::String verb_export_timeline(const char* timl_path, const char* out_otio,
                                                           crd::memory::IAllocator* alloc);

// DIAG.8b: run the authored CEIR text program at `program_path` (cooked under that name) from `entry` (default "main")
// with `args`, under a runtime inspection session on its own executing thread. At every stop (a `breaks` line, or a
// step) the report records the stop's authored line, column and call depth and each `watches` line's typed value
// (status, type text, unit flag, value when available), then applies the next of `actions` ("continue", "into",
// "over", "out", "cancel"; "continue" once they run out). At most `max_stops` stops are reported (0 = 64); past that
// the run is cancelled and the report says `truncated`. `ok` is true when the run finished or was cancelled by the
// script. CLI-only: typed authority over runtime inspection for agent transports is DIAG.8c's, so it is not a tool
// in `tools/list` yet.
[[nodiscard]] crd::containers::String verb_inspect(const char* program_path, const char* entry,
                                                   crd::containers::ConstSpan<crd::i64> args,
                                                   crd::containers::ConstSpan<crd::u32> breaks,
                                                   crd::containers::ConstSpan<crd::u32> watches,
                                                   crd::containers::ConstSpan<const char*> actions,
                                                   crd::u32 max_stops, crd::memory::IAllocator* alloc);

// One request through the host's typed diagnostic command service (crd/perf/diag_commands.hpp). The report is the
// service's response document unchanged, so a native caller, this verb and the MCP `diag` tool return the same bytes
// for the same service state. The service, its grant and its file root belong to the host; a request cannot name them.
[[nodiscard]] crd::containers::String verb_diag(crd::perf::DiagCommandService& service,
                                                const crd::perf::DiagRequest& request,
                                                crd::memory::IAllocator*      alloc);

// What a process grants the diagnostic commands it serves: decided when the process starts (command-line flags),
// never by a request. `grant` is a comma-separated authority list (nullptr = "read"); `root` is the directory path
// arguments resolve under (nullptr = none, so path commands answer unavailable).
struct DiagHostOptions
{
    const char* grant = nullptr;
    const char* root  = nullptr;
};

// The command-line form: build a service for `host`, run `request` once, return its report (an `ok:false` report
// when the grant does not parse).
[[nodiscard]] crd::containers::String verb_diag_host(const DiagHostOptions& host, const crd::perf::DiagRequest& request,
                                                     crd::memory::IAllocator* alloc);

// ── MCP ────────────────────────────────────────────────────────────────────────────────────────────────────────
// One JSON-RPC 2.0 request line → the response line ("" for notifications). Handles initialize · ping ·
// tools/list · tools/call (each tool = one verb above; errors surface as isError content, protocol faults as
// JSON-RPC errors). The stdio loop in main.cpp is read-line → mcp_handle → write-line.
[[nodiscard]] crd::containers::String mcp_handle(crd::containers::ConstSpan<crd::u8> request,
                                                 crd::memory::IAllocator* alloc);

// The same, with the host's diagnostic command service bound: tools/list adds the `diag` tool and tools/call runs it
// through verb_diag. The tool's arguments are the request's fields only (command, path, cursor, page_items,
// page_bytes, schema); anything else in `arguments` is ignored, so a call cannot raise the host's grant. With `diag`
// null this is exactly the two-argument form.
[[nodiscard]] crd::containers::String mcp_handle(crd::containers::ConstSpan<crd::u8> request,
                                                 crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* diag);

} // namespace crd::ceridc
