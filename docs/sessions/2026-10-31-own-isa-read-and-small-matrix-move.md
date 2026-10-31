# 2026-10-31 — OWN.1: the libm ISA read onto crd-core, the small-matrix routines onto crd-math

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [OWN.1](../ROADMAP.md#slice-own.1), the third and fourth audit moves after the
[seed-generator move](2026-10-31-own-seed-generators-move.md). Built, tested and measured on `cerid-track-b`. The
contract is [capability ownership](../design/capability-ownership.md#the-isa-read-and-small-matrix-move).

## CI for the previous batches

No complete GitHub Actions run exists yet for any commit of this track (20629f41 to 8e5a3d55): every run of the day
was cancelled by a newer push, and the one in progress (41215667488, c2645814) predates them. Its finished jobs
(Repository checks on both hosts, linux-gcc-shipping, linux-gcc-relwithdebinfo, linux-gcc-debug-scalar, the ARM64
debug lanes, win-vs, win-arm64-debug) pass. The own CI's store (`ci/ops.py build list`) holds no builds. Nothing to
trace to this track.

## The ISA read (capability `cpu.kernel-dispatch`)

`libm.hpp` was the one file outside crd-core that read compiler ISA macros: `__AVX2__` on MSVC and `__FMA__` on GCC
and Clang, to choose between the FMA instruction and its software bodies. `crd/core/kernel_level.hpp` now reports the
instruction set for code whose choice is not a kernel table's:

- `CRD_TARGET_X64`: 1 on x64, ARM64EC excluded (it compiles x64 code as AArch64);
- `CRD_TARGET_X64_FMA`: 1 when such a unit may issue FMA (GCC and Clang's `-mfma`, carried by the avx2 and avx512
  levels' x86-64-v3 and -v4; MSVC's `/arch:AVX2` and up, which define no `__FMA__`).

`libm.hpp` chooses its body from them and records it as `libm::kFmaBody` (`Native`: the compiler's builtin or the C
library's fma; `Intrinsic`: MSVC's in an FMA unit; `Software`: the integer and round-to-odd bodies). Its private macros
are undefined at the header's end, so the constant is what a test can read. `CRD_MATH_SCALAR_DISPATCH` became
`!CRD_KERNEL_LEVEL && CRD_TARGET_X64 && !CRD_TARGET_X64_FMA`; the condition it replaces also required `!__AVX2__` on
GCC and Clang, which differs only for code built with `-mavx2` but not `-mfma`, and no preset or level compiles that
(every x64 level is an x86-64-vN level, MSVC's `/arch:AVX2` brings both).

Oracle, at compile time: the SIMD-type probe (`tests/foundation/math/kernels/simd_level_probe.cpp`, one unit per
CPU level) refuses to compile unless crd-core reports FMA on exactly the avx2 and avx512 levels of x64 and none
elsewhere, and `kFmaBody` is `Software` on the other x64 levels, `Intrinsic` on MSVC's FMA levels and `Native`
otherwise; the soft-fma case of `test_simd_levels.cpp`, now guarded by crd-core's macros, asserts `Software` in general
code. The first build caught an error of this batch's own: the guard had read `CRD_LIBM_FMA_SOFT`, which `libm.hpp`
undefines at its end, so the soft-fma case had compiled out; `kFmaBody` replaced it and the case runs again
(7 assertions). CI's forced-level, MSVC, clang-cl and ARM64 lanes compile the probe on every target. The selected code
is the same, so no time board applies.

## The small-matrix routines (capability `math.fixed-size-linear-algebra`)

crd-math's `mat_types.hpp` gained Mat2 and Mat3 `determinant` (the cofactor expansion along the first column, in the
written order), `inverse`, Mat3 `try_inverse` and `inverse_transpose`, and Mat2 and Mat3 `solve` and `try_solve`
(Cramer's rule). The inverses divide by the value `determinant` returns, bit for bit, so a caller that tests the
determinant tests the value the inverse uses. The 3x3 solve takes each of its four determinants as `determinant` of
the transpose (the expansion along the first row), in which the four share the six 2x2 minors of the lower two rows of
`[m | b]`; `try_solve` gates on the determinant it divides by. `try_inverse` and `try_solve` refuse
`|det| < min_abs_det` and pass a NaN determinant, as `optimal_position` did.

| Site | Now | Oracle |
|---|---|---|
| `transform.hpp`: `det3`, `inverse_transpose_3`, the 2D attributes' determinant | `determinant`, `inverse_transpose` | bit-exact against the removed copies, 4096 draws with an exponent spread, f32 and f64 (`test_transform.cpp`) |
| `quadric.hpp`: `optimal_position`'s symmetric cofactor inverse | `try_inverse`, then the matrix-vector product | bit-exact and the same refusals, 4096 quadrics (sums of one to four planes and raw blocks) in f32 and f64, and the bound's edge (`test_quadric.cpp`) |
| `circumcenter.hpp`: both centres' Cramer's rule | `try_solve` on the system translated to the first vertex | 3D bit-exact; 2D within a declared tolerance of 2^-40 of the extent plus the circumradius near the origin; equidistance within it everywhere, offset by 1000 too (`test_circumcenter.cpp`) |

The 3D circumcentre copy expanded its determinants along the first row, as crd-math's 3x3 solve does, so it keeps its
bits; the 2D copy squared absolute coordinates and lost digits away from the origin, which the translated system
keeps. The degenerate
gate is now `|det|` below the smallest normal f64: zero, as before, or a determinant so small its reciprocal overflows,
where the copies returned infinities rather than the documented centroid. The circumcentres had no direct tests;
`test_circumcenter.cpp` adds the exact cases in both precisions, the collinear and coplanar fallbacks and the sweep,
and the Delaunay, Voronoi, Ruppert and tetrahedral-refinement suites pass unchanged.

Reviewed and left: geometry-convex's sub-distance barycentrics (signed volumes from crd-math's `dot` and `cross`),
CKIR's evaluator `mat_det` (the IR oracle's determinant rounded per operation at the node's dtype) and its
2D-Gaussian-splat emitter (Cramer's rule written into IR), both under decision 6.

## Before/after board

`runtime/examples/bench_small_matrix.cpp` (a retained bench) times each moved function (`owner`) against the copy it
replaced (`reference`, written out) over 4096 drawn inputs in one process, alternating which runs first;
[ab-board.py](../../scripts/ab-board.py) reads both arms from each run. linux-gcc-release, GCC 13.3, `cerid-track-b`
(8 vCPUs, Intel Xeon @ 2.80 GHz), CPU 3 pinned, 10 repetitions after one warm-up, band 2%, the host idle before and
after (`build-bench.py host`). Times are ns per item; speed ratio = reference over owner, median with its bootstrap 95%
interval.

| case | reference | owner | speed ratio | verdict |
|---|---|---|---|---|
| det3-f32 | 2.386 | 2.387 | 0.9981 [0.9958-1.001] | level |
| inverse-transpose-f32 | 6.728 | 6.733 | 0.9992 [0.9973-1.002] | level |
| quadric-optimal-f64 | 8.546 | 8.708 | 0.9817 [0.98-0.9842] | level |
| circumcenter-2d | 5.655 | 4.917 | 1.15 [1.14-1.151] | faster |
| circumcenter-3d | 12.44 | 12.59 | 0.989 [0.9877-0.9894] | level |

The determinant and the inverse-transpose are the copies' expressions. `optimal_position` pays about 1.8%, inside the
band: a general inverse forms all nine cofactors of the symmetric block where the copy formed six, the price of keeping
its bits through one owner routine. The 2D circumcentre gained 15%: the translated system needs no squared absolute
coordinates.

Two earlier boards shaped `solve`. Written as four calls of `determinant`, with the circumcentre testing the
determinant before solving, circumcenter-3d ran at 0.91 of the copy's speed. Taking the numerators from the cofactor
matrix and gating inside `try_solve` brought it to 0.95, and expanding along the first row, where Cramer's four
determinants share the lower rows' minors (27 multiplications against the adjugate's 33), brought it to parity and
made the 3D centre bit-exact, since the copy expanded that way too.

## Verification (cerid-track-b, linux-gcc-debug, GCC 13.3)

- Full tree build (1301 steps), no warnings; the rebuilds after each change, no warnings.
- Test executables, each from a scratch directory: crd-math (187 cases), crd-core (105), crd-geometry-primitives
  (205), crd-geometry-mesh-processing (143), crd-geometry-delaunay (112), crd-geometry-convex (207),
  crd-geometry-spatial (133), crd-geometry-bvh (82), crd-geometry-polygon (92), crd-geometry-mesh (42),
  crd-geometry-decomposition (21), crd-lod (18), crd-scene (300), crd-eylem (31), crd-eylem-rigid3d (35),
  crd-meshgen (11): all pass. crd-anim-tests fails one case when run from a scratch directory (it opens the fox
  glTF by a path relative to the working directory) and passes under CTest, which sets the directory.
- The forced-level arms (`ctest -R kernels-at`, 986 tests: every [kernels] case of crd-math, crd-geometry-primitives
  and the other families at each compiled level) pass, the soft-fma case among them.
- The guard CTests (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, with crd-repository-tools
  and crd-ci-platform: 22 tests) pass, among them crd-check-capability-owners, crd-check-source-routes (the new board's route) and
  crd-check-include-budget.
- The strict tidy helper (`tidy-files.py`, LLVM 20.1) on every changed C++ file: clean.
- `check-capability-owners.py`: no `pending` entry left for `cpu.kernel-dispatch` or
  `math.fixed-size-linear-algebra`; one file pending in all (crd-perf's bundle publish).

## Remaining on OWN.1

The audio biquad loop onto hesap-dsp; the crd-compression module; crd-perf's bundle publish onto durable files once
crd-perf can link crd-platform; the Clough-Tocher orientation. Each with its oracle and before/after board.
