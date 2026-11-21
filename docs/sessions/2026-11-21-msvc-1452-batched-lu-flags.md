# 2026-11-21 — MSVC 14.52 dropped the avx512 batched LU's singular flags

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch)'s per-level bit-identity contract (ADR-0134 §7). No track
owned it: the network track's notes recorded it as outside NET-PATH.

## The failure

Since GitHub's windows-2025 image moved to MSVC 14.52.37025 (runs 44807121941 and 44861565940), win-release,
win-shipping and win-shipping-profile failed six hesap-tensor tests. "every level the host runs gives the scalar
tier's batched bits" counted 44 mismatches for f32 at `avx512` and `avx512+avx512-vnni`. The process self-check then
refused the avx512 table, so "the process runs the level its CPU selection resolves" saw avx2 and "tensor self-check:
every table the host runs gives the known answers" failed. f64, avx2 and every Linux lane passed.

## Reproduction and cause

- 14.51.36231 (the workstation's): win-release built under SDE `-spr` passes the test.
- 14.52.37025: Microsoft's own x64 packages from the VS 18 stable channel manifest (18.12.1), each payload checked
  against the manifest's SHA-256, into `~/.cerid/tools/msvc-14.52`. That x64 Desktop CRT package ships no DLL import
  libraries, so the workstation's 14.51 `msvcrt`/`msvcprt`/`vcruntime`/`oldnames` were appended after 14.52's own
  libraries. These libraries name DLL exports only and do not change code generation. win-release built under SDE
  reproduces exactly 44 mismatches at both avx512 variants.
- The checker now reports each routine, and the factorizations each part, separately. The 44 are all LU `info`:
  3 per matrix size from n = 2 to 16 and 10 at n = 22, 23 and 32. Values and pivots were exact.
- `lu_lanes` reads its singular-lane mask from a stored vector as `bm[q] != T(0)`. A set lane is all ones, which is a
  NaN, and `NaN != 0` is true. With that read, 14.52's optimised avx512 code lost singular flags; reading the lane's
  integer bits instead restores every one. The exact miscompiled step is not pinned down. n = 1 passed although it
  has singular lanes (inputs 5 and 17), so the loss is not simply "every set lane reads clear". Sizes 2 to 16 lost 3
  flags each and 22, 23 and 32 lost 10. Cholesky's `gm[q] == T(0)` reads the same kind of mask and gave the right
  answer; it now reads the bits as well. The SVD's convergence read compares stored 0 and 1 values, not a NaN mask.

## Fix

- `tensor_batched_kernels.cpp`: `lane_set` reads a stored mask lane as its integer bits (`__builtin_bit_cast`; a
  first version with `std::bit_cast` failed `crd-check-isa-isolation`, since the std template's instance is defined
  outside the level namespace). Both LU and Cholesky use it.
- `vec4f.hpp`'s scalar `select` fallback (CRD_SIMD_TARGET 0) read its mask lanes the same way, as `m[i] != 0.0F`. It
  now reads the bits.
- `test_tensor_kernels.cpp`: a failure now names the routine, the element type, LU or Cholesky and n. It also says
  which of values, `info` and pivots differ.

## Evidence

- 14.52 win-release under SDE `-spr`: the three failing tests pass. All 22 `[kernels]` cases pass (197,356
  assertions), and the whole suite passes natively (55 cases, 174,118 assertions). Before the fix the same tree gave
  the 44 mismatches (the teeth).
- 14.51 win-release: 117 hesap-tensor ctest cases pass, and `crd-check-isa-isolation` passes.
- win-debug-scalar: crd-math-tests builds; its 20 ctest cases pass.
- clang-tidy 22.1.8 (`tidy-files.py`) is clean on the three files, the scalar branch of `vec4f.hpp` included.
- `crd-check-diag-runbooks` and `crd-check-provider-symbols` fail in this one-target tree because their inputs were
  never built (tests of unbuilt targets; AWS-LC's `crypto.lib`). Hosted CI runs them on full trees.
- Speed: one integer test replaces one float compare per lane per column, outside the elimination loops.
- Hosted proof: manual change-tier run [44895545159](https://github.com/yatiyr/CRD/actions/runs/44895545159) on
  98617b60 (win-release with its avx512 SDE arms, win-shipping). win-shipping-profile and the scalar presets, the only
  builders of the `vec4f.hpp` fallback, run in the next complete run.
