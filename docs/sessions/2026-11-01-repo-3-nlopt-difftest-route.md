# 2026-11-01 — REPO.3: the Powell-method differential tests get a checked route

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [REPO.3](../ROADMAP.md#slice-repo.3), the `unrouted` programs of `scripts/source-routes.json` it owns. Run on
`cerid-track-b` (spot c4-standard-8, Ubuntu 24.04, GCC 13.3, no GPU) in a `linux-gcc-release` tree with the new
option and in `linux-gcc-debug`. Track B's scope order put [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0) and
[REPO.3d](../ROADMAP.md#slice-repo.3d) first; both wait on hosted CI and DIAG (below), so this batch took REPO.3's
local work.

## CI first

No GitHub Actions run has finished for any commit since 03571fa3. This is how the workflow's concurrency group
behaves for pushes, not a fault: `cancel-in-progress` is off for pushes, but GitHub keeps one running and one pending
run per group, and a newer push replaces the pending one. Run 41269105413 (b0b1b245) has been running since
03:25 UTC; every run queued behind it was replaced by the next push, and the one pending now (41274373728, de5e10b0,
whose parent is ee6e9651) covers all of MCP-ADAPT.0's commits and starts when 41269105413 finishes.

That running run has two failed jobs, neither from this track's open work. `win-vs` stopped on windows.h's `min` and
`max` macros in `filesystem.cpp` (`std::numeric_limits<i64>::max()`), a consequence of crd-platform no longer pulling
GLFW (aa91b382, this track); 9723ac75 (NOMINMAX in the three units that include windows.h) fixed it at 03:54 UTC,
after b0b1b245, and every windows.h include under `engine/foundation/platform` now has NOMINMAX before it.
`win-arm64-debug` stopped on MSVC's C4324 for `CheckStream` in `checksum.cpp`, which 921e19cc (track A) fixed.

## The route

The three differential tests of hesap-opt's Powell ports (`cobyla_difftest`, `newuoa_difftest`,
`bobyqa_difftest`, v7-p-2 to v7-p-4) compare each routine with NLopt's on identical inputs and whole minimizations
with their evaluation counts, bit for bit. Since the 2026-06-10 ports nothing built them:
`scripts/setup-nlopt-ref.sh` cloned NLopt's moving `master`, patched object files out of a copy of the library with
`ar d`, and left the compile to a hand-typed `g++` line. They now have a checked route.

- **Pinned oracle.** `cmake/pins.json` `nlopt`: v2.11.0 (88c424d4, the release current when the ports were made),
  the commit-addressed archive with its SHA-256 and size, MIT (COPYRIGHT and the three algorithms' COPYRIGHT files
  read from the archive), gated by `CRD_BUILD_HESAP_VS_NLOPT`. The license manifest and the pinned-inputs contract
  name it.
- **Option.** `CRD_BUILD_HESAP_VS_NLOPT` (root `CMakeLists.txt`, needs `CRD_BUILD_TESTS`) fetches the archive and
  enables C. `tests/numerics/hesap-opt/CMakeLists.txt` builds NLopt by its own CMake as an `ExternalProject`, so its
  install rules, rpath cache entries and options stay out of the tree: static, Release, at the baseline ISA, without
  its C++ (StoGO, AGS), LGPL (Luksan) and binding parts.
- **Oracle units.** `tests/numerics/hesap-opt/nlopt/<algorithm>_oracle.c` includes the reference source unmodified
  after its headers, with `static` defined empty, so the harness can call the file-scope routines, and adds the
  end-to-end shims that were heredocs in the old script (now with the allocation checks they lacked). The library's
  own member of that algorithm is never extracted, because the unit defines every symbol it would supply; no
  `ar d`. Each unit compiles under the tree's floating-point contract (`crd-simd-flags`), as NLopt's build compiles
  it (contraction off; newuoa.c without `_FORTIFY_SOURCE` on GCC 12 and later), without link-time optimization.
- **Tests.** The three programs moved from `runtime/examples` to `tests/numerics/hesap-opt/nlopt` (they are
  hesap-opt's tests, and `runtime/` is configured before the root's `enable_testing()`) and run as the `crd-nlopt-cobyla-difftest`, `crd-nlopt-newuoa-difftest`
  and `crd-nlopt-bobyqa-difftest` CTests (labels `hesap;nlopt-oracle`) under the tree's warnings.
  `scripts/source-routes.json` routes the directory by the option; `scripts/setup-nlopt-ref.sh` is gone.

## What the route found

The first run did not pass, and each finding was a real defect, fixed here.

1. **The ports formed pointers before their arrays.** f2c shifts each array argument before use (`--x;`,
   `a -= a_offset;`) so that `x[1]` is the first element. Forming such a pointer is undefined in C++, and with
   link-time optimization GCC saw the difftest's arrays and reported it (`-Werror=array-bounds`, "array subscript -1
   is outside array bounds of 'double[5]'", 20 sites in bobyqa.hpp). The new `crd/hesap/opt/f2c_view.hpp`
   (`detail::F2cView`, a base pointer and a bias) applies the shift inside each subscript: the 150 shifts of the
   three ports (COBYLA 24, NEWUOA 48, BOBYQA 78) became `const detail::F2cView x{x_base, 1};` with the parameter
   renamed, and the three drivers' `T* wp = w.data() - 1` became views as well. Every subscript stays verbatim, so the
   ports still read line for line against the reference; a raw use of a shifted name would no longer compile.
2. **BOBYQA's rescue read two variables GCC could not prove set.** `beta` and `denom` (`-Werror=maybe-uninitialized`
   at the `update` call) are set before every read: the first pass through `L80` has `knew == *kopt` and skips the
   update, and every later pass comes from the `goto L80` after their computation. They now start at zero, with that
   reasoning in a comment; the reference leaves them unset.
