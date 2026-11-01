# 2026-11-01 — REPO.3: one failure path for every CUDA program, CUDA under the Allman and provenance guards

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [REPO.3](../ROADMAP.md#slice-repo.3), the items the [CUDA route note](2026-11-01-repo-3-cuda-route.md#remaining-on-repo3)
left; [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0)'s hosted evidence. Built on `cerid-track-b` (spot c4-standard-8,
Intel Xeon, Ubuntu 24.04, GCC 13.3, CMake 4.4.5; no GPU) in its `linux-gcc-debug` tree (`~/cerid/build`) and the
`linux-gcc-release` CUDA tree (`~/cerid-cuda-build`).

## CI first

- **No push run has run since 03571fa3.** Each push replaces the push group's pending run: the runs at cacd6547
  (41291759660), a883e502 (41292341675) and 30a34cc3 (41292729183) were cancelled with no job started, and 41293138483
  at 71423138 was pending when this batch pushed.
- **The dispatch run 41274380605 (de5e10b0).** win-debug-scalar (124764040380, MSVC at the SSE2 floor) and
  win-public-checks (124764040379) finished their tests: the 31 cases whose names carry `ceridc` or MCP, and
  `platform: the protocol owns its output stream and stray writes go to stderr`, passed on each
  (`crd-ceridc-mcp-conformance`, the 2026-07-28 and 2025-11-25 cases, the envelope, the agent gate and the diag tool's
  native, verb, MCP and real-binary cases included). Their one failure, and win-asan shard 2's, win-shipping's and win-shipping-profile's, is
  `filesystem: the last write time converts exactly and orders files within one second` at `test_filesystem.cpp:145`
  (`REQUIRE( stamp(older, at) )`): the stamp `clock_cast` overflowed the Microsoft file clock, which track A's cc5ee9e9
  (after de5e10b0) fixes by narrowing to the file clock's tick first. win-debug was still testing at 09:35 UTC.
- **The 8311a54e run (41286750175)** fails win-clang-cl-shipping at `__udivti3`/`__divti3`, which 7cfed26c (not in
  8311a54e) fixes.

## One failure path for the MLP and onesweep programs

The five programs the CUDA route left on their own helpers now check every CUDA and cuBLASLt call through
`CRD_CUDA_CHECK` and stop at a result their judge refuses, exit 1 with the reason, as the other twelve do:

- **`bench/gpu-compute/mlp_bench.hpp`** holds what the four MLP programs repeated: the network's shape, the boards' LCG
  and the deterministic weights and inputs, the cuBLASLt forward (`LtForward`: one GEMM per layer with the ReLU
  epilogue, run as the board's ping-pong or keeping every activation) and backward (`LtBackward`: the mask kernel, the
  dW and da GEMMs with a 256 MiB workspace and the heuristic's first algorithm for each shape, refused when it offers
  none instead of running a zeroed algorithm), the CPU oracle of the output layer's dW, the two judges and the row
  timer. The measurement is the boards': three warm-up calls, then the least host time per call over 7 runs of 50
  (`crd_bench::best_wall_ms`), so the recorded ratios stay comparable. The hand-written kernels stay in their programs.
