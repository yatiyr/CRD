# ADR-0006 — Platform v1

<!-- doc-role: decision -->
> Decision record; read status and supersession notes. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Date:** 2026-04
**Status:** Accepted; amended 2026-11-01 ([below](#amendment-2026-11-01-the-window-backend-is-its-own-module))
**Tags:** [platform]

## Decision

- Window backend is GLFW via CPM. Public API backend-agnostic, PIMPL,
  no `GLFWwindow*` leak. Vulkan-ready by default.
- GLFW errors bridge to logger.
- `g_log_platform` is owned inside `crd-platform` (no cycle). The
  containers cycle-break is a historical exception, not the default
  pattern.
- `Extent2D` is a small platform-local POD, not `Vec2<i32>`. Avoids
  weakening `MathScalar` and the `crd-platform → crd-math` link.
- `Window` move-assign hand-written to avoid leaking the OS handle.
- Timing is `std::chrono::steady_clock`. `FrameClock` first tick zero-delta.
- Hybrid input: polling-first `InputState` always present; ordered queue
  opt-in. `Key` / `MouseButton` are Cerid-owned indices.
- Filesystem uses `std::filesystem`, with our own `String`-backed `Path`.
- Threading helpers minimal: name, ids, core counts, affinity, cpu_pause.
  No Mutex/CondVar wrappers, no thread pool (that's `crd-jobs`).
- No file watcher in v1. Lands with shader hot-reload.

## Amendment 2026-11-01: the window backend is its own module

The GLFW context and window (`context.hpp`, `window.hpp`, with the `platform.hpp` umbrella that includes them) moved
into `crd-platform-window` (`engine/foundation/platform-window`), which links GLFW; `crd-platform` keeps the OS
boundary (files, durable saves, asynchronous reads, file watching, dynamic libraries, threads, arguments, the input
snapshot types) and links no window library, so it joins the relocatable package. Include paths, the
`crd::platform` namespace and the decisions above are unchanged
([capability ownership, decision 9](../design/capability-ownership.md#decisions); OWN.1).

## References

- `docs/phases/phase-1-foundations.md`
- `docs/systems/platform.md`
