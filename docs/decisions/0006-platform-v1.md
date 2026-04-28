# ADR-0006 — Platform v1

**Date:** 2026-04
**Status:** Accepted
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

## References

- `docs/phases/phase-1-foundations.md`
- `docs/systems/platform.md`
