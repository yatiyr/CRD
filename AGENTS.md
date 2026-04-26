# Cerid Engine — Project Rules

> Single source of truth for agents and humans working on Cerid.
> Auto-loaded by OpenCode every session. Keep concise.

## What is Cerid
Cerid is a C++20 game/simulation engine, designed for future
expansion into robotics simulation. It features custom containers, math,
logging, memory, graphics (API-agnostic, Vulkan first), UI, Animation System, Editor, C++ scripting,
and job systems.

## Tech Stack
- C++20 (modern features required, no compiler extensions)
- CMake 3.25+ with Ninja generator (CMakePresets.json)
- MSVC 2022 (primary), clang-cl (verified), Linux GCC (in CI)
- Test framework: Catch2 v3 (via CPM.cmake)
- Format: clang-format (`.clang-format` in repo root)
- Lint: clang-tidy (`.clang-tidy` in repo root)
- MSVC `/Zc:preprocessor` is required (for `__VA_OPT__` in log macros)

## Build & Test
- Configure: `cmake --preset win-debug`
- Build: `cmake --build --preset win-debug`
- Test: `ctest --preset win-debug`
- ASan: `cmake --preset win-asan` then build + test
- Tidy: `cmake --preset win-tidy` then build
- Format: `clang-format -i <file>`
- clang-tidy: `clang-tidy -p build/win-debug <file>`

## Project Structure
```
engine/<module>/
  include/crd/<module>/   public headers (.hpp for C++, .h for C-only)
  src/                    implementation
tests/<module>/           Catch2 tests + CMakeLists.txt
runtime/                  startup skeleton
runtime/examples/         per-module smoke demos (smoke_log, smoke_memory, ...)
docs/ROADMAP.md           phase plan + decision log + "where I left off"
docs/sessions/            one file per session (use SESSION_TEMPLATE.md)
docs/systems/             one short overview per shipped module
docs/research/            research logs (use RESEARCH_TEMPLATE.md)
docs/<module>/<MODULE>_FILE.md  long-form deep-dive for major modules
```

## Coding Standards

### Naming (enforced by clang-tidy)
| Element        | Style       | Example                     |
| -------------- | ----------- | --------------------------- |
| Namespace      | lower_case  | `crd`, `crd::detail`        |
| Class / Struct | CamelCase   | `LogManager`, `Vec3`        |
| Enum / value   | CamelCase   | `LogLevel::Trace`           |
| Function       | lower_case  | `platform_name()`           |
| Variable       | lower_case  | `max_size`                  |
| Member         | lower_case  | `m_name` (m_ prefix)        |
| Constexpr var  | lower_case  | `default_capacity`          |
| Global const   | CamelCase   | `kMaxLogFiles` (k prefix)   |
| Template param | CamelCase   | `ValueType`                 |
| Macro          | UPPER_CASE  | `CRD_ASSERT`, `CRD_OS_WINDOWS` |

### Style (enforced by .clang-format)
- Allman braces (opening brace on its own line)
- 4-space indent, no tabs
- 120-char column limit
- Pointer alignment: left (`int* p`)
- `#pragma once` for headers
- Include order: project `"..."` first, `<crd/...>` second, `<...>` third

### Hard rules
- RAII only. No raw `new`/`delete`. Use smart pointers.
- `std::span`, `std::string_view`, `std::optional` over raw pointers.
- `noexcept` on moves and destructors.
- `[[nodiscard]]` on factories, accessors, and any function where ignoring the return is a bug.
- Concepts/`requires` over SFINAE.
- No `using namespace` in headers. Forward-declare where possible.
- No commented-out code. No TODOs unless explicitly asked.
- Performance-critical paths: data-oriented design, SIMD where measured beneficial.
- Use `CRD_ASSERT` / `CRD_VERIFY` for assertions.

## Definition of Done
1. Compiles clean — no warnings (`/WX`, `-Werror`).
2. clang-tidy and clang-format clean for changed files.
3. New code has unit tests. All existing tests pass.
4. Public API change → `context.md` and `docs/systems/<module>.md` updated.
5. Commit follows Conventional Commits (`feat`, `fix`, `refactor`, `docs`, ...).

## Module Status (snapshot — see `docs/ROADMAP.md` for full detail)

| Module           | Status |
| ---------------- | ------ |
| `crd-core`       | ✅      |
| `crd-log`        | ✅      |
| `crd-memory`     | ✅      |
| `crd-containers` | ✅      |
| `crd-math`       | 🚧      |
| `crd-platform`   | ⏳      |

Module dependency graph is curated. See `context.md`. Don't introduce new edges without surfacing it.

## Agent Roster

