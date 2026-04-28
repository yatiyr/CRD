# ADR-0001 — Build & language

**Date:** 2026-04
**Status:** Accepted
**Tags:** [build] [lang]

## Decision

- C++20, no compiler extensions. Allman braces, 4-space indent, 120 col.
- CMake 3.25+ + Ninja. `GLOB_RECURSE CONFIGURE_DEPENDS` for source detection.
- CPM.cmake for third-party deps; static libs for engine modules; dynamic
  only for scripting / plugins.
- MSVC `/Zc:preprocessor` is required (for `__VA_OPT__` in log macros).

## References

- `docs/phases/phase-1-foundations.md`
