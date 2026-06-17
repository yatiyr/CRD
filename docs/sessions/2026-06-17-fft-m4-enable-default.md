# 2026-06-17 — FFT M4: enable the generated hierarchical f64 sub-FFT by default

> Module: `crd-hesap-fft` (v10), Fork A. Continues `2026-06-17-fft-m3-hierarchical-2048.md`.
> Memory: `project_v10_fft_plan`. The change is default-ON; emergency fallback `-DCRD_FFT_DISABLE_HIER`.

## TL;DR — M4 INCOMPLETE: the enable is implemented + change-verified; the full-suite DoD is still pending.
The M2 (4096=64×64) + M3 (2048=64×32) generated hierarchical sub-FFTs are wired as the **default** f64 forward
`execute_batched` path at n∈{2048,4096}, b==block_width (the 8M/16M four-step sub-FFTs); inverse and f32 fall through
to radix-8 (emergency disable `-DCRD_FFT_DISABLE_HIER`). **The change is verified** on all 4 Windows configs (build +
`[fft]` 72/12 pass + ASan-clean + LTCG + clang-tidy/WX) + WSL gcc (vs MKL ~1e-15). **But `[fft]`-filtered runs are
NOT the DoD** (CLAUDE.md §8: the guard tests — `crd-simd-emission-check`, `crd-no-std-math-check`, … — exist only as
`add_test` entries and never appear in a binary's filtered run). **`ctest --preset win-debug` is now GREEN —
3935/3935, guards included** (run directly on the built binaries; the one direct-run hiccup was the disassembly guard
needing `dumpbin`/vcvars, which passes via `run-ctest.bat`). The DoD bar (a green `ctest --preset` with guards on ≥1
config) is **met on win-debug**; the formal multi-config sweep (asan/shipping/tidy full ctest + Linux) is the
remaining close, blocked only at its `cmake --build` step by a pre-existing FFT-unrelated stale-PCH `C1853` (MSVC
updated to 14.50.357) — fix + commands in §Phase 4. Not committing until that formal close is done.

## Phase 1 — the flip (default-ON + emergency fallback)
`fft.hpp`: the 5 hier guards `#ifdef CRD_FFT_M2_HIER` → `#ifndef CRD_FFT_DISABLE_HIER` (default-ON; disable with
`-DCRD_FFT_DISABLE_HIER`). Dispatch unchanged: f64 / Forward / b==BB / n∈{4096,2048}. Verified both ways on WSL:
default → 1.2e-15 + faster; `-DCRD_FFT_DISABLE_HIER` → radix-8 (1.5e-15). No public-API change; no planner change
beyond the sub-FFT dispatch.

| case | selected kernel | reason |
|---|---|---|
| f64 forward 2048 (b=32) | `codelet64_stage1_fused_64x32` + `codelet32_batched` | M3, 1.26× |
| f64 forward 4096 (b=16) | `codelet64_stage1_fused_64x64` + `codelet64_batched` | M2, 1.24× |
| f64 inverse 2048/4096 | `execute_batched` (radix-8) | fallback (inverse codelets = future) |
| f32 forward/inverse any | `execute_batched` | fallback (Vec8f port = future) |
| other lengths / b≠BB | `execute_batched` | fallback |

## Phase 2 — golden / tolerance audit (CLEAN)
The hier path changes the *forward* output ~5e-16 vs radix-8 (different op order). No test/consumer pins forward
output to a stored golden or expects old-vs-new bit-identity:
| consumer/test | compares against | tolerance | status |
|---|---|---|---|
| `[fft]` forward vs brute DFT | numerical | 1e-12 | ✓ (hier matches at ~1e-15) |
| `[fft]` four-step vs radix-2 oracle (2^19..**2^23**) | numerical | 1e-12 | ✓ (extended to 8M to gate 4096) |
| round-trip ifft∘fft | numerical | 1e-13 | ✓ (fwd-hier∘inv-radix8 = identity) |
| run-twice determinism | same-path memcmp | exact | ✓ (hier is deterministic) |
| DCT/DST · rfft · NUFFT | tolerance | various | ✓ (small grids — don't reach the four-step) |
| any stored golden | — | — | none exist |
Test addition: `test_fft.cpp` four-step-vs-oracle loop extended `{2^19..2^22}` → `{2^19..2^23}` (+ alloc 1→2 GiB) so
2^23 gates the 4096 hier path (n1=4096) in CI; 2^21/2^22 already gated 2048 (n1=2048).

## Phase 3 — correctness (machine-eps, all green)
- win-debug `[fft]`: **72 assertions / 12 cases PASS** (fresh build, incl. the new 8M oracle).
- win-asan `[fft]`: **72/12 PASS** on a forced-fresh rebuild — **ASan-clean** (no UAF/OOB/alignment in the generated
  loads/stores/bbuf/twiddle).
- win-shipping `[fft]` (LTCG): **72/12 PASS** (forced-fresh).
- WSL gcc (the `m3_full`/`m4_verify` probes, default-ON): correct vs MKL at 4M/8M/16M = **1.1–1.3e-15**.

## Phase 4 — DoD status (HONEST)
- ✅ **win-debug `ctest --preset` GREEN — 3935/3935, GUARDS INCLUDED** (the DoD bar). The full suite ran on the
  built binaries; the lone direct-run "failure" was `crd-simd-emission-check` reporting **`dumpbin not on PATH`** —
  purely because I launched `ctest --preset` from a plain PowerShell without vcvars; **re-run via `run-ctest.bat`
  (vcvars) it PASSES** (`1/1 Passed`). The grep-based guards (`crd-no-std-math/sort/non-ascii/untagged-numeric/
  malloc`) all passed in the direct run. So win-debug = full suite + guards green.
- ✅ **FFT change verified on all 4 Windows configs (FFT scope):** win-debug (build + `[fft]` 72/12), win-asan
  (ASan-clean), win-shipping (LTCG), win-tidy (`fft.cpp` + generated `hier_codelets.hpp` clean under **clang-tidy
  strict + /WX**). WSL gcc: clean + correct vs MKL.
- ⏳ **Full multi-config sweep (win-asan/shipping/tidy full ctest + Linux) — the formal close, not yet run.** Its
  `cmake --build` step is BLOCKED by a pre-existing, FFT-unrelated **`C1853` stale-PCH** (the MSVC toolchain updated
  to 14.50.357 since the build dirs were last built ⇒ stale PCHs across `crd-cooker` + the runtime smokes — neither
  is ctest-registered, which is why `ctest --preset` itself runs fine). Fix for the user: clear the stale PCHs then
  run the sanctioned sweep —
  ```powershell
  Get-ChildItem build\win-* -Recurse -Filter cmake_pch.cxx.pch | Remove-Item   # also cmake_pch.cxx.obj
  .\scripts\per-slice-check.ps1 -Reconfigure -BuildJobs 8      # then scripts\full-sweep.ps1 for Linux
  ```
  A heavy rebuild × configs = the 14900K thermal-hazard sweep CLAUDE.md gates behind the host-stability ladder —
  deferred to the user (their compiler update, their hardware-risk call). Guards: my change touches only lower-layer
  raw-f64 FFT + uses `std::cos/sin` already present in the ctor — no guarded surface violated.

## Phase 5 — performance (canonical `bench_fft_vs_refs`, new default vs `-DCRD_FFT_DISABLE_HIER`)
Robust axis = interleaved within-process (thermal-cancelling); the canonical best-of-reps ran warm post-build ⇒
understates. Engine speedup OLD→NEW:
| n | interleaved (m4_verify / m3_full) | canonical best-of-reps | ×MKL OLD→NEW (canonical) |
|---|---|---|---|
| 8M | **1.13–1.21×** | 1.04–1.12× (warm) | ~0.88 → ~0.90–0.94 |
| 4M | ~1.15–1.24× | 1.158× | ~0.84 → **~0.99** (near parity) |
| 2M | ~1.05× | 1.050× | ~0.76 (n1=2048 only) |
| 16M | 1.06–1.08× (m3_full) | — | ~0.90 (4096-only, M2) |
Acceptance (8M ≥1.05×): **met** by the robust interleaved measures; no 4M/16M regression (both improve); 2M improves.
All machine-eps. Still BEATS PocketFFT/FFTW everywhere; 8M/4M close on MKL.

## Phase 6 — parity dashboard (f64)
| version | 2M | 4M | 8M | 16M |
|---|---|---|---|---|
| after 4 archaeology wins (committed) | 0.76 | ~0.84 | ~0.84 | ~0.85 |
| **M4 default (hier 2048+4096)** | ~0.75 (partial) | **~0.99** | **~0.90–0.94** | ~0.90 |
Gap closed vs the pre-archaeology 0.76 baseline: 8M 0.76→~0.92 ≈ **two-thirds**; 4M to near-parity.

## Phase 7 — commit decision: win-debug DoD met; finish the multi-config sweep, then commit.
The FFT enable is correct + change-verified, and **`ctest --preset win-debug` is green incl. guards (3935/3935)** —
the DoD bar on the primary config is met. Before commit, close the formal multi-config sweep: clear the stale
smoke/cooker PCHs (the `Get-ChildItem … cmake_pch.cxx.pch | Remove-Item` one-liner in §Phase 4) → `per-slice-check.ps1
-Reconfigure` (win-asan/shipping/tidy) → `full-sweep.ps1` (Linux, no PCH staleness). Each config is expected green
(the change is already verified per-config FFT-scope + ASan-clean + LTCG + tidy + gcc; win-debug shows the full suite
+ guards pass). Agents don't commit — proposed message below; the user runs the sweep + commit.

```
perf(hesap-fft): enable generated hierarchical f64 subFFT for 2048/4096

Enable the generated hierarchical f64 forward subFFT path for lengths 2048 and 4096
(default; disable with -DCRD_FFT_DISABLE_HIER). M3 validated 2048 = 64x32 at 1.26x and
M2 validated 4096 = 64x64 at 1.24x. Full f64 C2C improves across large powers of two:
8M ~1.13-1.21x faster (-> ~0.90-0.94x MKL), 4M near parity, 16M unchanged. Correctness
machine-eps vs MKL (~1e-15); round-trip + determinism clean; ASan/LTCG/clang-tidy clean.
Inverse and f32 remain on the execute_batched fallback pending later ports. The
four-step-vs-oracle test now covers 2^23 to gate the 4096 hier path.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

## Phase 8 — next fronts (after the DoD closes + commit)
A. f32 (Vec8f) port of the 2048/4096 hier codelets (f32 currently ~0.78× MKL; likely the biggest remaining win).
B. inverse hier codelets (round-trip perf + API symmetry).
C. planner/cost-model dispatch thresholds.
D. the last 8M f64 ~0.92→1.0 push.

## Files
- `engine/.../fft.hpp` (guards flipped, comments) · `engine/.../detail/hier_codelets.hpp` (generated) ·
  `tests/hesap-fft/test_fft.cpp` (oracle → 2^23) · `build/run_m4_verify.sh` · `build/run_m4_perf.sh`.
- ⚠ **The macro rename broke the old probe scripts**: `run_m2_8m.sh` / `run_m3_full.sh` pass `-DCRD_FFT_M2_HIER`,
  which is now a **no-op** ⇒ their "BASE" arm is hier-ON too (no longer a valid baseline). Any re-run for a
  BASE-vs-hier comparison must use **`-DCRD_FFT_DISABLE_HIER`** for the baseline (as `run_m4_perf.sh` correctly does).
