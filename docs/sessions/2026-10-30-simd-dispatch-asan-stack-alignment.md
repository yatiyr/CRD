# 2026-10-30 — SIMD-DISPATCH: the AVX-512 probe under GCC ASan, clang's double promotion and an empty memcmp

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (CI first). Before:
[fft f64 plans](2026-10-30-simd-dispatch-fft-f64-plans.md).

## CI first

Completed push run 41021189146 (4ab1cd90) and scheduled run 41034829979 (530d0f4e, jobs done, run still listed
queued; not cancelled) failed nine and five jobs. The fft `Stack overflow` cases and the sparse LinearOps' NaN payloads
are fixed in fa525bfe and 530d0f4e, after their commits. Three defects were new:

- **linux-clang-tsan** (push run 41059057789, e0c70766): every unit reaching `crd/math/libm.hpp` stopped at
  `const crd::f64 c = z;` in `fma_f32` (686 units, `-Wdouble-promotion`; f70b933d). GCC warns only in arithmetic, so
  its lanes passed. Now a `static_cast`; `units.cpp` and `clocks.cpp` pass with the tsan tree's clang command.
- **linux-gcc-asan**, `batch_gradient picks`: `same_bits` passed `want_rows.data()`, null for zero rows, to `memcmp`
  (UBSan `null pointer passed as argument 2`). An empty count now compares equal without the call, as the fuzz
  harness's helper does.
- **linux-gcc-asan**, `simd types: every level the host executes gives the portable level's bits` and
  `crd-math-kernels-at-avx512`: `SEGV on unknown address` in `d8_half_stores`, a compiler defect, below.

## The AVX-512 probe under GCC ASan ([TP-7](../third-party-defects.md#tp-7))

`d8_half_stores` (avx512 unit) zeroes `f64 t[8] = {}` with `vmovdqa64 zmm`. A 512-bit temporary realigns the frame to
64 bytes and GCC places `t` 64-aligned from that frame, but ASan's use-after-return frame (on by default) gives the
same layout a base that leaves `t` 32 bytes off: #GP. It needs Ubuntu's default `-fstack-protector-strong`.
`scripts/asan_stack_align_control.cpp` (no Cerid code) under SDE 10.13.1 `-skx`, WSL2:

| Compiler | -O0 / -O1 / -O2 | `-fno-stack-protector` | fake stack off | `-mstore-max=256 -mmove-max=256` |
|---|---|---|---|---|
| GCC 13.3.0 | faults (exit 132, `vmovdqa64`) | clean | clean | clean |
| GCC 12.4.0 | faults | clean | clean | clean |
| Clang 18 | clean (`t` 32-aligned, assumed 32) | | | |

GCC on x64 under ASan now builds with the two caps (`CMakeLists.txt`): use-after-return detection and the stack
protector stay on. In the avx512 probe unit the caps change one function, `d8_half_stores` (the zeroing becomes two
32-byte stores); at `x86-64` to `x86-64-v3` the objects are identical. The failing runners had AVX-512 (EPYC 9V74,
Xeon 8370C); EPYC 7763 runners skip the level, which is why earlier asan jobs passed. Hosted proof comes when the asan
job draws an AVX-512 runner. The dispatched host check's new `x64-asan-stack-alignment` leg runs the control under the
pinned SDE with every compiler of the image (GCC 14 included) and retires the caps when the default `g++` runs clean.

## linux-clang-tsan lost its runner

In 41021189146 the tsan job stopped at 08:54, 66 minutes into Test, with `The hosted runner lost communication with
the server`; no test had failed. In flight: `crd-hesap-direct-kernels-at-avx2` (21 min, 1,155 s whole in the green
run), `-at-avx512` (4 min) and the neural ODE case. The green tsan run of 530d0f4e ran the same arms overlapping at
`-j 4` with at least 10.8 of 15.6 GiB available (commit peak 11.0 GiB), so these tests' memory does not explain it; the
log holds no other sign. Recorded as a runner loss; the next tsan job on this fix is the check.

## Audit

Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none dispatched (41034829979 still listed).
Repository variables `FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only read): the
Firebase Test Lab lane switches on with the Android build of the portability block after SIMD-DISPATCH; no secrets
for the AMD or Mac lanes.
