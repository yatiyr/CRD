# 2026-10-29 — SIMD-DISPATCH: the permute board's HPTT peer for every element type

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 5, the permute board against HPTT). Before:
[every f32 forward as an arm](2026-10-29-simd-dispatch-fft-forward-arms.md).

## CI first

- No completed run has a failed job. Push runs 40933364118 and 40934084386 were replaced while queued (no jobs).
  Push run 40929108987 (1ebe9dc4) is in progress, the first to run linux-gcc-arm64-asan after a1901325 and win-asan
  under the three-job bound; its finished jobs passed. Nothing cancelled or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 wait on a green complete-tier run; none has completed since.

## Correction

The permute arms note and the row said HPTT's f64 and complex rows needed a pinned HPTT, a download. They do not:
the local clone (external/hptt at 9425386, the commit external/PEER_ORACLES.md records) instantiates
`Transpose<float, double, FloatComplex, DoubleComplex>` and declares `create_plan` for all four. Only the Cerid-side
harness (`crd_hptt_bench.cpp`) was f32-only. A `cmake/pins.json` archive entry is optional cold reproducibility, to
ask for at the board.

## What landed

- **`crd-bench-hptt-peer`** (`runtime/examples/peers/hptt_peer.hpp`, `.cpp`): HPTT's plan of an element kind
  (f32, f64 and both complex types as re/im pairs; no `std::complex` crosses), row-major, alpha 1, beta 0, ESTIMATE,
  on a given OpenMP thread count; its version line names the sources by a SHA-256 prefix and the kernel.
- **`crd-hptt`** (`runtime/CMakeLists.txt`, option `CRD_BUILD_HESAP_VS_HPTT`, x64 Linux only, local only):
  HPTT's four sources from `CRD_HPTT_DIR` at the peer ISA, its AVX kernels only when that ISA has AVX (a compile
  check; HPTT's own CMakeLists forces `-march=native -mavx`), static, PIC, no IPO, `CRD_ISA_ROLE peer`. GCC's
  `-Wformat` on HPTT's one diagnostic `printf` is off for that target.
- **`bench_permute`** links the peer under the option (`CRD_BENCH_PERMUTE_HPTT`): an `hptt` row for every type and
  case, poisoned and compared bit for bit with the reference walk like the arms; in the rounds on one thread, in a
  block after the arms on N threads. `value()` states why alpha 1 keeps the bits (finite, no negative zero). The
  retained target stays peer-free in CI.
- `scripts/run_permute_bench.sh` configures the option with the checkout's clone (the WSL mirror leaves out the
  ignored `external/`); the external harness block is gone. `check-peer-headers.py` counts HPTT's headers as a
  header library (only its peer includes them), with its fixture; `source-routes.json` routes the peer source.

## Checks

The linux-gcc-release lane tree configured with the option (AVX found) and built `crd-bench-hptt-peer` without a
warning; objects-only GCC 13 compiles at the tree's `-Werror` of `bench_permute` with and without the define;
strict tidy (LLVM 20) clean on the three files through a scratch database; clang-format, Allman braces, the
PeerHeaders tests, the plan and repository checks. The bench does not run until the board.

## Left

The board at row close, HPTT's rows included.
