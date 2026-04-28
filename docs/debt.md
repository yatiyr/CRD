# Cerid — Open Debt

Items that are not blockers but should not be forgotten. When picked up,
move to a session log entry and remove from here.

## Active debt

- **Disabled-trace benchmark** is broken in the Release bench suite. Fix or
  replace before refreshing `docs/bench/baseline_2026-04.md`.
- **Doxygen-style per-symbol comments** are uneven in `crd-core`.
- **No SPSC `RingBuffer`** yet; v1 is single-threaded refuse-on-full.
  Lock-free version arrives with `crd-jobs` (Phase 2.5).
- **No file watcher in `crd-platform`.** Lands with shader hot-reload
  (Phase 2.3).
- **Multi-viewport ImGui** deferred — Vulkan multi-viewport has known rough
  edges. Single-viewport docking only in Phase 2.1.

## Cleared debt

(empty — items move here with a date when resolved)