You (the human) orchestrate. Local agents do the work. Heavy is for escalation.

| Agent          | Model                  | Mode      | Role                                       |
| -------------- | ---------------------- | --------- | ------------------------------------------ |
| `@planner`     | cerid-coder (64k)      | primary   | Session re-entry, propose today's plan     |
| `@researcher`  | cerid-coder (64k)      | subagent  | SearXNG research, writes to `docs/research/` |
| `@architect`   | cerid-deep (32k+)      | subagent  | ADRs for new systems / cross-cutting changes |
| `@coder`       | cerid-coder (64k)      | subagent  | Implementation                             |
| `@tester`      | cerid-coder (64k)      | subagent  | Tests + iterate-to-green build/test loop   |
| `@debugger`    | cerid-coder (64k)      | subagent  | Bug hunt + regression test                 |
| `@reviewer`    | cerid-deep (32k+)      | subagent  | Code review against Definition of Done     |
| `@docs-keeper` | cerid-deep (32k+)      | subagent  | ROADMAP, sessions, systems, deep-dives     |
| `@heavy`       | claude-opus-4-7 / gpt  | primary   | Escalation only — costs real money         |

## Standard Session Flow

```
/session-start             @planner re-orients you, proposes today's plan
                           ↓ you say "go" (or refine)
/research <topic>          (optional — only if approach is unclear)
@architect                 (optional — only for new systems / big changes)
@coder                     implement
/verify                    @tester iterates build+tests until green (max 5)
/review                    @reviewer checks against Definition of Done
                           (loop @coder if CHANGES_REQUESTED)
/session-end               @docs-keeper writes session doc, updates everything
```

You commit yourself. Agents never run `git commit` or `git push`.

## When to Use `@heavy`

Heavy runs on Claude Opus / GPT — every token costs real money. Use ONLY when:
- Two contradictory `@architect` ADRs on the same question.
- `@debugger` looped 3+ times without progress.
- Final review of a Phase milestone before merging.
- Modern C++ pattern where local models gave incorrect or unsafe code.
- Long-term direction decisions (graphics API, scripting model, etc.).

DO NOT use heavy for: routine code, tests, docs, simple bugs, initial research, "where is X" questions. Heavy will refuse and redirect you anyway.

Invoke via `/escalate <reason>` or `@heavy`.

## Cheat Sheet — Commands

| Command                | What it does                                              |
| ---------------------- | --------------------------------------------------------- |
| `/session-start`       | `@planner` re-orients + proposes plan                     |
| `/session-end`         | `@docs-keeper` updates session doc, ROADMAP, systems, context.md |
| `/status`              | Quick repo glance — git + module status, no planning      |
| `/feature <desc>`      | Print pipeline as checklist (no auto-run)                 |
| `/research <topic>`    | `@researcher` writes log to `docs/research/`              |
| `/bugfix <desc>`       | Diagnose → fix → regression → review                      |
| `/verify`              | `@tester` iterate to green (max 5 cycles)                 |
| `/build [preset]`      | One-shot build + test (no iteration)                      |
| `/review`              | `@reviewer` against current diff                          |
| `/escalate <reason>`   | Hand off to `@heavy` (costs tokens)                       |

## Documentation Conventions

- **`context.md`** — Stable working memory: module status, dependencies, where to look. Updated incrementally by `@docs-keeper`. Never rewritten from scratch.
- **`docs/ROADMAP.md`** — Phase plan + chronological decision log + "Where I left off" (the planner's main re-entry source). Append-only decision log; targeted edits to status table.
- **`docs/sessions/YYYY-MM-DD-<slug>.md`** — One per session. Format from `SESSION_TEMPLATE.md`.
- **`docs/systems/<module>.md`** — Short overview per shipped module. Plain English. Stable; sessions tell the story, overviews describe the result.
- **`docs/research/YYYY-MM-DD-<slug>.md`** — Research logs. Format from `RESEARCH_TEMPLATE.md`.
- **`docs/<module>/<MODULE>_FILE.md`** — Long-form deep-dive for major modules. Only updated when explicitly requested.

## Platform
- Windows 11, PowerShell 7.
- Use PowerShell-compatible commands. Avoid `cat`, `grep`, `sed`, `rm -rf`.
  Use `Get-Content`, `Select-String`, `Remove-Item -Recurse -Force` (cautiously).

## Git Policy
- **Agents NEVER run `git commit` or `git push`.** The user commits themselves.
- Agents may freely run `git status`, `git log`, `git diff`, `git show`, `git branch`.
- Conventional Commits format: `feat(<module>): ...`, `fix(<module>): ...`, etc.
- When an agent finishes, it proposes a commit message in chat. The user runs commit themselves.
