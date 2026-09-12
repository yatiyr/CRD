> **Dated phases reference.** Current order/status: [ROADMAP](../../ROADMAP.md). Read [AGENTS](../../../AGENTS.md) for current rules.
> Old Next lists, API examples and implementation claims below require current-source verification. Unique requirements remain in force through their owning master rows.

# Phase 1.6 — Configuration substrate

<!-- doc-role: archive -->
> Historical reference; old status/schedules/grants are not current instructions. Current work: [ROADMAP](../../ROADMAP.md); current rules: [AGENTS](../../../AGENTS.md).

> **Tracking consolidated 2026-09-12:** this file retains detailed requirements and dated evidence.
> Live order/status/subslices are only in [ROADMAP](../../ROADMAP.md#master-table). Earlier schedules and Next
> pointers below are historical; current ownership/sequence follows ADR-0129 and the execution contract.

**Status:** ◧ 1.6a shipped; 1.6b (hot-reload hook) deferred — no active work. (Header refreshed 2026-08-07; the plan below is the original.)

Small but pervasive — everything downstream consumes it. ImGui overlay (2.1)
needs a config format; this phase ships the substrate so 2.1 has it ready.

## Slices

### 1.6a — `crd-config` core  ✅ shipped

- `toml++` wrapper (single-header, exceptions-free mode)
- Schema-with-defaults: every `get<T>(key, default)` non-fatal with logged
  warning on miss / type mismatch
- Typed get/set: `int`, `float`, `String`, arrays, color (`Vec4f`)
- Parse errors via `g_log_config` channel
- Sample config under `engine/config/sample.toml`

Shipped in session: `docs/sessions/2026-04-28-config-core.md`

Delivered:

- `crd-config` module scaffold
- `toml++` wrapper in exceptions-free mode
- typed `get<T>(key, fallback)` and `set<T>(key, value)`
- non-fatal fallback behavior with warnings on miss / type mismatch
- parse errors via `g_log_config`
- sample config + `smoke_config`

**Exit criteria:**
- Debug + Release + ASan all green
- Public header is `engine/config/include/crd/config/config.hpp`
- `docs/systems/config.md` overview written

### 1.6b — Hot-reload hook  ⏳

- Optional file-watch callback, off by default
- Real file watcher implementation parked until Phase 2.3 pulls it in for
  shader hot-reload
- For now: explicit `Config::reload()` re-reads and fires registered
  callbacks

### 1.6c — First consumers  ⏳

- ImGui layer config: `runtime/configs/imgui_layer.toml`
- Log config: `runtime/configs/log.toml` (optional, may slip)
- Input bindings: `runtime/configs/input.toml` (optional, may slip)

## Decisions

- ADR-0012 — Configuration substrate

## Open questions

- Hot-reload — 1.6a or 1.6b? Currently 1.6b. Hold unless ImGui forces.
- Config namespacing convention? `imgui.theme.preset` vs `[imgui.theme]
  preset`. TOML handles both; pick one for consistency in 1.6a.
