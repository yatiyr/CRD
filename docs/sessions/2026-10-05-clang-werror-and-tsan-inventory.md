# Clang `-Werror` sites and the first full TSan run

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.1b](../ROADMAP.md#slice-diag.1b); decision:
> [user decisions 2026-10-05](2026-10-05-user-decisions.md#2-diag1b-tsan). Rules: [AGENTS](../../AGENTS.md).

## Inventory

The hosted TSan lane first needed the clang `-Werror` sites fixed. The REPO.DEV.11 audit had counted 40. A keep-going
build of the whole tree on the reference host (WSL2, clang 18.1.3, the `linux-clang-tsan` preset,
`-ferror-limit=0`) found **793 unique sites in 91 files**:

| Warning | Sites |
|---|---|
| `-Wsign-conversion` | 674 |
| `-Wdouble-promotion` | 111 |
| `-Wunused-variable` | 4 |
| `-Wnested-anon-types` | 2 |
| `-Wunused-private-field` | 1 |
| `-Wformat-nonliteral` | 1 |

The first pass reported 398 sites, because clang stops reporting a translation unit after 20 errors.

## Why sign conversions appeared only now

The tree passes `-Wconversion` to both GCC and clang (`CMakeLists.txt`). In C++, GCC's `-Wconversion` does not include
sign conversions; clang's does. On the reference host, `std::size_t f(int i) { return i; }` warns only under
clang++. No lane had ever enforced sign conversions on this tree. MSVC and clang-cl use a different warning set.

## Decision

The count was 20 times the premise of the 2026-10-05 decision, so it went back to the user with a recommendation:
"go with your recommendation".

- **Sign conversions:** clang gets `-Wno-sign-conversion` beside `-Wconversion`, so the flag means the same on every
  Linux compiler. The comment in `CMakeLists.txt` records the divergence.
- **The other 119 sites:** fixed by hand. GCC also enables `-Wdouble-promotion` and misses these only because clang
  checks more contexts: arguments, initialisers and comparisons. In an engine with bit-exact floating-point oracles an
  unintended promotion is real signal.

## Fixes

- **Float-to-double promotions:** explicit `static_cast<double>` (or `crd::f64`) where a `float` value is meant to
  widen.
  - `ckir_rt.hpp` (38 sites): ReSTIR and the RT kernels' light constants. One call site now reuses `RestirMath`'s
    stored doubles.
  - `render_fullscreen_build.cpp` (17): the clear colour and depth attributes.
  - `test_vulkan_gsplat.cpp` (21), `ckir_oit_test.hpp` (2), and the dense, eigen, randomized-range and
    interpolation tests (6).
  - Two `WithinAbs(x, 1e-3F)` tolerances became `1e-3`, slightly tighter, never looser.
- **`NAN` in the scipy reference tables:** `NAN` is a `float` constant. The three generators
  (`gen_{continuous,discrete,heavy}_refs.py`) now print `static_cast<double>(NAN)`. The committed outputs received the
  identical token change rather than a regeneration, which could move values with a different scipy version. Their
  hashes are refreshed in `scripts/generated-sources.json`.
- **Nested anonymous types:** `String`'s SSO and heap representations are named structs outside the anonymous union.
  The `sizeof(String) == 32` assertion still holds.
- **Unused private field:** the non-Windows `AudioDevice` stub now owns `m_impl` as the WASAPI build does.
- **Non-literal format string:** `ga_append` is declared `format(printf, 4, 5)` under GCC and clang, so every call's
  arguments are now checked. GCC builds `crd-hesap-autodiff-tests` clean, and all 124 cases pass.
- **Unused variables:** three spans in `ChebyshevPreconditioner::apply` are removed. The Windows-only marker flag in
  `crash_capture_specimen.cpp` is guarded by `_WIN32`.
- **Strict tidy findings in the touched files, all predating this work:** eleven `[[nodiscard]]` in `string.hpp`, plus
  a split declaration and a lower-case local constant in `chebyshev.hpp`.

## Verification

- The clang TSan tree builds and links completely with `-Werror`: ninja exit 0, no errors. That includes the targets
  that the first pass never reached behind failed objects.
- `win-debug` full build. The full `win-debug` CTest suite passes on the RTX (7,195 tests, 0 failed, one existing skip,
  879 s).
- The strict tidy gate is clean on all 13 changed C++ files. The brace guard, the generated-source guard and the docs
  validator pass.

## First full TSan run (interim evidence, not the lane)

TSan run on the reference host with the hosted lane's environment:
- lavapipe as the Vulkan device;
- the pinned validation layer;
- `TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1 history_size=4`.

The WSL2 VM restarted at test 6,501 of 6,866, while the Windows suite ran at the same time. The remaining 365 ran
afterwards at `-j 4`.

**Result: 174 of 6,866 tests failed, with 158 ThreadSanitizer reports.** So the hosted lane is not added yet: it would
be red. The reports group as follows.

| Group | Count | Status |
|---|---|---|
| Inside lavapipe (`libvulkan_lvp.so`, `pthread_mutex_destroy` against `pthread_mutex_lock`) | 76 reports | Uninstrumented third-party driver. Candidate for a scoped `called_from_lib` suppression, once confirmed |
| Validation layer `DestroyFence`, called from `VulkanFrameGraph::~VulkanFrameGraph` | 73 reports | Open: either a race in the uninstrumented layer, or engine fence lifetime. Read report by report |
| `FiberPool::for_each_fiber` (`fiber_pool.hpp:94`) during `wait_graph_snapshot` and `monitored_snapshot` | 6 reports | **Engine race candidates**: the diagnostic snapshots read fiber state that workers mutate |
| `perf::frame_mark` in the ring-tearing test | 1 report | Open: a seqlock-style reader is reported unless its accesses are atomic |
| `counter_wait` (full suspension; two sequential waits) | 2 segfaults | Fibers under TSan: check the `__tsan_switch_to_fiber` annotations |
| Linux crash capture (9), sanitizer-specimen harness (5), crash specimen (1) | 15 failures | TSan owns the signal handlers and alternate stacks. The tests must recognise TSan, not be suppressed |
| Watchdog and wait-graph cases, `job samples carry the fiber_id` | 7 failures | Follow from the fiber and snapshot rows above |

The Vulkan raster and frame-graph failures are those reports ending each test through `halt_on_error`.

## Next

Triage in the order of the table, engine candidates first. Add suppressions only for third-party frames proven
uninstrumented. Then rerun, and add the hosted complete-tier lane when the run is green.
