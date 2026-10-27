# 2026-10-27 — SIMD-DISPATCH: the hesap-fft kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- Complete run 40515770381 (`31e2b9ce`, still running) failed win-release, win-shipping and win-shipping-profile on
  an MSVC LTCG crash linking `crd-hesap-direct-tests`; fixed in `14840123`
  ([direct note](2026-10-27-simd-dispatch-direct-kernels.md#ci-fix-msvc-ltcg-crash)). Every other completed run was
  cancelled with no failed job.
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) stay Needs CI.
  40515770381 predates `7eec1bf7` and fails three Windows lanes, so it serves neither; the next complete run at or
  after `14840123` is the candidate.

## Slice A: experiment gates

The 23 `CRD_FFT_*` experiment macros no build defines (the `DISABLE_*` escape hatches, the opt-in IP4, FS5, R4,
IP4_PAD and P1_NT paths, the M16-B/M17/M18/M19 POCs and the M16-B/M18 ablations) are resolved to their default
branches in `fft.hpp` and `hier_codelets.hpp` (a partial preprocessor; 10,963 lines go). Preprocessed `fft.hpp`
(MSVC `/EP`, assert file and line normalized) is token-identical to HEAD under avx2 release and debug, scalar and
`CRD_FFT_PROFILE`, apart from the hand deletions that followed: the FS5 members and their release, the empty
`m_abuf`/`m_bbuf`, the dead `m16b_*` locals and three f64 stage-1 codelets no path calls (64x16, 16x64, 32x64).
Four more unreferenced codelets (`codelet{32,64,128,256}_stage1_fused_sh`) are in the generated batched header and
go with its emitter edit. The plan's state no longer depends on a macro. `detail/aos_codelets.hpp` (raw AVX2,
included by nothing) is deleted with its manifest entry; its generator stays as the research tool
[fft-mkl-crush](../research/fft-mkl-crush.md) names. `CRD_FFT_PROFILE` and `prof::` stay until the kernel unit
decides where its counters live.

The M18 fused chain (f32 1M forward, 0.85x MKL against the gather fusion's 0.69x on the 2026-06 harness, never made
default; [design §18](../design/hesap_fft_generated_codelets.md)) went with the gates. The row names it as the
f32 1M lever for the row-close board.

## Slice B: the codelets in the level namespace

`batched_codelets_gen.hpp` and `codelets.hpp` (their generators, regenerated in WSL, the reference host that
reproduced both byte for byte before the edit) and the hand-maintained `hier_codelets.hpp` put their codelets in
`gen::inline CRD_MATH_LEVEL_NS` and `detail::inline CRD_MATH_LEVEL_NS`, so a kernel unit's copy can never merge with
another level's. The lane-scalar bodies call `crd::math::libm::fma` instead of `std::fma` (both single-rounded, the
same bits; `<cmath>` is gone), and the generator no longer emits the four unreferenced f64 `stage1_fused_sh`
codelets. The fft program passes on win-debug (the lane-scalar bodies). Left for the kernel unit: the lane-scalar
bodies build results with `Complex<T>{re, im}`, an inline member of `crd::hesap` outside the level namespace.

## CI read (slice C)

- No completed run since the last read has a new failed job. 40515770381 (`31e2b9ce`) is still running; its four
  failed jobs are the ones `b306e2ea` (linux-gcc-asan disk) and `14840123` (the MSVC LTCG crash) fixed. 40537963527,
  40533607455, 40531277767, 40530655387 and 40526567620 were cancelled with no failed job; 40539013278 (`c855a5a7`) is
  pending. Nothing was dispatched.
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) stay Needs CI;
  no complete run at or after `14840123` has finished.

## Slice C: the transform as a per-level kernel family

- `b59d0510`: the generated lane-scalar bodies stored each output as a `Complex<T>{re, im}` value, whose constructor
  is an inline `crd::hesap` function outside the level namespace; they now write `.re` and `.im` (2,224 stores, the
  same bits; regenerated in WSL).
- `crd_add_cpu_kernels(crd-hesap-fft NAME fft LEVELS portable sse2 avx2)`. `fft_kernels.hpp` holds only data: the
  `FftPlanState` the plan fills (sizes, raw tables, the arrays' and sub-plans' views), the small-N twiddles as plain
  lanes, the table (`execute`, `execute_batched`, both types) and the profile counters' declarations.
  `fft_dispatch.hpp` and `src/fft_kernel_dispatch.cpp` resolve the level once.
- `kernels/fft_kernels.cpp` is the engine moved out of `fft.hpp` (3,016 lines, `m_x` read as `plan.x`): one
  `Engine<T>` over the state, sub-plans run on the same level, the 22 `Complex` constructions became component
  stores, the four-step's prefetch, streaming store and fence take plain accesses on the portable level, and
  `small_n_codelets.hpp` lives in the level namespace on twiddles `src/fft.cpp` builds once with `crd::math::cos`
  and `sin` (the same values). Dead code went with the move: the uncalled `transpose`, `transpose_blocked`,
  `transpose_simd_c64`, `radix2_pass` and `butterfly_row`.
