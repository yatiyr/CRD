# DIAG.11b follow-up (n): every source held to a build route

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [test instruments](../design/test-instruments.md#source-routes),
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b). Rules: [AGENTS](../../AGENTS.md).

## CI read at the start

`gh run list --branch main --limit 15` at 07:57 local (2026-10-19). No completed run of a pushed commit had a failure
without a fix already on main:

- [39189832409](https://github.com/yatiyr/CRD/actions/runs/39189832409) (`471109f6`), in progress with 18 of 25 jobs
  green. `windows-clang-cl (win-clang-cl)` (job 117873846630) and `windows-clang-cl-shipping` (job 117873846627)
  passed `crd-check-include-budget` (`22/7784 ... Passed 4.79 sec` and `22/7607 ... Passed 2.48 sec`). An entry the
  probe cannot compile is UNCHECKED and fails the check (held by a case since `56e45b71`'s batch), so a pass on both
  clang-cl lanes is the hosted proof that every budget entry is compiled and within there: the operator's loose end 5
  is closed. Both repository jobs and windows-native were green as well (loose end 4, the 8.3 path, was read on the
  same run by the previous batch).
- 39200628155, 39195570971, 39195138375: cancelled by newer pushes; `gh run view --json jobs` lists no job for any of
  them. 39201708392 (`56e45b71`, complete tier): pending.

## What was wrong

A test source that no target lists compiles nowhere and its cases never run: `tests/execution/ceir/test_scene.cpp`
(CEIR-17a) sat that way until 2026-10-17. Nothing caught it. A lexical rule over the CMake files cannot (159 test sources
are globbed, six are named by another directory's CMakeLists), and the uncompiled set differs per tree: the D3D12
backends exist only on Windows, the CUDA, WebGPU and HIP backends only where their toolkit is found (the hosted runners
have none of them), the benchmark suites not on the shipping presets.

A sweep of the four local trees (win-debug, linux-gcc-debug, linux-gcc-asan, linux-clang-tsan) against `git ls-files`
found 167 tracked sources that none of them compiles. With the 27 MATLAB `.m` scripts set aside (not C-family):

- the deliberate exclusions the row named: the installed-package consumer (its own CMake project), the HIP and Metal
  backends and their tests, the two geometry-convex probes;
- the GenMC model of the jobs counter protocol, compiled by GenMC itself;
- 100 sources in `runtime/examples` and `scripts/`: 39 behind the peer-bench options (Eigen/OpenBLAS, liblbfgs,
  L-BFGS-B, ILUPACK, CHOLMOD, SPQR, STRUMPACK), 27 built by peer scripts (`scripts/run_bench_*.sh` and friends), and
  34 built by nothing at all;
- 27 GPU-vendor measurement programs under `bench/` (CUDA, cuFFT, cuBLASLt, VkFFT), five of them generated kernels
  that another one includes, and the Shewchuk predicates reference.

## The guard

`scripts/check-source-routes.py --build <tree>` reads the tree's `compile_commands.json` (the source of each command and,
from its object path `CMakeFiles/<target>.dir/`, the targets that compile sources), `CMakeCache.txt` and
`CMakeFiles/<version>/CMakeSystem.cmake`, and judges every C-family source (`.c .cc .cpp .cxx .cu .mm .asm .S`) under
`engine tests tools runtime sandbox bench scripts`, tracked or untracked-but-not-ignored, against
`scripts/source-routes.json`:

| Route | The tree must show |
|---|---|
| (none) | a target compiles the source |
| `built_when` (`platform`, `option`, `target`) | the source is compiled exactly when every condition holds |
| `built_by` | the file exists and names the source; the tree does not compile it |
| `included_by` | the includer includes the source |
| `unrouted` (a ROADMAP row) | the row exists and is not Done, the source lies outside engine/, tests/, tools/ and sandbox/, and the tree does not compile it |

An option or target a condition names must be defined by a CMake file (a typo cannot read as OFF forever), every
pattern must match a source, every entry needs its reason, and two entries naming a source at the same rank fail.
The CTest `crd-check-source-routes` runs on every full configuration with a compile database (Ninja and Makefiles,
`CRD_MODULES` empty). On win-debug it takes about 5 s under ctest (0.3 s direct).

The committed manifest, as judged on win-debug: 1,808 sources, 1,669 compiled, 79 gated off (platform, options,
toolkits), 21 built outside the tree, 5 included, 34 unrouted; on linux-gcc-debug (mirror tree, retained benches on)
1,661 compiled, 84 gated off.

## Routes given rather than recorded

- **The 32 peer-free retained programs** (`runtime/examples`: the DSP, stats, tensor, interpolation, quadrature,
  wavelet, comms benches, `opt_scoreboard`, `prof_fft_kernel`) build under the new `CRD_BUILD_RETAINED_BENCHES`
  option. `linux-gcc-debug` and the presets that inherit it (`-scalar`, `-sse2`, `-public-checks`) turn it on, so a
  hosted lane compiles and links each one; win-tidy stays off it. The runtime module now declares the six hesap modules
  they link (`hesap-comms`, `-dsp`, `-fft`, `-interp`, `-quadrature`, `-wavelet`); `crd_verify_modules` and
  `test-module-selection.py` pass. They had rotted: MSVC `/W4 /WX` found discarded `[[nodiscard]]` build statuses
  in `bench_interp.cpp` (seven, now checked by `require_built`, which ends the run instead of timing an unbuilt
  interpolant) and the deprecated `getenv`/`fopen` and POSIX `clock_gettime` in `bench_sparse_tensor.cpp` and
  `bench_nn.cpp`; GCC's `-Werror` found nine implicit `usize`-to-`double` conversions, a zero-length `printf` format
  and two shadowed locals; strict tidy on the nine touched files found 18 more (forwarding references never forwarded,
  lower-case literal suffixes, multi-declarations, constant naming, and a conditional with identical arms in the sparse
  MTTKRP's mode order, `target == 1U ? 0U : 0U`, which was right by accident: both orders start with mode 0). All are
  repaired, nothing disabled; the three badly hand-formatted DSP benches are rewritten in the project's form.
  Verified: all 32 compile and link on win-debug (MSVC) and linux-gcc-debug (GCC 13.3, `-Werror`), the nine touched
  files are strict-tidy clean, and clang-format reports nothing on an added line.
- **The two geometry-convex probes** are removed, as both files' own headers asked once their question was answered:
  the EPA probe had already become "EPA: rotated hull-vs-hull self-consistency" in `test_epa.cpp`, and the GJK
  simplex-size histogram answered its v2c question (`docs/archive/systems/geometry-convex.md`). The CMakeLists comment
  records both.
- `bobyqa_difftest`, `cobyla_difftest` and `newuoa_difftest` were first among the peer-free ones; their link showed
  they need the NLopt reference objects (`scripts/setup-nlopt-ref.sh`), so they are unrouted below.

## Unrouted, owned by REPO.3

37 sources with no script or option that builds them, each needing a peer or vendor toolchain: all of `bench/` (the
five generated kernels excepted, `included_by` their profile program), `scripts/bench_gemm_v0d.cpp`,
`scripts/v14m_ggml_q8_bench.c`, and in `runtime/examples` `bench_batched` and `bench_fft_f32` (MKL),
`bench_bessel_vs_refs`, `bench_dist_boost`, `bench_interp_boost`, `bench_special_v12d`, `bench_special_vs_refs`
(Boost), `bench_ode_vs_refs` (Boost odeint and SUNDIALS KLU), `bench_dsp_sosfilt_vs_liquid.c`,
`bench_dsp_windows_vs_liquid.c` (liquid-dsp), and the three NLopt difftests. The WSL reference host has Boost,
liquid-dsp, FFTW, SUNDIALS and the NLopt reference build (`~/cerid-deps`); it has no MKL, cuFFT is on the Windows CUDA
install. REPO.3 owns giving each a checked route or retiring it with its board as the record; it is queued as the next
batch.

## Teeth

- **The 2026-10-17 defect itself:** `test_scene.cpp` removed from crd-ceir-tests' list, win-debug reconfigured: the
  guard fails with `tests/execution/ceir/test_scene.cpp: no target of this tree compiles it` and the CTest
  `crd-check-source-routes` fails; restored (byte-identical copy, touched), reconfigured, the CTest passes.
- **A new file nobody registered:** an untracked `tests/execution/ceir/test_orphan_probe.cpp` fails the guard without
  a reconfigure; removed, it passes.
- **The rules:** fifteen mutations of the guard, run supervised with the original restored after each (digest
  checked), each fail `test-repository-tools.py SourceRoutes` (12 cases): an uncompiled source without an entry, a
  compiled source whose condition fails, an ignored option or target condition, a route or includer that need not
  name the source, unrouted in a module directory, a Done owner, a dead pattern, two entries at one rank, ON read as
  false, any number read as true, a database without `output`, an undefined option name, an entry without a reason.

## Verification

- win-debug: reconfigured (the module graph verifies with the new DEPENDS), the 32 retained programs built with the
  option on, then the tree returned to the preset's default; `ctest -R "^crd-no-|^crd-check"`: 13 of 13 (the new guard
  included); the guard and the runbook guard pass (the build-gate runbook entry now names the guard, its command run
  once as written).
- linux-gcc-debug (mirror tree, `wsl-lane.sh configure`/`build`): the 32 programs build with `-Werror`;
  `crd-check-source-routes` passes in the tree and directly; the 12 unit cases pass on WSL Python.
- The guard on the other local trees with current compile databases: win-shipping and win-shipping-profile
  (`tests/bench` gated off, 81 gated), win-clang-cl (its `-- <source>` command form), win-clang-cl-shipping and
  win-asan pass; so does a configure-only `win-debug-scalar` tree with `-DCMAKE_DISABLE_FIND_PACKAGE_CUDAToolkit=ON`,
  the hosted shape (no CUDA targets, the scalar SIMD level; 1,659 compiled, 86 gated), deleted afterwards with the
  project-sync registrations byte-identical. No CMake file outside the WebGPU backend and its tests names
  `crd-kir-webgpu`, and the package installs only `crd_package_library` targets, none of the retained programs.
- `test-repository-tools.py` (117, 3 skipped locally for CMake and Ninja off PATH, as before), `test-module-selection.py`
  (12), `test-native-build-profiles.py` (7), `test-dev-workflow.py` (59), `check-ci-tiers.py`, `check-allman-braces.py`,
  strict tidy on the nine touched C++ files, clang-format `--dry-run` on their added lines, `check-master-plan.py` and
  `check-repository.py`: pass.

## Hosted evidence needed

- `crd-check-source-routes` passes on every Ninja lane that registers it, the Windows lanes (D3D12 compiled, CUDA,
  WebGPU and HIP absent), the Linux lanes, and the shipping lanes (`tests/bench` gated off).
- linux-gcc-debug, linux-gcc-debug-scalar, linux-gcc-debug-sse2 and linux-gcc-public-checks build the 32 retained
  programs with no warning.
- The repository jobs pass `test-repository-tools.py` with the `SourceRoutes` cases.

## Follow-up for an extra batch

The 37 unrouted sources above (REPO.3): a checked route per program (an option with its peer found, as the L-BFGS-B
difftest has, verified on the host that has the peer) or retirement with its board as the record. The same batch takes
`bench/gpu-compute/dbg_err.txt`, `dbg_out.txt` and `san_out.txt`, tracked debug output beside those programs.

Not in this guard, deliberately: the second half of the class, a test executable that is built but that no CTest runs
(`crd-jobs-schedcheck-tests` is one, `EXCLUDE_FROM_ALL` and unregistered). It needs the tree's link outputs against
`ctest --show-only=json-v1` and an owner per helper executable, and it is DIAG.11b clause (9)'s own item (dev.py and CI
ownership of specimens and mode activation), where the schedcheck gap is already listed.
