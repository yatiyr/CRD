#pragma once

// verbs.hpp — GEO-11 (D-007 row 76): the AGENT SURFACE — every engine operation as a headless verb emitting a
// MACHINE-READABLE JSON report, and `mcp_handle` exposing the SAME verbs over MCP (JSON-RPC 2.0, the stdio
// transport's per-line payload). ⛔ agent edits are TRANSACTIONAL: every verb validates COMPLETELY before its
// first side effect — an invalid request changes NOTHING on disk (and `dry_run` stops a valid one before the
// write). The CLI (main.cpp) and the MCP loop are thin shells over these functions — one implementation, two
// transports (the Blender-MCP lesson: the surface is the product, the socket is plumbing).
//
// Verbs: import · cook · query · instantiate · sequence · render · export_timeline · inspect · diag.

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
// script. It runs the same scripted inspection (crd/ceir/cook/inspect_script.hpp) as the `program.inspect` diagnostic
// command, which is how an agent transport reaches it: through the `diag` tool, under the host's Execute grant. This
// verb is the command line's convenience form (any readable path, one report) and is not itself an MCP tool.
// DIAG.9a: `record_path` (null: none) writes the inspected run as a new run record (crd/ceir/cook/replay_record.hpp)
// that `replay.run` replays without a session; an existing file there is refused before anything runs, and the
// report's `record` object says whether it was written (a cancelled run is not recorded). `seed_text` (null: none) is
// a decimal u64: the run's host random streams (input.random reads `SeededInputs` of it, and a record keeps every
// draw); without it the host has no random source and a draw fails input-unavailable. A malformed seed is refused
// before anything runs. `clock` (null: none) gives the run's time domains, as replay.record's clock arguments do:
// input.clock and input.time_step read them, a record keeps every read, and a malformed value is refused before
// anything runs. `events_text` (null: none) gives the run's input event queue 0, as replay.record's `events` does
// (ceir::cook::parse_events_argument: at most 32 comma-separated events; empty is an open queue with no event):
// input.event takes them in order, a record keeps every read, and a malformed list is refused before anything runs.
// Without it the host has no event queue and input.event fails input-unavailable.
struct InspectClockFlags
{
    const char* clock    = nullptr; // --clock: only "wall" (the wall domain reads the host's monotonic clock live)
    const char* sim_time = nullptr; // --sim-time: a decimal i64, the sim domain's reading in nanoseconds
    const char* sim_step = nullptr; // --sim-step: a decimal i64, the sim domain's current step in nanoseconds
};
[[nodiscard]] crd::containers::String verb_inspect(const char* program_path, const char* entry,
                                                   crd::containers::ConstSpan<crd::i64> args,
                                                   crd::containers::ConstSpan<crd::u32> breaks,
                                                   crd::containers::ConstSpan<crd::u32> watches,
                                                   crd::containers::ConstSpan<const char*> actions,
                                                   crd::u32 max_stops, crd::memory::IAllocator* alloc,
                                                   const char* record_path = nullptr,
                                                   const char* seed_text   = nullptr,
                                                   const InspectClockFlags* clock       = nullptr,
                                                   const char*              events_text = nullptr);

// One request through the host's typed diagnostic command service (crd/perf/diag_commands.hpp). The report is the
// service's response document unchanged, so a native caller, this verb and the MCP `diag` tool return the same bytes
// for the same service state. The service, its grant and its file root belong to the host; a request cannot name them.
[[nodiscard]] crd::containers::String verb_diag(crd::perf::DiagCommandService& service,
                                                const crd::perf::DiagRequest& request,
                                                crd::memory::IAllocator*      alloc);

// Register the commands ceridc serves beyond crd-perf's built-ins with `service`: program.provenance,
// program.inspect and replay.prepare (crd-ceir-cook) over the CEIR dialects the inspect verb runs, and gpu.resources.
// Every ceridc service binds through this, so the CLI verb, the MCP tool and a test's native service list and answer
// the same commands. False when the service refuses a registration.
[[nodiscard]] bool bind_diag_commands(crd::perf::DiagCommandService& service);

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
// page_bytes, schema, and `args`: an object of the command's named arguments, each a string); anything else in
// `arguments` is ignored, so a call cannot raise the host's grant. With `diag` null this is exactly the two-argument
// form.
[[nodiscard]] crd::containers::String mcp_handle(crd::containers::ConstSpan<crd::u8> request,
                                                 crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* diag);

} // namespace crd::ceridc
