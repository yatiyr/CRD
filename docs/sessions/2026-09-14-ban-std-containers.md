# Enforce the no-owning-STL-container rule with a guard, and convert the tree

<!-- doc-role: historical -->
> Dated evidence. Contract: [CODING](../CODING.md) ("No owning STL containers. Use Cerid Array/String/HashMap;
> non-owning span/string_view/optional and standard algorithms are permitted"), [PRINCIPLES](../PRINCIPLES.md).
> Rules: [AGENTS](../../AGENTS.md). This enforces a longstanding rule that had no lint guard.

## User direction

The maintainer required that the owning-STL-container rule already stated in CODING.md be enforced by a hard guard,
like the existing `check_no_std_sort`, and that every existing violation be converted to the engine containers and
kept green. The scope is owning STL **containers** (and algorithms, already covered by the numeric/sort guards); the
non-owning views `std::span`/`std::string_view`/`std::optional` and `<algorithm>` remain allowed, as CODING.md states.

## The guard

[scripts/check_no_std_containers.sh](../../scripts/check_no_std_containers.sh) and its
[PowerShell twin](../../scripts/check_no_std_containers.ps1), modelled on `check_no_std_sort`. It bans, tree-wide
across `engine`, `tools`, `tests` and `sandbox` (including generated `.inc` data): `std::string`/`basic_string`/
`wstring`/`u*string`, `std::vector`, `std::array`, `std::deque`, `std::forward_list`, `std::list`, `std::map`/
`multimap`/`unordered_map`/`unordered_multimap`, `std::set`/`multiset`/`unordered_set`/`unordered_multiset`,
`std::stack`, `std::queue`, `std::priority_queue`, `std::valarray`. It strips `//` and single-line `/* */` comments
first, so explanatory prose ("our std::vector") never trips it, and runs one `grep`/`Select-String` pass plus a
per-candidate re-check (about three seconds). Registered as the CTest `crd-no-std-containers-check` in
[tests/foundation/math/CMakeLists.txt](../../tests/foundation/math/CMakeLists.txt) (both the PowerShell and bash
branches), so it runs on every hosted lane. A genuine third-party boundary may mark a single line
`crd-lint-allow-std-container`; no line in the tree uses it.

## The conversion

The guard initially reported 306 owning-container lines across ~60 files; all were converted (guard now PASS,
zero violations):

- **Foundation.** jobs scheduler/worker pool (`std::vector`→`Array`, elements `unique_ptr`/`thread` unchanged); the
  whole log module — `std::deque`→`RingBuffer` for the async queue, `std::string`→`String` with `std::format_to`
  into a `String`, ring-buffer sink records; `assert.cpp` keeps a fixed C array (crd-core cannot depend on
  crd-containers); platform filesystem and dynamic-library Win32 wide-char boundaries (`std::wstring`→
  `Array<wchar_t>` for owning buffers, `std::wstring_view` for the read-only parameter); config toml++ reads via
  `std::string_view`; `input` key-state as C arrays.
- **Geometry / numerics / physics / assets.** the BVH, Delaunay, CDT, convex, primitives, supernodal-LU, rigid-body
  and resources/scene-cooker test suites (`std::vector`→`Array`, `std::array`→`FixedArray` or C array, `std::set`→
  `HashSet`), sha1 (`std::array`→a filled `FixedArray`), the DX12 work-graph context.
- **Tools.** asset-cooker preset/profile toml++ reads via `std::string_view`.

Two additive, non-breaking substrate changes made the conversion clean:

- **`crd::containers::String`** gained `value_type` (so `std::format_to`/`std::back_inserter` work), a
  `(count, char)` fill constructor, and `operator+=`.
- **`crd::containers::Array`** gained a standard iterator-range constructor (deduction excludes the `(usize,
  IAllocator*)` capacity ctor, so it never shadows it).

Semantic hazards found and handled, not papered over:

- `std::vector<T> v(n)` (n elements) silently maps to `Array<T> v(n)` (n **capacity**, size 0); every such site was
  converted to declare-then-`resize` so it does not read out of bounds.
- `crd::Array` cannot hold `std::atomic` (its relocation move-constructs, which atomics delete); the two atomic test
  arrays became fixed C arrays with value-initialisation.
- `std::array<T,N>` always has N elements but `FixedArray<T,N>` starts empty; the one index-assigned case (sha1's
  digest) fills via `push_back`, the rest are aggregate-initialised.

## Verification

- **Guard:** `check_no_std_containers.sh` PASS (0 violations); the `crd-no-std-containers-check` CTest passes.
- **Build:** `win-debug` (Ninja, MSVC 19.51, vcvars-sourced) builds fully, 0 errors.
- **Tests:** the affected suites — log, jobs, containers, memory, platform, core, perf, geometry (bvh/delaunay/cdt/
  convex/primitives), assets (resources/scene-cooker), numerics (supernodal-lu), physics (rigid-body) — 916 of 916
  pass under `ctest`.
- **Docs:** `check-master-plan.py` and `check-repository.py` PASS.

## Handoff

One diff: the two guard scripts, the CTest registration, `crd::String`/`crd::Array` additions, and the ~60 converted
files. Local evidence is complete on `win-debug`; the hosted lanes own the full matrix (Linux, the sanitizer lanes,
clang-cl, the shipping configs), so the change is `Needs CI` until the maintainer's published run is green. No commit
was made by the agent.
