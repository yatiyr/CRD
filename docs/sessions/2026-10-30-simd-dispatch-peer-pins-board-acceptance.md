# 2026-10-30 — SIMD-DISPATCH: the HPTT pin and the row-close boards' acceptance rule

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 10, the boards). Before:
[the dense factors' levers](2026-10-30-simd-dispatch-jobs-factor-picks.md).

## CI first

- No completed run has a failed job. Push run 40929108987 (1ebe9dc4) still runs four jobs (linux-clang-tsan,
  win-debug, win-asan, linux-gcc-asan); its 24 finished jobs passed. 40952979940 (6b9b7b2e) was replaced while queued
  (no jobs), so 40956523310 (d9dd6098) is the first win-asan run under the three-job bound. Nothing cancelled or
  dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has completed.

## User decisions recorded

- **Peer pins** (2026-10-30 00:05, approved; the operator downloaded both archives): HPTT at the commit of the earlier
  local clone, and Google Highway 1.4.0 (the user chose the latest release over the 1.3.0 the row named) as the
  measured design peer for the dispatch cost. Nothing else is approved for download.
- **Board acceptance** (2026-10-30 00:10, the user's order for the row-close boards, replacing "a family that loses to
  its peer is fixed before the row closes"): "if we are not losing too much ground from the older version of our
  engine it is fine since we got healthier and stronger and more correct ... DO NOT GRIND". Each family at each level
  is compared with its own pre-row board (same host, interleaved where the binaries allow) and with its peer. It is
  accepted when it has not regressed beyond the board's measured run-to-run noise (the noise and margin stated); a
  peer's lead is then recorded, not fixed. A regression beyond noise is fixed at its cause. Anything more is at most
  one focused attempt per target backed by a profile, a counter or a named algorithmic gap (the fft f32 1M forward
  against MKL is the named target), kept only when the board shows the win bit-exactly, otherwise the gap is
  recorded. No exploratory sweeps; the retune's thread sweep keeps a lever only on a clear saved win.

## What landed

- **`hptt` pin** (cmake/pins.json): springer13/hptt at 942538649b51…, the commit-addressed archive, SHA-256
  df623380…c86f97, 635,801 bytes, BSD-3-Clause (LICENSE.txt read from the archive), gated on
  `CRD_BUILD_HESAP_VS_HPTT`. The approved file was verified against the entry (`pins.py fetch --archive`) and seeded
  into `%USERPROFILE%/.cerid/archives`, the directory the drivers' `CRD_INPUT_ARCHIVES` names; the configure used it.
- **crd-hptt from the pin**: the root CMakeLists.txt refuses the option off x64 Linux and adds the package
  (download only); `runtime/CMakeLists.txt` builds the four sources from `hptt_SOURCE_DIR`. `CRD_HPTT_DIR` and the
  per-file digest are gone: the archive's hash is the provenance, and the peer's version line names the pinned commit.
- `scripts/run_permute_bench.sh` passes `CRD_INPUT_ARCHIVES` like the other peer drivers; the license manifest,
  pinned-inputs.md and the PinnedInputs tooling test name twelve packages.

## Checks

The linux-gcc-release lane tree configured with the option from the seeded archive ("using local archive",
"HPTT 942538649b51, AVX kernels: 1") and built `crd-bench-hptt-peer` and `bench_permute` without a warning;
check-pins, the license manifest and generated-sources checks, the tooling tests, the plan and repository checks. No
bench ran.

## Left

Highway 1.4.0's pin with its peer unit and the dispatch-cost board (check-pins refuses a pin no configure adds, so
the pin lands with its consumer); the other boards and the row-close session.
