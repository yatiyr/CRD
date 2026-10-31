# 2026-10-31 — OWN.1: the biquad section step onto hesap-dsp, the walk orientation onto crd-math

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [OWN.1](../ROADMAP.md#slice-own.1), the fifth and sixth audit moves after the
[ISA-read and small-matrix move](2026-10-31-own-isa-read-and-small-matrix-move.md). Built, tested and measured on
`cerid-track-b`. The contract is
[capability ownership](../design/capability-ownership.md#the-biquad-step-and-walk-orientation-move).

## CI for the previous batches

No complete GitHub Actions run exists yet for any commit of this track: every run of the day up to e532ee73 was
cancelled by a newer push, and the run for d5cc2ec0 (41231558128) is still in progress. Its finished jobs: Repository
checks on both hosts pass; all five ARM64 lanes fail at the build, before any test, for two causes outside this
track's commits:

- `engine/foundation/core/src/cpu_features.cpp:1500` and `:1513` (97a9ba74, DIAG's machine identity): the AArch64
  branch declares `implementer` and `part`, which shadow the locals of the same names above it (`-Werror=shadow` on
  GCC and Clang, the four Linux ARM64 lanes);
- `tests/foundation/net/test_register_ids.cpp:51` (5eea2fa9, NET's register): a signed/unsigned `==` (MSVC C4389 as
  an error, win-arm64-debug).

Both belong to the rows whose tracks own those files, which fix CI first in their next batch. The ARM64 lanes therefore
have not yet compiled this track's `kernel_level.hpp` and `libm.hpp` change; it is read again on the first run past
those fixes.

A linux-gcc-release build of the previous attempt at this batch stopped in `durable_file.cpp:848` (an ignored
`ftruncate` result, `-Werror=unused-result`); be0adf3f on main had already fixed it, and this batch's rebase carries it.

## The biquad section step (capability `numerics.signal-processing`)

The audio module designed its biquad with hesap-dsp's RBJ formulas and ran its own direct-form II transposed loop, the
same recurrence `sosfilt_stream` wrote inline. hesap-dsp's `filter.hpp` now holds `BiquadState<T>` (moved from
`filtering.hpp`) and `biquad_step(bq, state, x)`, `constexpr`, scipy's order; `sosfilt_stream` cascades it and
`apply_biquad` runs it per channel, its `z1`/`z2` arrays replaced by one `BiquadState<f64>` per channel in the graph
renderer and the CEIR executor. The registry's entry gains the step's signature
(`b2 * x - ... a2 *`), so a third copy outside hesap-dsp is refused.

| Site | Oracle |
|---|---|
| `biquad_step` | bit-exact against the recurrence written out: 64 drawn stable sections × 512 samples, f32 and f64, outputs and both state words; an impulse response through a dyadic section checked at compile time (`test_filtering.cpp`) |
| `apply_biquad` | bit-exact against the audio copy for the four filter types × three cutoffs × three Qs over 3001 stereo frames, in one pass and cut into blocks of 256, 7, 1, 113 and 64: the bus and both channels' states (`test_audio_graph.cpp`) |
| `sosfilt_stream`, the graph and CEIR renders | the scipy reference vectors and the streaming-determinism cases of `test_filtering.cpp`, the 4-track gate and the CEIR-vs-graph bit-exact suite, unchanged |

## The walk orientation (capability `geometry.robust-predicates`, reviewed)

The Clough-Tocher walk's `fast_orient` was the 2D cross product written by hand. It now reads crd-math's
`cross(b - a, q - a)`: the same products in the same order, bit-exact over 65536 drawn triangles per precision with one
query in eight on the edge line (`test_clough_tocher.cpp`). It stays a non-robust sign on purpose, with geometry's
adaptive `orient2d` as the exact fallback scan, so the walk has no second predicate.

## Tidy on the touched headers

The strict tidy helper reported pre-existing findings in three of the touched headers: multiple declarations in one
statement (`filter.hpp`'s `zpk_to_sos`, `filtering.hpp`'s `lfilter`, `lfilter_zi` and `filtfilt`, and 23 in
`clough_tocher.hpp`), a nested conditional in `tf_to_zpk`, a capitalised local (`M` in `lfilter_zi`) and a lower-case
literal suffix. Each is fixed (declarations isolated in their order, the conditional an `if`); the code computes the
same values, which the suites above show.

## Before/after board

`runtime/examples/bench_biquad_and_orient.cpp` (a retained bench) times each moved site (`owner`) against the code it
replaced (`reference`, written out as it was) over the same 4096 inputs in one process, alternating which runs first;
[ab-board.py](../../scripts/ab-board.py) reads both arms from each run. linux-gcc-release, GCC 13.3, `cerid-track-b`
(8 vCPUs, Intel Xeon), CPU 3 pinned, one warm-up and 10 repetitions, band 2%, the host idle before and after
(`build-bench.py host`). Times are ns per item; speed ratio = reference over owner, median with its bootstrap 95%
interval.

| case | reference | owner | speed ratio | verdict |
|---|---|---|---|---|
| audio-biquad-stereo | 5.436 | 5.436 | 0.9999 [0.9986-1.001] | level |
| sosfilt-4-sections | 8.287 | 8.274 | 1.002 [1.001-1.005] | level |
| walk-orient-f64 | 2.008 | 2.000 | 1.003 [1-1.012] | level |

The first board read sosfilt-4-sections as 13% slower (0.8735). Its reference was not the copy: it took raw pointers
and the section count as a compile-time constant, so the compiler unrolled the cascade. The reference is now
`sosfilt_stream` as `filtering.hpp` wrote it (the same spans, the count read at run time), and the arms are level. The
unrolled form's 13% is a property of a fixed section count, not of the move.

## Verification (cerid-track-b, linux-gcc-debug, GCC 13.3)

- Tree build (619 steps after the rebase), no warnings.
- Test executables, each from a scratch directory: crd-hesap-dsp (104 cases), crd-audio (31), crd-hesap-interp (36),
  crd-hesap-comms (27): all pass.
- The forced-level arms (`ctest -R kernels-at`, 986 tests) pass.
- The guard CTests (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, with crd-repository-tools
  and crd-ci-platform: 22 tests) pass, among them crd-check-capability-owners (the new signature), crd-check-source-routes
  (the new board's route) and crd-check-include-budget.
- The strict tidy helper (`tidy-files.py`, LLVM 20.1) on every changed C++ file: clean.
- The machine was preempted once during the run; the job was restarted from the step it had reached.

## Remaining on OWN.1

The crd-compression module (decision 1: resources' `deflate` and the crdr zstd use move into a new foundation module),
and crd-perf's bundle publish onto crd-platform's durable files, which waits on crd-perf being able to link
crd-platform (crd-perf ships in the relocatable package and crd-platform does not yet).