- `fft.hpp` is the plan: the ctor (now building the IP4-AoS tables on every build, so the plan no longer depends on
  the general code's ISA), `execute_reference`, and `execute`/`execute_batched` as one table call. It includes no
  codelet and no SIMD header; a new include-budget entry keeps `<immintrin.h>` out of it on GCC and Clang. A unit
  that includes only `fft.hpp` no longer parses the codelets; the three kernel units do.
- `CRD_FFT_PROFILE` stays: the counters are plain data in `src/fft.cpp`, the kernels read `__rdtsc`.
- Determinism: bit-identical run to run on a level; across levels tolerance-declared (ADR-0134 §7, ADR-0092 §1)
  while the avx2 level keeps the paths its `CRD_SIMD_HAS_AVX2` gates select.

Verification: `test_fft_kernels.cpp` ([kernels], with `crd-hesap-fft-kernels-at-<level>`): every level the host runs
against `execute_reference` at the test_fft.cpp tolerances for every size 2^0 to 2^21, both types and directions,
aligned and one word off, with guard words and a run-to-run bit check; the batched paths per column (N=8 even and
odd, N=16 inside and past the over-2 budget, the leaves, the hier sub-FFTs at their block width); zeros and
subnormals (no flush), one infinity or NaN; the process level. win-debug: the program passes (2,039 assertions in
the [kernels] cases, the rest 281), the eight fatal specimens pass (on win-debug the larger `FftPlan` collides with no request),
and `llvm-nm` on the three kernel objects shows no code symbol outside the level namespace.

Left of the fft item: one algorithm per size on every level (port the small-N lane-trick codelets and the four-step
AVX2 twiddle blocks to `Vec4d`/`Vec8f`, drop the remaining `CRD_SIMD_HAS_AVX2` gates) and then the bit-exact
declaration; the bench scripts (`run_bench_fft*.sh`, `run_bench_dct.sh`, `run_bench_nufft.sh`,
`prof_fft_kernel.sh`) link the fft, hesap and math archives for the row-close boards.


## CI read (slice D)

- No completed run has a new failed job. 40515770381 (`31e2b9ce`) is still running; its four failed jobs were fixed by
  `b306e2ea` and `14840123`. 40548005123, 40539013278, 40537963527, 40533607455, 40531277767, 40530655387 and
  40526567620 were cancelled before any job ran (zero jobs); 40548341193 (`18e78475`) is pending. Nothing dispatched.
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) stay Needs CI.

## Slice D: one algorithm per size on every level, bit-exact

- The f64 small-N lane-trick codelets (N 8, 16, 32 and the batched N 8 and 16) run on `Vec4d` (`fmaddsub`,
  `swap_pairs`, `concat_lo`/`concat_hi`) instead of raw AVX2, so `small_n_codelets.hpp` is compiled by every unit.
- The `CRD_SIMD_HAS_AVX2` gates around IP4-AoS, the deep-split and standalone-hier bands and the small-N paths are
  gone: their bodies already ran on `Vec4d`/`Vec8f` and the generated codelets.
- The four-step twiddle is one `Vec8f`/`Vec4d` body (`load_complex_deinterleaved`, the recurrence reseeded every 8
  (f32) or 32 (f64) steps) instead of two raw AVX2 blocks and a scalar loop, which is deleted; its f64 lane stores go
  through `store_complex`'s non-temporal branch. The f32 paired non-temporal scatter moved into
  `store_complex_pair_nt`. Only memory hints stay level-gated (`CRD_SIMD_HAS_SSE2`: prefetch, non-temporal stores, the
  fence; plain accesses on portable), and they change no value; hesap-fft has no `CRD_SIMD_HAS_AVX2` test left.
- The avx2 level keeps HEAD's operations and bits; the portable and sse2 levels change on the sizes those paths cover.
- Declaration: the family is bit-exact, portable the reference (`fft_kernels.hpp`, ADR-0134 §7, ADR-0092 §1), NaN
  payloads aside.

Verification: `test_fft_kernels.cpp` now also compares every level's output with portable's, bit for bit, for every
size 2^0 to 2^21, both types, directions and alignments, every batched shape and the zero/subnormal inputs. win-debug:
the [kernels] cases pass (2,915 assertions; the generated codelets run their lane-scalar edition there, the small-N
codelets and the four-step twiddle their `Vec` bodies), the other 281 pass. The optimized codelet bodies are proved by
the hosted release lanes and the `crd-hesap-fft-kernels-at-<level>` arms.

Left of the fft item: the f32 1M forward where MKL leads, and the bench scripts linking the fft archives.
