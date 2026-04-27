# Session — 2026-04-27 — Phase 1 closeout / quality check

## Goal

Do a quick but honest Phase 1 retrospective before jumping into graphics.
Validate the tree, refresh the bench picture as far as the current suite
allows, sweep stale wording, and decide whether Cerid is ready to start the
RHI track.

## What we checked

- Verified the current module stack is in place: `crd-core`, `crd-log`,
  `crd-memory`, `crd-containers`, `crd-math`, `crd-platform`, `crd-app`.
- Re-verified the full test matrix already achieved in previous sessions:
  - Debug: `197/197`
  - Release: `196/196`
  - ASan: `197/197`
- Re-ran the release benchmark executable (`crd-bench.exe [bench]`) to see
  whether the Phase 1 baseline could be refreshed cleanly.

## Findings

### Strengths

- **The foundational module graph is now coherent.** Platform and app exist,
  and the next graphics work can plug into a real window + loop + event path
  instead of a speculative shell.
- **Quality bar is strong for this stage.** Three build flavours are green,
  ASan already caught real bugs, and the project now has enough tests to
  refactor with confidence.
- **The architecture is in a good place for graphics.** `crd-platform` is
  GLFW-backed but public-API clean, and `crd-app` remains graphics-agnostic.
  That is the right setup for a minimal `crd-rhi` entry.

### Risks / gaps

- **Benchmark refresh is not fully green.** The release bench suite now fails
  on the `Disabled CRD_LOG_TRACE cost` benchmark because Catch2 detects it as
  optimized away and refuses to measure it. The rest of the benchmark set
  still runs and produces plausible numbers, but this one case prevents a
  clean baseline refresh.
- **A few docs needed wording alignment** from the older `crd-graphics`
  naming toward the newer `crd-rhi` / `crd-rhi-vulkan` / `crd-renderer`
  split. This session cleaned the most important spots.

## Decision

Phase 1 is functionally complete enough to begin graphics planning and even
start `crd-rhi`, but the closeout is recorded as **not perfectly closed** yet
because the benchmark suite needs one small maintenance pass:

- either stabilize the disabled-trace benchmark so it cannot be optimized away
- or explicitly remove/replace that benchmark and refresh the baseline file

This is not a blocker for architectural graphics planning, but it is a real
quality follow-up item and should stay visible.

## Next session starts with

`crd-rhi` v1a scaffold / interface design, with one small optional cleanup
item ahead of it: fix the disabled-trace benchmark if we want a fully clean
Phase 1 closeout before drawing the first triangle.
