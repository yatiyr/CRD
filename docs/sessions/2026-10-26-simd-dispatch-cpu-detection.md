# SIMD-DISPATCH: the dispatch contract, and CPU detection with one startup selection

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- Push run [40340291698](https://github.com/yatiyr/CRD/actions/runs/40340291698) (head `4e0a0298`) failed seven jobs
  on the two flight-ring races, and 40351178841 (head `e4c56fb8`, running) win-clang-cl on the post-window case
  (`take_capture` false at line 324, the same `REQUIRE` as line 306 before the file grew). `13f8e651` and `1df6bdd1`,
  both newer than those heads and in HEAD, repaired them ([races](2026-10-26-ci-audit-and-flight-ring-races.md)).
  linux-gcc-asan's other failure is the registered TP-5. The cancelled runs 40341016749 to 40384366175 have no failed
  job.
- Needs CI audit: no complete-tier run has finished since 40307186916, so DIAG.11c and DIAG.12 stay as that audit
  left them. Nothing was dispatched (a push run is pending and one is in progress).

## Design

ADR-0134 settles the mechanism of every clause of the row: the levels, one detection owner in crd-core, the
intersection over every CPU, the `CRD_CPU_LEVEL` override, the baseline build, per-level kernel units in per-level
namespaces with routine-level dispatch (ADR-0088's revert came from a call per microkernel), the disassembly guard,
cross-level determinism, the forced-level arms with SDE, the reporting and the boards.

## Implementation

- `crd/core/cpu_features.hpp` and `src/cpu_features.cpp` (crd-core). One table maps each of 45 features to its CPUID
  word, bit and XCR0 state; `decode_cpuid` is a pure function of the words, so every gate is tested without the
  hardware. AVX10 version and vector length come from leaf 0x24, the hybrid core type from leaf 0x1A. XGETBV is
  inline assembly on GCC and Clang, whose `_xgetbv` needs `-mxsave`, which this baseline unit does not take.
- `detect_cpus()` pins the calling thread to every CPU (Windows: every processor group; Linux: an 8,192-CPU set on the
  stack, no heap) and restores it. `add_cpu_report` intersects; `apply_cpu_override` is the pure override;
  `cpu_selection()` reads both once.
- TickClock's `counter_invariant` reads the selection; its own CPUID code is gone.
- The specimen harness gains `self_executable()`, which the Catch2 fault test now uses instead of its own copy.

On this host (i9-14900K, Windows): 32 CPUs visited, 0 refused, both core types, uniform, XCR0 0x7, level avx2, with
`sse2 ... avx-vnni hybrid invariant-tsc` usable. Under WSL2 the same 32 CPUs, and the hypervisor hides the hybrid
bit. A second walk takes about 2 ms.

## Verification

`[cpu]` in crd-core-tests (8 cases): every level's features, each OS-state gate, AVX10 at 256 and 512 bits, APX and
AMX state, leaves above the maximum, the hybrid core type, the intersection of a performance and an efficiency core,
the override (pure, and in child processes started with `CRD_CPU_LEVEL`), the restored affinity, and the selection
against the OS's own view: `IsProcessorFeaturePresent` on Windows, every processor's `/proc/cpuinfo` flags on Linux.

Teeth on win-debug, each built, failing, restored and rebuilt:

| Tooth | Failed |
|---|---|
| AVX2 read from bit 6 | 4 cases, the OS view among them |
| XCR0 gate removed | the two state cases (`3 == 2`) |
| override clamp removed | the pure and the child-process override cases |
| affinity not restored | the restore case |
| `usable` not intersected | the intersection case (`4 == 3`) |
| walk stops after one CPU | the selection case (`1 == 32`) |

The AVX2 tooth on linux-gcc-debug failed the cpuinfo oracle case and three others. After the restores the 8 cases
pass on win-debug and linux-gcc-debug, with the `catch2 fault`, `TickClock` and `MonotonicClock` CTests and the 15
ctest guards on win-debug.

## Left in the row

Baseline build and per-level kernel units with their dispatch tables (crd-math first), the isolation guard, the
cross-level differentials, the forced-level CI arms and SDE, the level in the doctor, host tuple and board records,
and the boards per level.
