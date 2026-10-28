# 2026-10-29 — SIMD-DISPATCH: the CPU level in the doctor, the host tuple and the board records

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md).

## CI first

Push run 40748658864 (f7304724) is still in progress (win-asan). Every failed job's first error is one already fixed at
HEAD: `std::memcpy` in fft_kernels.cpp (fcb3bb42, every GCC and Clang leg, ARM64 included), `ARM64_CNTVCT` in
clocks.cpp (fcb3bb42, windows-arm64) and the tidy names (b80f733a). Every other run of the last 15 was cancelled by
the concurrency group before a job started. Needs CI rows DIAG.11c and DIAG.12 still wait for a complete-tier run.

## What landed

- **One line.** `crd::format_cpu_selection` (crd-core, no heap) writes
  `cpu detected=<level> selected=<level> override=<status>`, then ` crt=<n> fma3=<0|1>` where the image's CRT
  dispatches, then ` extra=<features>`: the kernel features above the selected level's definition (none under an
  applied level; on the i9-14900K at detected avx2 they are aes, pclmulqdq, xsave, sha, gfni, vaes, vpclmulqdq,
  avx-vnni, hybrid, invariant-tsc). It returns the full length, so a cut line is visible. The cpu-level child and the
  forced-arm case print it; their parsers read its fields.
- **Doctor.** `DoctorReport::cpu` and `cpu_crt`; `diag.capabilities` adds `cpu_detected`, `cpu_selected`,
  `cpu_override`, and `crt_level` and `ucrt_fma3` where the CRT dispatches. Additive, so `cerid-diagnostics/1` and
  command schema 1 stay. The installed-package consumer checks the fields too.
- **Harness.** `host_tuple()` ends with the line of the child it spawns: `apply_cpu_override(cpu_selection(),
  CRD_CPU_LEVEL as set now)`, because a test may change the variable after the parent selected (the child CRT state
  is the child's and not named). `apply_cpu_override` rebuilds `kernel_features` from the usable set, so a parent that
  is itself an `-at-sse2` arm names a child's extra features correctly.
- **Boards.** The selection is a function of the CPUs a process may run on, their OS-saved state, `CRD_CPU_LEVEL` and
  the build's kernel cap, so `runtime/cpu_level_record` run under the measured program's variable and CPU list
  records exactly its selection, with no edit to the 96 programs. `--expect` stops the board when the level did not
  apply or exceeds the cap (it replaces run-lane-bench.sh's silent run at the detected level). run-lane-bench.sh
  calls it before each level; the five hand-linked boards (dtype, einsum, elementwise, permute, reduce) source
  `scripts/bench-level.sh` (`at_level`), which also replaces permute's own copy. crd-bench prints the line from a
  Catch2 listener. A diag-overhead record carries `cpu_level` (with `kernel_cap`); `diag-overhead.py --cpu-level`
  sets the variable for every process and fails a record that did not run it applied, one under the variable on a
  board without it, one with no level (a before arm's older build may name none) and a workload whose arms ran
  different selections; the rendered head names the level.

## Checks

win-debug: crd-core-tests `[cpu]` (1276 assertions), crd-perf-tests `[doctor]`, `[harness]` and the capabilities case,
`test-diag-overhead.py` (35, three new), `cpu_level_record` for none, sse2, a refused avx512, a set variable on a
detected board and usage, crd-bench's first line, and a diag-overhead record under sse2
(`"cpu_level":{"detected":"avx2","selected":"sse2","override":"applied","kernel_cap":"avx512","crt":1,"ucrt_fma3":false}`).
No teeth, WSL lane or boards (the fast-row order).
