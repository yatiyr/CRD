# C++ coding contract

<!-- doc-role: rule -->
> Current rule. Current work: [ROADMAP](ROADMAP.md); current rules: [AGENTS](../AGENTS.md).

Read before code changes. [AGENTS](../AGENTS.md) governs conduct; [BUILDING](BUILDING.md) governs verification.
Match the surrounding **hand-formatted** style. Do not run `clang-format -i`; the older auto-format instruction
was superseded by the user's recorded formatting directive. Run incremental LLVM-20 clang-tidy on changed files.

| Element | Style | Example |
|---|---|---|
| Namespace, function, local variable | lower_case | `crd`, `platform_name`, `max_size` |
| Class, struct, enum and enum value | CamelCase | `LogManager`, `LogLevel::Trace` |
| Member | m_lower_case | `m_name` |
| Function-local constant, `const` or `constexpr` | lower_case | `default_capacity`, `expected_count` |
| Namespace/class-scope constexpr, `static` constant | kCamelCase | `kDefaultCapacity` |
| Global constant | kCamelCase | `kMaxLogFiles` |
| Template parameter | CamelCase | `ValueType` |
| Macro | UPPER_CASE | `CRD_ASSERT`, `CRD_OS_WINDOWS` |

C++20 without compiler extensions; Allman braces, four spaces, 120-column target, pointer-left, `#pragma once`,
includes in project → `<crd/...>` → standard/external order. Preserve established local alignment.

- RAII; no raw owning `new`/`delete`. Containers take `IAllocator*` at construction, not as a template parameter.
  No hidden/default malloc escape hatch in engine, tools, tests or probes. Use named allocators where required.
- No owning STL containers. Use Cerid Array/String/HashMap; non-owning span/string_view/optional and standard
  algorithms are permitted subject to the repository's numeric/sort guards.
- `noexcept` moves/destructors; `[[nodiscard]]` factories/accessors; Concepts/requires over SFINAE.
  No `using namespace` in headers; no commented-out code or new TODOs without user direction.
- Use `CRD_ASSERT`/`CRD_VERIFY`. Use `static_cast<T>(literal)` for non-exact defaults in templated numeric code to
  avoid GCC float-conversion failures. Public physical quantities follow PRINCIPLES/ADR-0078.
- Append new virtual methods at the **end** of an interface. Reordering requires deliberate ABI/migration work
  and the affected clean/LTCG checks; never silently shift vtable slots.
- Tests use ASCII names; brackets belong in Catch2 tags, not test names. Keep registration and guard coverage.
- Code lives in `engine/<family>/<module>/include/crd/...` and `src/`; tests in `tests/<family>/<module>/`.
  Public API/dependency changes update the source map and relevant contract. No vendor types cross public boundaries.
