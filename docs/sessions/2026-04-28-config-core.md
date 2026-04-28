# Session — 2026-04-28 — crd-config core

## Goal

Ship the configuration substrate core before ImGui overlay work begins: a
typed TOML wrapper with non-fatal schema-with-defaults behavior.

## What we built / changed

- **`engine/config/`** new module
- **Dependency:** `toml++` via CPM
- **Public API:** `include/crd/config/config.hpp`
- **Log channel:** `g_log_config`
- **Implementation:**
  - `load_from_string()`
  - `load_from_file()`
  - `reload()`
  - `contains()`
  - typed `get<T>(key, fallback)`
  - typed `set<T>(key, value)`
  - dot-path nested access (`imgui.theme.preset`)
- **Supported types:**
  - `int`, `i64`, `f32`, `f64`, `bool`, `String`
  - `Array<i64>`, `Array<f32>`, `Array<String>`
  - `Vec4f`
- **Behavior:**
  - missing key -> warning + fallback
  - type mismatch -> warning + fallback
  - parse failure -> error + load failure
- **Sample config:** `engine/config/sample.toml`
- **Smoke:** `runtime/examples/smoke_config.cpp`
- **Tests:** `tests/config/test_config.cpp`

## Plain-English explanation

Cerid now has a real configuration substrate instead of ad hoc parsing. The
API is intentionally boring and safe: you ask for a value by key, provide a
fallback, and if the key is missing or malformed the engine logs a warning and
keeps going. That is the right behavior for downstream systems like the ImGui
debug overlay, where config should improve workflow rather than make startup
brittle.

The module also deliberately keeps runtime config as text. This is not part of
the cooked runtime asset pipeline; it is a small human-authored settings layer.

## Decisions made

- Reused the already-pinned config substrate ADR (`ADR-0012`); no new ADR was
  needed.
- Dot-path lookup is the stable API even when TOML uses nested tables.
- Hot-reload is still deferred to 1.6b; this slice stays focused on the core.

## Files touched

- `CMakeLists.txt` — added `toml++` CPM package and `engine/config`
- `engine/config/CMakeLists.txt` — new
- `engine/config/include/crd/config/config.hpp` — new
- `engine/config/include/crd/config/log_channel.hpp` — new
- `engine/config/src/config.cpp` — new
- `engine/config/src/log_channel.cpp` — new
- `engine/config/sample.toml` — new
- `tests/CMakeLists.txt` — added `tests/config`
- `tests/config/CMakeLists.txt` — new
- `tests/config/test_config.cpp` — new
- `runtime/CMakeLists.txt` — added `smoke_config`
- `runtime/examples/smoke_config.cpp` — new
- `docs/systems/config.md` — new
- `docs/phases/phase-1.6-config.md` — 1.6a marked shipped
- `docs/ROADMAP.md` — hub status updated
- `context.md` — current focus / last shipped / test counts updated

## Tests / verification

- `win-debug`: 209/209
- `win-release`: 208/208 (Debug-only stats test correctly skipped)
- `win-asan`: 209/209, no leaks, no UAF, no OOB
- `smoke_config` loads `engine/config/sample.toml` and prints parsed values

## Next session starts with

ImGui debug overlay (2.1), now consuming `crd-config` as its first real user.
