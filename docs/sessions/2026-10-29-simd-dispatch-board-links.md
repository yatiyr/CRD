# 2026-10-29 — SIMD-DISPATCH: every board's program a CMake target, one level list

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md);
follows [ARM64 traps](2026-10-29-simd-dispatch-arm64-traps.md).

## CI first

Push run 40826432863 (dd860168) is still running. Its one new failure, linux-gcc-shipping, is the MTTKRP oracle's
`fids[5]` at the link of crd-hesap-tensor-tests, the failure linux-gcc-release had: f5be91c4 fixed it, after dd860168.
The run's other failures were traced in the last note. Every later push run was replaced before any job started
(0 jobs). DIAG.11c and DIAG.12 stay Needs CI: no completed run carries their hosted evidence.

## Why

The boards per level need every program built the way the engine is and run at every level the host runs. Eight
boards did neither:
- Five tensor boards (dtype, elementwise, reduce, permute, einsum) compiled `scripts/bench_*.cpp` by hand with g++.
  They found the SIMD target by grepping `compile_commands.json`, and their own level list was only detected and
  portable, so no board ran sse2 or sse4.2, the levels hesap-tensor builds.
- Three SUNDIALS boards (IMEX, Krylov, sensitivities) linked against the checkout's old `build/linux-gcc-release`
  tree without crd-hesap-ode, at one level.

## What landed

- **One level list.** `cpu_level_record --levels` prints the board's arms on the pinned CPUs: detected, then every
  lower level that both the CPUs and the build's cap include, highest first. The detected arm's own level is not
  listed again. On this host the list is `detected sse4.2 sse2 portable`; on an AVX-512 host it adds avx2.
  `run-lane-bench.sh` takes it as its default (CRD_BENCH_LEVELS still overrides it). A family without an sse4.2
  table runs its sse2 table at sse4.2, which is what such a CPU runs.
- **Tensor boards.** The five sources moved to `runtime/examples` as retained benches (CRD_BUILD_RETAINED_BENCHES),
  so linux-gcc-debug compiles and links them on every push. Their scripts keep the gates and the peers and call
  `run-lane-bench.sh`, permute once per worker count. `bench-level.sh` is deleted, since no script sources it now.
  bench_dtypes now checks its arena allocations (`allocate_array_or_fatal`), which crd-check-allocation-results
  requires once the file is under `runtime/examples`.
- **SUNDIALS boards.** New option CRD_BUILD_HESAP_VS_SUNDIALS (local only) finds SUNDIALS 6 and builds the three
  benches against crd-hesap-ode with the tree's warnings as errors. SUNDIALS is a compiled C library, so it needs no
  peer library. Each script is now one `run-lane-bench.sh` call, and the three sources' formatter findings are fixed.
- `source-routes.json` routes all eight by option. ADR-0134 §8 and `scripts/README.md` name the default list.

## Checks

- win-debug: `cpu_level_record` builds, and `--levels` prints `detected sse4.2 sse2 portable`. A bad argument exits
  2. Tidy is clean.
- linux-gcc-debug mirror tree: the five tensor benches compile and link with the tree's warnings as errors.
- linux-gcc-release mirror tree with CRD_BUILD_HESAP_VS_SUNDIALS: SUNDIALS 6.4.1 is found and the three benches
  link. `run-lane-bench.sh` ran once with an empty level list (build only). The default list, read in the lane
  under taskset, has four arms. All eight scripts pass `bash -n`.
- The ctest guards, clang-format on the changed C++, and the repository validators. As the fast-row order says, no
  board was run.

## Left

The Boost peers (a download, awaiting the user), the no-peer-header check after them, and the row's other Left
items.
