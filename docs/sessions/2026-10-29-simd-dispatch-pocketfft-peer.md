# 2026-10-29 — SIMD-DISPATCH: the pocketfft peer and the ARM64 level checks

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md);
follows [the tiled four-step](2026-10-29-simd-dispatch-tiled-four-step.md).

## CI first

Push run 40784988994 (92db578d) finished. linux-gcc-asan failed on the ISA guard's zstd `_c_loop` objects, which
`zstd-fast-huf-bmi2` (e17984e3) accepts, and on TP-5 (registered). win-asan's kernels-at failures are the UCRT fma
fixed by dd3d54c4; the job was cancelled at GitHub's 6-hour cap with 7,707 of 8,091 tests run, the cost 5fbf494d cut.
Every other job was traced in earlier batches. Push run 40826432863 (dd860168) is running. DIAG.11c and DIAG.12 stay
Needs CI: no completed run carries their hosted evidence yet.

Run 40826432863 (dd860168) failed three jobs before it finished.

- linux-gcc-release failed at the link of crd-hesap-tensor-tests. GCC's link-time inlining of the recursive MTTKRP
  oracle reported `fids[5]` past the CSF tree's five levels (-Warray-bounds). The oracle now takes its level as a
  template argument, so no instantiation goes past `CsfTree::kLevels`.
- Both ARM64 debug lanes now build. Each fails four tests: the ISA guard, the ELF build-id record, and the
  `__builtin_trap` SIGILL and FP-division SIGFPE crash captures. These are the next batch's first job.

## What landed

- **ARM64 level checks** (d75538c5). The CRD_CPU_LEVEL child case returned at once on ARM64. It now starts a child at
  portable and at neon (both must apply) and at sse2 (neon stays, another architecture's level), and the OS-view case
  checks that the detected level is neon and that the OS reports Advanced SIMD (`HWCAP_ASIMD`, `PF_ARM_NEON`). The
  forced-arm `[cpu-level-applied]` case already ran on the neon arm (CrdSimd.cmake), contrary to scout brief 1.
- **`crd-bench-pocketfft-peer`**: the only unit that includes `pocketfft_hdronly.h` (`examples/peers/pocketfft_peer`),
  built by `crd_add_bench_peer` at the host's ISA. `crd-fft-peers` carries only FFTW and MKL, so a stray include in a
  bench no longer compiles. The header's SHA-256 prefix and f64 vector length are in `isa()`, which both benches print.
- **Plans built once.** pocketfft's `c2c` and `dct` build their plan and buffer on every call (`POCKETFFT_CACHE_SIZE`
  defaults to 0), while Cerid, FFTW and MKL time execution only. The peer keeps a `pocketfft_c` and a `T_dcst23` plan
  and runs what those calls do for one contiguous array: copy the input to the output and execute in place. This
  raises pocketfft's number against the 2026-06 and 2026-07 boards. No thread pool (`POCKETFFT_NO_MULTITHREADING`):
  every board is one thread. The peer takes no `crd-simd-flags`, so GCC's default FP contraction applies, as in
  pocketfft's own build.
- **Validation.** Both benches compare pocketfft with FFTW, and Cerid with FFTW (FFT) or with its direct DCT (DCT
  ≤ 4096), and exit 1 when an error passes 32·eps·log2 n.
- **Warnings as errors.** Every FFT bench now builds with the tree's warnings as errors, since no peer header library
  reaches them. This found an unused parameter in `bench_nufft_vs_finufft`, now removed. GCC 13 reports
  `-Wnull-dereference` inside pocketfft's system header, on scratch arrays that would be null only at a length its
  plans never use. A diagnostic pragma covers only the header's own text.
- `Owned` moved to `peer_api.hpp`, for both peers.

## Checks

- WSL GCC 13 compile with the tree's warnings and `-Werror` at -O2: the peer at `-march=native`, and
  bench_fft_vs_refs, bench_dct_vs_refs, bench_fft_f32 and bench_nufft_vs_finufft at the baseline. Hosted CI does not
  build these programs.
- A scratch driver against FFTW 3.3.10 showed c2c and DCT-II agree to 2.6e-16 for n = 8 to 65,536. The DCT-II matches
  REDFT10's 2·Σ convention.
- win-debug: the Eigen peer and all 33 vs-reference benches build at /W4 /WX after the `Owned` move. crd-core-tests
  `[cpu]` passes. The source-route and include-budget guards pass. win-debug is back to
  `CRD_BUILD_HESAP_VS_REFERENCE=OFF`.
- No boards, following the fast-row order.
- Host event: check-allman-braces.py, which is pure single-threaded Python, failed in 3 of 45 native Windows runs.
  Each failure was a different impossible type error inside CPython (a `cell` where a `str` was, and `enumerate`
  reporting "'tuple' object is not an iterator"). Every rerun passed. `scripts/tsan-host-check.py` (20 rounds,
  pinned to CPUs 0 and 1 beside two busy loops) then found events in the Cerid-free controls: fiber 3 of 20, plain 4
  of 20, the jobs arm 2 of 20 and the unsanitized control 0. The host fault is back at a higher rate than on
  2026-10-27 (1 in 30). It now reaches native Windows processes. Local results are rerun once and recorded; the
  hosted lanes are the evidence.

## Left

The Boost peers, the no-peer-header check and PRINCIPLES §3.