- **`cuda_bench.hpp`** gains `device_array<T>`, an owned device allocation (one owner for every program's buffers).
- **Resolved**: the never-set `NGROUP` and `NOATOMIC` experiment gates (8 groups; the atomic scatter always on, so
  the fused dW is always judged); an allocated, never-used gradient buffer; `ckir_mlp_bench`'s weight-convention probe,
  which printed a guess and judged nothing (the comparison with cuBLASLt's column-major pipeline proves the convention);
  its determinism check now compares raw bits and fails the run; the onesweep program's CUB check now judges sorted
  order and the permutation as the CKIR sorts are judged (it compared only the first and last keys), its kernel profile
  runs through the same timer, and its stale `<cstdio>`-for-device-printf include and unbuffered stdout are gone (the
  emitted kernels print nothing).

## The onesweep emitter call

Regenerating `ckir_onesweep_gen.cu` found a defect in its generator: `test_ckir_subgroup.cpp`'s `[.emit-cuda-sort]`
called `build_sort_scatter_onesweep(g, epb, 256, 8, shift, pass, nblocks, true)` after the builder gained its `lanes`
parameter, so `true` became one lane per subgroup (a 256 x 256-word rounds workspace, 0x42404 to 0x48404 bytes of
shared memory, which ptxas refuses against the 0xc000 static limit) and the hardware `__match_any_sync` rank was off.
The case now passes 32 lanes and the hardware match; the regenerated A, B and C scatters use `__match_any_sync` (225
sites) and fit (C is the 41 KiB configuration). Its header line names configuration C, which the file always held.

## CUDA under the guards

- **Allman.** `check-allman-braces.py` holds `.cu` and `.cuh` (3,291 sources, PASS); the twelve converted programs
  and `spectrum_multiply.cuh` were already clean, the five rewritten ones are. `.gitattributes` stores CUDA sources
  with LF like the C++ ones.
- **Provenance.** `check-generated.py` scans `bench/` and `.cu`/`.cuh` for generation markers, and the eight
  CKIR-emitted kernels are in `scripts/generated-sources.json` as `reproducible`, generated by their hidden Catch2
  cases (`[.emit-cuda-mlp]`, `[.emit-cuda-mlp-bwd]`, `[.emit-cuda-sort]`, `[.emit-fft-cuda]`), each note naming its case.
  The FFT emitter now writes a marker line like the others. Both MLP kernels reproduced their committed bytes; the
  onesweep and five FFT kernels were regenerated (the current emitter names every value once, so the old bytes
  predated it), with two runs writing the same bytes. The two hand-written duel programs no longer open with a
  sentence the marker rule reads as a generation marker.
- **Literal paths.** The three emit cases write through `CRD_REPO_DIR` (both test targets define it), and the hidden
  neural-material and ocean-frame cases of `test_vulkan_context.cpp` write their BMPs to `CRD_REPO_DIR "/build/..."`;
  `tests/.clang-tidy`'s comment names the root file instead of a checkout path.

## Evidence

- `crd-kir-tests` (linux-gcc-debug) rebuilt and the four emit cases pass, run from a scratch directory; the
  Vulkan test target compiles.
- `cmake --build ~/cerid-cuda-build/linux-gcc-release --target crd-gpu-vs-cuda -- -k 0`: all 17 programs build with
  nvcc 13.3.1 for `sm_89` and the warnings as errors, including the regenerated kernels.
- On the GPU-less machine each rewritten program stops at its first CUDA call with the reason, exit 1, e.g.
  `mlp_fused_bench.cu:120: cudaGetDeviceProperties(&prop, 0) failed: CUDA driver version is insufficient for CUDA
  runtime version (35)`.
- `scripts/test-repository-tools.py AllmanBraces Provenance` (new: a marked kernel under `bench/` must be listed; the
  CUDA sources are in the Allman set and the emitted ones are not), `check-generated.py` (199 entries),
  `check-allman-braces.py`, `check-repository.py` and `check-master-plan.py` pass.

## Remaining on REPO.3

- The run on the measuring PC's NVIDIA GPU (`bash scripts/run_bench_gpu_cuda.sh` under WSL2), the boards' rows and
  ratios against the recorded ones.
- The LunarG Qt line from a cold Windows lane.
- Literal checkout paths left in scripts: the reference-corpus generators write to `/mnt/d/Dev/cerid/...`
  (`v14a_dtypes_corpus.py`, `v14b_elementwise_corpus.py`, `v14e_einsum_corpus.py`, `v14g_export_corpus.py`,
  `v14j_decomp_oracle.py`, `v14m_nn_oracle.py`, `v14m_nn_peers.py`, `bench_einsum_plan.py`), as do
  `v14_matlab_board.m` and `v14_matlab_ttb.m` (`D:/Dev/cerid/...`); each to the repository root its own location
  gives. (The `scripts/test-*.py` fixtures that spell a checkout path are sample data, not paths they open.)
- The close after [REPO.3d](../ROADMAP.md#slice-repo.3d).