3. **NEWUOA's difftest failed 427 of 3,773 checks** (bigden, biglag, trsapp and the end-to-end runs). NEWUOA is the
   one reference that calls `sin`, `cos` and `atan`; the port calls crd-math's, and glibc's are not correctly
   rounded, so one angle a unit in the last place apart carried through the step. The oracle now takes those three
   from crd-math (`crd_oracle_sin`, `_cos`, `_atan`, defined in `newuoa_difftest.cpp`); `sqrt` and `fabs` are exact on
   both sides. With that alone every check passes, which confirms the cause and that the translation is exact. COBYLA
   and BOBYQA call only `sqrt` and `fabs`.

The three port headers and the Catch2 suites now point to the tests' new place (the COBYLA header named a
`CRD_BUILD_HESAP_VS_COBYLA` option that never existed).

## Evidence

- `linux-gcc-release` with `-DCRD_BUILD_HESAP_VS_NLOPT=ON` (`-O3`, LTO, `-Werror`): the three difftests and
  `crd-hesap-opt-tests` build without a warning. `ctest -L nlopt-oracle`: 3/3 pass; `cobyla_difftest` 2,050 checks,
  `newuoa_difftest` 3,773, `bobyqa_difftest` 3,045, none failed. `crd-hesap-opt-tests`: 3,832 assertions in 154 test
  cases pass.
- `linux-gcc-debug`: `crd-hesap-opt-tests` passes the same 3,832 assertions; the guard CTests
  (`^crd-check|^crd-no-|^crd-diag-coverage$|^crd-diag-seeded-faults$`) 20/20 pass.
- Teeth: before the math bridge the NEWUOA difftest failed 427 checks; before the views the BOBYQA difftest did not
  link.
- `check-source-routes.py` on both trees: PASS, 27 sources `unrouted` (30 before: the three difftests).
- Local validators: `check-pins.py` (15 packages), `check-generated.py` (the regenerated license manifest),
  `check-allman-braces.py`, `check-capability-owners.py`, `check-master-plan.py`, `check-repository.py`.

## Housekeeping

`bench/gpu-compute/dbg_err.txt`, `dbg_out.txt` and `san_out.txt` were compute-sanitizer and debug output committed on
2026-07-14 with the CUDA boards; they are removed.

## Remaining on REPO.3

- The other 27 `unrouted` sources: the CUDA and VkFFT programs under `bench/gpu-compute` and `bench/gpu-fft`, the
  Shewchuk predicates reference under `bench/reference`, `scripts/bench_gemm_v0d.cpp`, `scripts/v14m_ggml_q8_bench.c`,
  the MKL batched-GEMM bench and the two liquid-dsp peers in `runtime/examples`. Each gets a checked route or is
  retired with its board as the record; the GPU ones can be verified only on a host with the CUDA toolkit and a GPU.
- The pinned actions' Node deprecation lines and the cold Windows lane's LunarG Qt warning, read from the next
  complete run's logs.
- Mojibake found while checking `cmake/pins.json`: `tests/execution/ceir/test_plan.cpp` (24 sequences and a byte
  order mark) and `tests/gpu/gpu-context-vulkan/test_vulkan_gsplat.cpp` (one) were decoded as Windows-1252 and saved
  as UTF-8 at some point; they get repaired and a guard.
- Then REPO.3d (DIAG, then full remote CI evidence on the published revision) and REPO.3's own close.
