#pragma once

// crd-frame-cook — CEIR-20b: the WORK-PASS COOK. `extract_work_desc` turns an authored frame graph's `work.produce` /
// `work.consume` / `work.compact` passes into a `ceir.gpu::WorkBuildDesc` — the seam where the AUTHORED
// wavefront_work.frame.toml meets `build_work_ceir` + `execute_work_lowered` (retiring the §134 C++ host loop; the
// orchestration becomes an authored asset). ⛔ frame-cook already links crd-ceir-gpu (the build_fullscreen_ceir
// precedent), so naming ceir-gpu types here is in-bounds.
//
// ⛔ MIXED frame: NON-work passes are SKIPPED (the wavefront's trace is a plain `compute.dispatch`). QUEUE-vs-BINDING
// BY RESOURCE KIND (advisor, the 19c per-buffer layout ≠ 20a's queue-with-embedded-count): a produce's %queue = its
// EXACTLY-1 CounterBuffer WRITE, a consume's %queue = its EXACTLY-1 CounterBuffer READ (20a's declared intent — a
// counter's first 4 bytes ARE the device count); a compact's src = the read counter, dst = the written counter. Every
// OTHER referenced resource is a binding. ≠ the required counter count ⇒ `FrameCookError::WorkQueueNotOne` (via
// `where`). ⛔ The %queue/binding OPERAND order here is IR identity, NOT SSBO-slot order — the record-time caller
// re-orders by a fixed per-kernel binding table (the .ckir binding contract); `source_param` = the resource-name hash
// so the caller correlates each queue/binding Value → its device/host buffer.

#include <crd/ceir/gpu/work_build.hpp>   // WorkBuildDesc
#include <crd/framecook/frame_asset.hpp> // FrameGraphDesc / FrameCookError

namespace crd::framecook
{
// Cook the work passes of `desc` into `out` (CLEARED then filled; non-work passes skipped). Returns Ok, or
// WorkQueueNotOne (with
// `*where` = the offending pass name, when non-null) if a produce/consume/compact pass does not reference exactly its
// required CounterBuffer queue(s). The caller keeps `desc` alive while `out` is used (out.stages[].kernel is a
// StringView into `desc`).
[[nodiscard]] FrameCookError extract_work_desc(const FrameGraphDesc& desc, crd::ceir::gpu::WorkBuildDesc& out,
                                               const char** where);
} // namespace crd::framecook
